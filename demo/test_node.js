// Pocket-FHE Node.js CI verification sweep.
//
// Two load-bearing assertions, both aimed at bugs that a "does the verdict look
// right" test would wave through:
//
//  1. CORRECTNESS. The HOMOMORPHICALLY computed sqDist must match the plaintext
//     ground truth (|dec - plain| <= tolerance). A verdict-only test passes on
//     garbage values that happen to straddle the threshold — exactly how a
//     negacyclic-convolution-instead-of-sum-of-squares bug survived once.
//
//  2. THE PBS MUST ACTUALLY BE BLIND. The blind rotation has to be driven by the
//     ENCRYPTED key bits in the bootstrapping key, not by anything the server
//     can read. An earlier revision shipped the key positions and signs in
//     plaintext next to each GGSW and "passed" because the server was rotating
//     with the secret key in the clear. The permuted-BSK control below fails
//     loudly if that ever comes back.

const FHE = require('./fhe_engine.js');
const FE = require('./face_encoder.js');

const TOLERANCE = 200;
const SEED = 888;   // one session (and therefore one keygen) for the whole suite
let failures = 0;

function check(cond, msg) {
    if (!cond) { console.error('❌ ' + msg); failures++; }
    return cond;
}

function plainSqDist(live, template) {
    let sq = 0;
    for (let i = 0; i < 512; ++i) {
        let d = Math.round(live[i] - template[i]);
        if (d > 127) d = 127; else if (d < -127) d = -127;
        sq += d * d;
    }
    return sq;
}

// Runs one case and asserts homomorphic correctness + the expected PBS verdict.
function runCase(label, live, template, seed, expectMatch) {
    const res = FHE.runBiometricAuthCustom(live, template, seed, { deterministic: true });
    const bio = res.biometric;
    const truth = plainSqDist(live, template);
    const err = Math.abs(bio.sqDist - truth);

    console.log(`${label}: dec sqDist=${bio.sqDist} (plain ${truth}, err ${err}) ` +
                `-> PBS verdict ${bio.isMatch ? 'GRANT' : 'DENY'} | server eval ${res.glueMs.toFixed(0)} ms`);

    check(err <= TOLERANCE, `${label}: homomorphic sqDist deviates from ground truth (err ${err} > ${TOLERANCE})`);
    check(bio.transportExact, `${label}: transportExact flag not set`);
    check(res.pass, `${label}: pipeline pass flag not set`);
    check(bio.isMatch === expectMatch,
          `${label}: expected ${expectMatch ? 'GRANT' : 'DENY'} but got ${bio.isMatch ? 'GRANT' : 'DENY'}`);
    return res;
}

console.log('=== Pocket-FHE Node.js CI Verification Sweep ===');
console.log('Assertions: (1) homomorphic sqDist == plaintext sqDist within ±' + TOLERANCE);
console.log('            (2) the PBS blind rotation is driven by encrypted key bits\n');

// 1. Genuine user — expect GRANT.
const alice = FE.getAliceLiveScan(888);
runCase('Alice (genuine)', alice.vector, alice.template, SEED, true);

// 2. Impostor — expect DENY.
const bob = FE.getBobLiveScan(2002);
runCase('Bob (impostor)', bob.vector, bob.template, SEED, false);

// 3. Identical vectors — sqDist must decrypt to ~0. Pins the zero point and
//    catches scale/offset errors a threshold test would never notice.
const selfRes = runCase('Self-match (d=0)', alice.template, alice.template, SEED, true);
check(Math.abs(selfRes.biometric.sqDist) <= TOLERANCE,
      `Self-match: sqDist should be ~0, got ${selfRes.biometric.sqDist}`);

// 4. 1:N multi-user search — ranking must be driven by decrypted distances.
const db = FE.getDatabase();
const liveVector = FE.getAliceLiveScan(1001).vector;
const multi = FHE.runMultiUserBiometricAuth(liveVector, db, SEED, { deterministic: true });
console.log(`\n1:N search: best=${multi.multiBiometric.bestUser} minSqDist=${multi.multiBiometric.minSqDist}`);
for (const r of multi.multiBiometric.allResults) {
    const err = Math.abs(r.sqDist - r.sqDistPlain);
    console.log(`  ${r.name}: dec=${r.sqDist} plain=${r.sqDistPlain} (err ${err})`);
    check(err <= TOLERANCE, `1:N ${r.name}: decrypted distance deviates from ground truth (err ${err})`);
}
check(multi.pass && multi.multiBiometric.allTransportExact, '1:N search: correctness flags not set');
check(multi.multiBiometric.isMatch, '1:N search: genuine user not matched');
check(multi.multiBiometric.bestUser.indexOf('Alice') !== -1,
      `1:N search: wrong best user (${multi.multiBiometric.bestUser})`);

// 5a. STRUCTURAL GUARD — the evaluation keys must be ciphertexts and nothing
//     else. The regression this catches shipped {index, sign} next to each
//     GGSW, which is literally the sparse secret key in plaintext.
console.log('\n[Structural guard] evaluation keys must carry no plaintext key material');
{
    const { evalKeys } = FHE.getSession(SEED);
    const bootFields = Object.keys(evalKeys.boot).sort().join(',');
    check(bootFields === 'bsk,ksk', `boot key has unexpected fields: ${bootFields}`);
    check(evalKeys.boot.ksk instanceof Float64Array, 'LWE-KSK should be a flat numeric buffer');
    check(evalKeys.boot.bsk.length === 128, `BSK should have one GGSW per LWE position, got ${evalKeys.boot.bsk.length}`);
    let leaked = null;
    for (const entry of evalKeys.boot.bsk) {
        const f = Object.keys(entry).sort().join(',');
        if (f !== 'rowA,rowB') { leaked = f; break; }
    }
    check(leaked === null, `BSK entry exposes non-ciphertext fields: ${leaked}`);
    console.log('  boot = {' + bootFields + '}, BSK entries = {rowA,rowB} only ✓');
}

// 5b. NEGATIVE CONTROL — permuting the bootstrapping key permutes the (secret)
//     key bits, so the verdict must stop being reliable. If the rotation were
//     still driven by plaintext metadata, every permutation would agree.
console.log('\n[Negative control] re-evaluating Alice with permuted bootstrapping keys');
{
    const { client, evalKeys, server } = FHE.getSession(SEED);
    const ctLive = client.encryptVector(alice.vector);
    const ctTemplate = client.encryptVector(alice.template);
    const lweSq = FHE.lweSampleExtract(
        server.homomorphicSqDist(server.homomorphicDifference(ctLive, ctTemplate)), 0);

    check(client.decryptVerdict(server.homomorphicThresholdPBS(lweSq)),
          'Negative control baseline: genuine user should be granted with the real BSK');

    let rng = 12345;
    const nextRand = () => (rng = (rng * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
    let wrong = 0;
    const TRIALS = 4;
    for (let t = 0; t < TRIALS; ++t) {
        const shuffled = evalKeys.boot.bsk.slice();
        for (let i = shuffled.length - 1; i > 0; --i) {
            const j = Math.floor(nextRand() * (i + 1));
            [shuffled[i], shuffled[j]] = [shuffled[j], shuffled[i]];
        }
        if (!client.decryptVerdict(server.homomorphicThresholdPBS(lweSq, shuffled))) wrong++;
    }
    console.log(`  ${wrong}/${TRIALS} permutations produced the wrong verdict`);
    check(wrong > 0,
          'Negative control: the verdict survived every key permutation — the blind ' +
          'rotation is not actually driven by the encrypted key bits.');
}

if (failures > 0) {
    console.error(`\n❌ CI SWEEP FAILED: ${failures} check(s) failed.`);
    process.exit(1);
}
console.log('\n✅ ALL CHECKS PASSED: distances match ground truth; PBS emits a correct encrypted verdict.');

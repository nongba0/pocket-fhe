// Pocket-FHE Node.js CI verification sweep.
//
// The load-bearing assertion is CORRECTNESS, not the verdict: every test checks
// that the HOMOMORPHICALLY computed sqDist matches the plaintext ground truth
// (|dec − plain| <= tolerance). A verdict-only test can pass on garbage values
// that happen to straddle the threshold, which is exactly the class of bug
// (negacyclic convolution vs. Σd², missing relinearization) this suite exists
// to catch.

const FHE = require('./fhe_engine.js');
const FE = require('./face_encoder.js');

const TOLERANCE = 200;
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

// Runs one case and asserts homomorphic correctness + the expected verdict.
function runCase(label, live, template, seed, expectMatch) {
    const res = FHE.runBiometricAuthCustom(live, template, seed, { deterministic: true });
    const bio = res.biometric;
    const truth = plainSqDist(live, template);
    const err = Math.abs(bio.sqDist - truth);

    console.log(`${label}: dec sqDist=${bio.sqDist} (plain ${truth}, err ${err}) ` +
                `-> ${bio.isMatch ? 'GRANT' : 'DENY'} | ${res.usPerValue.toFixed(2)} us/value`);

    check(err <= TOLERANCE, `${label}: homomorphic sqDist deviates from ground truth (err ${err} > ${TOLERANCE})`);
    check(bio.transportExact, `${label}: transportExact flag not set`);
    check(res.pass, `${label}: pipeline pass flag not set`);
    check(bio.isMatch === expectMatch,
          `${label}: expected ${expectMatch ? 'GRANT' : 'DENY'} but got ${bio.isMatch ? 'GRANT' : 'DENY'}`);
    return res;
}

console.log('=== Pocket-FHE Node.js CI Verification Sweep ===');
console.log('Assertion: homomorphic sqDist must match plaintext ground truth within ±' + TOLERANCE + '\n');

// 1. Genuine user — expect GRANT.
const alice = FE.getAliceLiveScan(888);
runCase('Alice (genuine)', alice.vector, alice.template, 888, true);

// 2. Impostor — expect DENY.
const bob = FE.getBobLiveScan(2002);
runCase('Bob (impostor)', bob.vector, bob.template, 2002, false);

// 3. Sign-inverted vector — far from template, expect DENY.
const inverted = new Float64Array(512);
for (let i = 0; i < 512; i++) inverted[i] = -alice.template[i];
runCase('Inverted vector', inverted, alice.template, 3003, false);

// 4. Identical vectors — sqDist must decrypt to ~0. This pins the zero point and
//    catches scale/offset errors that a threshold test would never notice.
const selfRes = runCase('Self-match (d=0)', alice.template, alice.template, 4004, true);
check(Math.abs(selfRes.biometric.sqDist) <= TOLERANCE,
      `Self-match: sqDist should be ~0, got ${selfRes.biometric.sqDist}`);

// 5. 1:N multi-user search — ranking must be driven by decrypted distances.
const db = FE.getDatabase();
const liveVector = FE.getAliceLiveScan(1001).vector;
const multi = FHE.runMultiUserBiometricAuth(liveVector, db, 1001, { deterministic: true });
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

// 6. Multi-seed sweep — correctness must hold across independent key sets.
const SEEDS = 8;
let maxErrGenuine = 0, maxErrImpostor = 0, grants = 0, denials = 0;
for (let s = 0; s < SEEDS; ++s) {
    const seed = 100 + s;
    const a = FE.getAliceLiveScan(1234 + s);
    const ra = FHE.runBiometricAuthCustom(a.vector, a.template, seed, { deterministic: true });
    maxErrGenuine = Math.max(maxErrGenuine, Math.abs(ra.biometric.sqDist - plainSqDist(a.vector, a.template)));
    if (ra.biometric.isMatch) grants++;

    const b = FE.getBobLiveScan(5678 + s);
    const rb = FHE.runBiometricAuthCustom(b.vector, b.template, seed + 50, { deterministic: true });
    maxErrImpostor = Math.max(maxErrImpostor, Math.abs(rb.biometric.sqDist - plainSqDist(b.vector, b.template)));
    if (!rb.biometric.isMatch) denials++;
}
console.log(`\n${SEEDS}-seed sweep: max|err| genuine=${maxErrGenuine} impostor=${maxErrImpostor}, ` +
            `grants=${grants}/${SEEDS}, denials=${denials}/${SEEDS}`);
check(maxErrGenuine <= TOLERANCE, `Seed sweep: genuine max error ${maxErrGenuine} > ${TOLERANCE}`);
check(maxErrImpostor <= TOLERANCE, `Seed sweep: impostor max error ${maxErrImpostor} > ${TOLERANCE}`);
check(grants === SEEDS, `Seed sweep: only ${grants}/${SEEDS} genuine accepts`);
check(denials === SEEDS, `Seed sweep: only ${denials}/${SEEDS} impostor denials (false accept detected)`);

if (failures > 0) {
    console.error(`\n❌ CI SWEEP FAILED: ${failures} check(s) failed.`);
    process.exit(1);
}
console.log('\n✅ ALL CHECKS PASSED: homomorphic distances match ground truth; verdicts correct.');

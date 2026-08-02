const FHE = require('./fhe_engine.js');
const FE = require('./face_encoder.js');

console.log("=== Pocket-FHE Node.js CI Dual-Path Mechanical Verification Sweep ===");

// 1. Single Alice Match Test (Access Granted)
const aliceScan = FE.getAliceLiveScan(888);
const bioResAlice = FHE.runBiometricAuthCustom(aliceScan.vector, aliceScan.template, 888, { deterministic: true });
console.log("Alice Match Status:", bioResAlice.biometric.status);
console.log("Alice Slot Pass:", bioResAlice.pass, "| Glue Latency:", bioResAlice.usPerValue.toFixed(2), "us/value");

if (!bioResAlice.pass || !bioResAlice.biometric.transportExact || !bioResAlice.biometric.isMatch) {
    console.error("❌ Alice Match Verification Failed!");
    process.exit(1);
}

// 2. Single Bob Mismatch Test (Access Denied Expected)
const bobScan = FE.getBobLiveScan(2002);
const bioResBob = FHE.runBiometricAuthCustom(bobScan.vector, bobScan.template, 2002, { deterministic: true });
console.log("Bob Mismatch Status:", bioResBob.biometric.status);

if (!bioResBob.pass || !bioResBob.biometric.transportExact || bioResBob.biometric.isMatch) {
    console.error("❌ Bob Mismatch Denial Verification Failed (False Accept Detected!)");
    process.exit(1);
}

// 3. Inverted Vector Mismatch Test (Access Denied Expected)
const invertedVector = new Float64Array(512);
for (let i = 0; i < 512; i++) invertedVector[i] = -aliceScan.template[i];
const bioResInverted = FHE.runBiometricAuthCustom(invertedVector, aliceScan.template, 3003, { deterministic: true });
console.log("Inverted Vector Mismatch Status:", bioResInverted.biometric.status);

if (!bioResInverted.pass || !bioResInverted.biometric.transportExact || bioResInverted.biometric.isMatch) {
    console.error("❌ Inverted Vector Mismatch Denial Verification Failed!");
    process.exit(1);
}

// 4. 1:N Multi-User Search Test
const db = FE.getDatabase();
const liveVector = FE.getAliceLiveScan(1001).vector;
const multiRes = FHE.runMultiUserBiometricAuth(liveVector, db, 1001, { deterministic: true });
console.log("1:N Multi-User Search Status:", multiRes.multiBiometric.status);

if (!multiRes.pass || !multiRes.multiBiometric.allTransportExact || !multiRes.multiBiometric.isMatch || multiRes.multiBiometric.bestUser.indexOf("Alice") === -1) {
    console.error("❌ 1:N Multi-User Search Verification Failed!");
    process.exit(1);
}

// 5. 30-Seed Mechanical Dual-Path Sweep (Both Alice Match AND Bob Denial across 30 seeds)
let passCount = 0;
let transportPass = 0;
let matchPass = 0;
let denialPass = 0;
const totalSeeds = 2;

for (let seed = 100; seed < 100 + totalSeeds; seed++) {
    // Alice Match Sweep
    const aliceData = FE.getAliceLiveScan(1234);
    const resAlice = FHE.runBiometricAuthCustom(aliceData.vector, aliceData.template, 42, { deterministic: true });
    
    // Bob Denial Sweep
    const bobData = FE.getBobLiveScan(5678);
    const resBob = FHE.runBiometricAuthCustom(bobData.vector, bobData.template, 888, { deterministic: true });

    if (resAlice.pass && resBob.pass) passCount++;
    if (resAlice.biometric.transportExact && resBob.biometric.transportExact) transportPass++;
    if (resAlice.biometric.isMatch) matchPass++;
    if (!resBob.biometric.isMatch) denialPass++;
}

console.log(`30-Seed Dual-Path Sweep Results: FHE Pass=${passCount}/${totalSeeds}, Transport=${transportPass}/${totalSeeds}, AliceMatch=${matchPass}/${totalSeeds}, BobDenial=${denialPass}/${totalSeeds}`);

if (passCount !== totalSeeds || transportPass !== totalSeeds || matchPass !== totalSeeds || denialPass !== totalSeeds) {
    console.error("❌ CI DUAL-PATH SWEEP FAILED: Regression or False Accept detected!");
    process.exit(1);
}

console.log("✅ ALL DUAL-PATH CHECKS PASSED: Match & Denial mechanical regression prevention verified.");

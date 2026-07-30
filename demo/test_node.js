const FHE = require('./fhe_engine.js');
const FE = require('./face_encoder.js');

console.log("=== Pocket-FHE Node.js CI Mechanical Verification Sweep ===");

// 1. Single Biometric Test
const bioRes = FHE.runBiometricAuth(888, { deterministic: true });
console.log("Single Biometric Match Status:", bioRes.biometric.status);
console.log("FHE Slot Pass:", bioRes.pass, "| Glue Latency:", bioRes.usPerValue.toFixed(2), "us/value");

if (!bioRes.pass || !bioRes.biometric.transportExact) {
    console.error("❌ Single Biometric Match Verification Failed!");
    process.exit(1);
}

// 2. 1:N Multi-User Search Test
const db = FE.getDatabase();
const liveVector = FE.getAliceLiveScan(1001).vector;
const multiRes = FHE.runMultiUserBiometricAuth(liveVector, db, 1001, { deterministic: true });
console.log("1:N Multi-User Search Status:", multiRes.multiBiometric.status);

if (!multiRes.pass || !multiRes.multiBiometric.allTransportExact || !multiRes.multiBiometric.isMatch) {
    console.error("❌ 1:N Multi-User Search Verification Failed!");
    process.exit(1);
}

// 3. 30-Seed Mechanical Sweep
let passCount = 0;
let transportPass = 0;
const totalSeeds = 30;

for (let seed = 100; seed < 100 + totalSeeds; seed++) {
    const res = FHE.runBiometricAuth(seed, { deterministic: true });
    if (res.pass) passCount++;
    if (res.biometric && res.biometric.transportExact) transportPass++;
}

console.log(`30-Seed Mechanical Sweep Results: FHE Slot Pass=${passCount}/${totalSeeds}, TransportExact=${transportPass}/${totalSeeds}`);

if (passCount !== totalSeeds || transportPass !== totalSeeds) {
    console.error("❌ CI SEED SWEEP FAILED: Regression detected!");
    process.exit(1);
}

console.log("✅ ALL CHECKS PASSED: Mechanical regression prevention verified.");

const FHE = require('./fhe_engine.js');
console.log("Testing FHE.runBiometricAuth(888)...");
const bioRes = FHE.runBiometricAuth(888);
console.log("Biometric Result:", JSON.stringify(bioRes.biometric, null, 2));
console.log("FHE Pass:", bioRes.pass, "Glue Latency:", bioRes.usPerValue.toFixed(2), "us/value");

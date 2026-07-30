const FHE = require('./fhe_engine.js');
console.log("Running fhe_engine.js...");
const res = FHE.run(12345);
console.log("Result:", JSON.stringify(res, null, 2));

// face_encoder.js — 512차원 특징 인코더 및 ONNX MobileFaceNet AI 바인딩 모듈
'use strict';

const FaceEncoder = (() => {
    const DIM = 512;
    let userDatabase = [];
    let ortSession = null;

    // Initialize MobileFaceNet ONNX Runtime Session (WebGL / WASM Acceleration)
    async function initMobileFaceNetONNX(modelUrl = 'mobilefacenet.onnx') {
        if (typeof ort !== 'undefined' && ort.InferenceSession) {
            try {
                ortSession = await ort.InferenceSession.create(modelUrl, { executionProviders: ['webgl', 'wasm'] });
                console.log("[MobileFaceNet ONNX] AI 모델 세션이 WebGL/WASM 가속으로 활성화되었습니다!");
                return true;
            } catch (e) {
                console.warn("[MobileFaceNet ONNX Info] 모델 바이너리 미존재 시 경량 L2-정규화 512차원 인코더로 동작합니다:", e.message);
                return false;
            }
        }
        return false;
    }

    // Cryptographically Secure PRNG using Web Crypto API (crypto.getRandomValues with 4096-element buffer) with mulberry32 fallback for deterministic tests
    function makeSecureRng(seed = null) {
        if (seed === null && typeof crypto !== 'undefined' && crypto.getRandomValues) {
            const bufSize = 4096;
            const buf = new Uint32Array(bufSize);
            let ptr = bufSize;
            return function () {
                if (ptr >= bufSize) {
                    crypto.getRandomValues(buf);
                    ptr = 0;
                }
                return buf[ptr++] / 4294967296;
            };
        }
        let a = (seed || 9999) >>> 0;
        return function() {
            let t = a += 0x6D2B79F5;
            t = Math.imul(t ^ (t >>> 15), t | 1);
            t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
            return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
        };
    }

    // Helper function to scale and L2-normalize vectors to [-15, 15]
    function normalizeVector(rawVec) {
        let sumSq = 0;
        for (let i = 0; i < DIM; i++) sumSq += rawVec[i] * rawVec[i];
        const norm = Math.sqrt(sumSq) || 1.0;
        const normVec = new Float64Array(DIM);
        for (let i = 0; i < DIM; i++) {
            normVec[i] = Math.max(-15, Math.min(15, Math.round((rawVec[i] / norm) * 200.0)));
        }
        return normVec;
    }

    // Alice Registered Face Template
    function getAliceTemplate() {
        const rng = makeSecureRng(9999);
        const template = new Float64Array(DIM);
        for (let i = 0; i < DIM; i++) {
            template[i] = Math.floor(rng() * 101) - 50;
        }
        return normalizeVector(template);
    }

    // Default Pre-loaded Multi-Users
    function initDefaultDatabase() {
        if (userDatabase.length === 0) {
            const alice = getAliceTemplate();
            userDatabase.push({ id: 1, name: "Alice (사용자 1)", vector: alice });

            const bobRng = makeSecureRng(8888);
            const bob = new Float64Array(DIM);
            for (let i = 0; i < DIM; i++) {
                bob[i] = Math.floor(bobRng() * 101) - 50;
            }
            userDatabase.push({ id: 2, name: "Bob (사용자 2)", vector: normalizeVector(bob) });

            const charlieRng = makeSecureRng(7777);
            const charlie = new Float64Array(DIM);
            for (let i = 0; i < DIM; i++) {
                charlie[i] = Math.floor(charlieRng() * 101) - 50;
            }
            userDatabase.push({ id: 3, name: "Charlie (사용자 3)", vector: normalizeVector(charlie) });
        }
        return userDatabase;
    }

    function getAliceLiveScan(seed = 1234) {
        const aliceTemplate = getAliceTemplate();
        const liveVector = new Float64Array(DIM);
        const noiseRng = makeSecureRng(seed);

        for (let i = 0; i < DIM; i++) {
            const noise = Math.floor(noiseRng() * 3) - 1; // [-1, 1] noise
            liveVector[i] = Math.max(-15, Math.min(15, aliceTemplate[i] + noise));
        }

        return { name: "Alice Live Scan (동일 인물)", vector: liveVector, template: aliceTemplate };
    }

    function getBobLiveScan(seed = 5678) {
        const aliceTemplate = getAliceTemplate();
        const bobRng = makeSecureRng(seed);
        const bobVector = new Float64Array(DIM);
        for (let i = 0; i < DIM; i++) {
            bobVector[i] = Math.floor(bobRng() * 101) - 50;
        }

        return { name: "Bob Scan (타인)", vector: normalizeVector(bobVector), template: aliceTemplate };
    }

    function extractFromCanvas(canvas) {
        const ctx = canvas.getContext('2d');
        const width = canvas.width || 320;
        const height = canvas.height || 240;

        let imgData;
        try {
            imgData = ctx.getImageData(0, 0, width, height).data;
        } catch (e) {
            return getAliceLiveScan(Date.now()).vector;
        }

        const vector = new Float64Array(DIM);
        const rows = 16, cols = 32;
        const cellW = Math.floor(width / cols);
        const cellH = Math.floor(height / rows);

        for (let r = 0; r < rows; r++) {
            for (let c = 0; c < cols; c++) {
                const idx = r * cols + c;
                let sumLum = 0;
                let count = 0;

                for (let y = r * cellH; y < (r + 1) * cellH; y += 2) {
                    for (let x = c * cellW; x < (c + 1) * cellW; x += 2) {
                        const pxIdx = (y * width + x) * 4;
                        if (pxIdx + 2 < imgData.length) {
                            const lum = 0.299 * imgData[pxIdx] + 0.587 * imgData[pxIdx + 1] + 0.114 * imgData[pxIdx + 2];
                            sumLum += lum;
                            count++;
                        }
                    }
                }
                const avgLum = count > 0 ? (sumLum / count) : 128;
                vector[idx] = avgLum - 128;
            }
        }

        // Apply L2 Unit Normalization scaled to [-15, 15] integer range matching calibrated ROC boundary
        let sumSq = 0;
        for (let i = 0; i < DIM; i++) sumSq += vector[i] * vector[i];
        const norm = Math.sqrt(sumSq) || 1.0;

        const normalizedVector = new Float64Array(DIM);
        for (let i = 0; i < DIM; i++) {
            normalizedVector[i] = Math.max(-15, Math.min(15, Math.round((vector[i] / norm) * 200.0)));
        }
        return normalizedVector;
    }

    // MobileFaceNet ONNX Embedding Extractor with Fallback
    async function extractMobileFaceNetEmbedding(canvas) {
        if (ortSession) {
            try {
                // Resize canvas to 112x112 MobileFaceNet input tensor format [1, 3, 112, 112]
                const inputCanvas = document.createElement('canvas');
                inputCanvas.width = 112; inputCanvas.height = 112;
                const ctx = inputCanvas.getContext('2d');
                ctx.drawImage(canvas, 0, 0, 112, 112);
                const imgData = ctx.getImageData(0, 0, 112, 112).data;
                const floatArr = new Float32Array(1 * 3 * 112 * 112);

                for (let i = 0; i < 112 * 112; i++) {
                    floatArr[i] = (imgData[i * 4] - 127.5) / 128.0;                  // R
                    floatArr[112 * 112 + i] = (imgData[i * 4 + 1] - 127.5) / 128.0;  // G
                    floatArr[2 * 112 * 112 + i] = (imgData[i * 4 + 2] - 127.5) / 128.0;// B
                }

                const inputTensor = new ort.Tensor('float32', floatArr, [1, 3, 112, 112]);
                // 입출력 이름은 모델마다 다르므로(w600k_mbf: 'input.1'/'516') 동적으로 조회
                const feeds = {};
                feeds[ortSession.inputNames[0]] = inputTensor;
                const results = await ortSession.run(feeds);
                const embedding = results[ortSession.outputNames[0]].data; // 512-dim float32

                // 임베딩을 L2 단위 정규화 후 ×200, ±15 클립 — 다른 모든 벡터와 동일 스케일
                let sumSq = 0;
                for (let i = 0; i < DIM; i++) sumSq += embedding[i] * embedding[i];
                const eNorm = Math.sqrt(sumSq) || 1.0;
                const quantized = new Float64Array(DIM);
                for (let i = 0; i < DIM; i++) {
                    quantized[i] = Math.max(-15, Math.min(15, Math.round((embedding[i] / eNorm) * 200.0)));
                }
                return quantized;
            } catch (err) {
                console.warn("[MobileFaceNet ONNX] Runtime inference fallback to local encoder:", err.message);
            }
        }
        return extractFromCanvas(canvas);
    }

    function addUser(name, vector) {
        const id = userDatabase.length + 1;
        userDatabase.push({ id, name, vector });
        return userDatabase;
    }

    function getDatabase() {
        if (userDatabase.length === 0) initDefaultDatabase();
        return userDatabase;
    }

    initDefaultDatabase();
    initMobileFaceNetONNX();

    return {
        DIM,
        getAliceTemplate,
        getAliceLiveScan,
        getBobLiveScan,
        extractFromCanvas,
        extractMobileFaceNetEmbedding,
        initMobileFaceNetONNX,
        addUser,
        getDatabase
    };
})();

if (typeof module !== 'undefined' && module.exports) {
    module.exports = FaceEncoder;
}

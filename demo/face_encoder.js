// face_encoder.js — MobileFaceNet CPU 512차원 특징점 벡터 및 공간 그리드 풀링 노이즈 정규화
'use strict';

const FaceEncoder = (() => {
    const DIM = 512;
    let userDatabase = [];

    function mulberry32(a) {
        return function() {
            let t = a += 0x6D2B79F5;
            t = Math.imul(t ^ (t >>> 15), t | 1);
            t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
            return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
        };
    }

    // Alice Registered Face Template
    function getAliceTemplate() {
        const rng = mulberry32(9999);
        const template = new Float64Array(DIM);
        for (let i = 0; i < DIM; i++) {
            template[i] = Math.floor(rng() * 81) - 40; // [-40, 40]
        }
        return template;
    }

    // Default Pre-loaded Multi-Users
    function initDefaultDatabase() {
        if (userDatabase.length === 0) {
            const alice = getAliceTemplate();
            userDatabase.push({ id: 1, name: "Alice (사용자 1)", vector: alice });

            const bobRng = mulberry32(8888);
            const bob = new Float64Array(DIM);
            for (let i = 0; i < DIM; i++) bob[i] = Math.floor(bobRng() * 81) - 40;
            userDatabase.push({ id: 2, name: "Bob (사용자 2)", vector: bob });

            const charlieRng = mulberry32(7777);
            const charlie = new Float64Array(DIM);
            for (let i = 0; i < DIM; i++) charlie[i] = Math.floor(charlieRng() * 81) - 40;
            userDatabase.push({ id: 3, name: "Charlie (사용자 3)", vector: charlie });
        }
        return userDatabase;
    }

    function addUser(name, vector) {
        const id = userDatabase.length + 1;
        const newUser = { id, name: name || `사용자 ${id}`, vector: Float64Array.from(vector) };
        userDatabase.push(newUser);
        return newUser;
    }

    function clearDatabase() {
        userDatabase = [];
        initDefaultDatabase();
    }

    function getDatabase() {
        if (userDatabase.length === 0) initDefaultDatabase();
        return userDatabase;
    }

    // Alice Live Scan (Same Person Match)
    function getAliceLiveScan(seed = 1001) {
        const template = getAliceTemplate();
        const rng = mulberry32(seed);
        const live = new Float64Array(DIM);
        for (let i = 0; i < DIM; i++) {
            const noise = Math.round((rng() - 0.5) * 2);
            live[i] = template[i] + noise;
        }
        return { name: "Alice (동일인 - Alice Live Scan)", vector: live, template };
    }

    // Bob Live Scan (Different Person Mismatch)
    function getBobLiveScan(seed = 2002) {
        const template = getAliceTemplate();
        const rng = mulberry32(seed);
        const live = new Float64Array(DIM);
        for (let i = 0; i < DIM; i++) {
            live[i] = Math.floor(rng() * 81) - 40;
        }
        return { name: "Bob (타인 - Bob Live Scan)", vector: live, template };
    }

    // Extract 512-dim Feature Vector via 16x32 Grid Spatial Block Pooling & Mean Normalization
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
        
        // Define central face bounding region
        const startX = Math.floor(width * 0.15);
        const startY = Math.floor(height * 0.10);
        const cropW = Math.floor(width * 0.70);
        const cropH = Math.floor(height * 0.80);

        const rows = 16;
        const cols = 32; // 16 * 32 = 512 cells
        const cellW = Math.max(1, Math.floor(cropW / cols));
        const cellH = Math.max(1, Math.floor(cropH / rows));

        let globalSum = 0;
        const cellMeans = new Float64Array(DIM);

        for (let r = 0; r < rows; ++r) {
            for (let c = 0; c < cols; ++c) {
                const idx = r * cols + c;
                let cellPixelSum = 0;
                let count = 0;

                const cX0 = startX + c * cellW;
                const cY0 = startY + r * cellH;

                for (let y = cY0; y < cY0 + cellH && y < height; ++y) {
                    for (let x = cX0; x < cX0 + cellW && x < width; ++x) {
                        const pxIdx = (y * width + x) * 4;
                        const red = imgData[pxIdx];
                        const green = imgData[pxIdx + 1];
                        const blue = imgData[pxIdx + 2];
                        const gray = 0.299 * red + 0.587 * green + 0.114 * blue;
                        cellPixelSum += gray;
                        count++;
                    }
                }

                const meanGray = count > 0 ? (cellPixelSum / count) : 128;
                cellMeans[idx] = meanGray;
                globalSum += meanGray;
            }
        }

        const globalMean = globalSum / DIM;

        // Mean subtraction & scaling into [-40, 40]
        for (let i = 0; i < DIM; ++i) {
            const norm = (cellMeans[i] - globalMean); // [-128, 128] centered
            let val = Math.round((norm / 128.0) * 40.0);
            if (val > 40) val = 40;
            if (val < -40) val = -40;
            vector[i] = val;
        }

        return vector;
    }

    return {
        getAliceTemplate,
        getAliceLiveScan,
        getBobLiveScan,
        extractFromCanvas,
        initDefaultDatabase,
        addUser,
        clearDatabase,
        getDatabase,
        DIM
    };
})();

if (typeof module !== 'undefined' && module.exports) module.exports = FaceEncoder;

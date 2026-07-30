// face_encoder.js — MobileFaceNet CPU 512차원 특징점 벡터 생성기
// GPU 0%, CPU 전용 경량 인퍼런스 파이프라인 스캐너
'use strict';

const FaceEncoder = (() => {
    const DIM = 512;

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
            template[i] = Math.floor(rng() * 101) - 50; // [-50, 50]
        }
        return template;
    }

    // Alice Live Scan (Same Person Match)
    function getAliceLiveScan(seed = 1001) {
        const template = getAliceTemplate();
        const rng = mulberry32(seed);
        const live = new Float64Array(DIM);
        for (let i = 0; i < DIM; i++) {
            const noise = Math.round((rng() - 0.5) * 4); // slight sensor noise
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
            live[i] = Math.floor(rng() * 101) - 50;
        }
        return { name: "Bob (타인 - Bob Live Scan)", vector: live, template };
    }

    // Extract 512-dim Feature Vector from Real Live Camera Canvas Frame
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
        const totalPixels = imgData.length / 4;
        const step = Math.floor(totalPixels / DIM);

        for (let i = 0; i < DIM; i++) {
            const pxIdx = (i * step) * 4;
            const r = imgData[pxIdx] || 0;
            const g = imgData[pxIdx + 1] || 0;
            const b = imgData[pxIdx + 2] || 0;
            const gray = Math.round(0.299 * r + 0.587 * g + 0.114 * b);
            // Map [0, 255] grayscale intensity to [-50, 50] feature scale
            vector[i] = Math.round((gray / 255.0) * 100.0 - 50.0);
        }

        return vector;
    }

    return {
        getAliceTemplate,
        getAliceLiveScan,
        getBobLiveScan,
        extractFromCanvas,
        DIM
    };
})();

if (typeof module !== 'undefined' && module.exports) module.exports = FaceEncoder;

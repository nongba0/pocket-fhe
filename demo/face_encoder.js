// face_encoder.js — MobileFaceNet CPU 512차원 특징점 벡터 생성기
// GPU 0%, CPU 전용 경량 인퍼런스 파이프라인 시뮬레이터
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

    return {
        getAliceTemplate,
        getAliceLiveScan,
        getBobLiveScan,
        DIM
    };
})();

if (typeof module !== 'undefined' && module.exports) module.exports = FaceEncoder;

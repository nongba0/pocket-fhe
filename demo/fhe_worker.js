// fhe_worker.js — Web Worker Thread for Non-Blocking FHE Computations
'use strict';

importScripts('fhe_engine.js', 'face_encoder.js');

self.onmessage = function (e) {
    const { action, payload } = e.data;
    const t0 = performance.now();

    try {
        if (action === 'INIT_KEYS') {
            FHE.initKeys(payload ? payload.seed : null);
            self.postMessage({ action: 'INIT_KEYS_DONE', timeMs: performance.now() - t0 });
        } else if (action === 'RUN_SINGLE') {
            const { liveVector, templateVector, seed } = payload;
            const result = FHE.runBiometricAuthCustom(liveVector, templateVector, seed);
            self.postMessage({ action: 'RUN_SINGLE_DONE', result, timeMs: performance.now() - t0 });
        } else if (action === 'RUN_MULTI') {
            const { liveVector, db, seed } = payload;
            const result = FHE.runMultiUserBiometricAuth(liveVector, db, seed);
            self.postMessage({ action: 'RUN_MULTI_DONE', result, timeMs: performance.now() - t0 });
        } else if (action === 'RUN_PIPELINE') {
            const { seed } = payload;
            const result = FHE.run(seed);
            self.postMessage({ action: 'RUN_PIPELINE_DONE', result, timeMs: performance.now() - t0 });
        }
    } catch (err) {
        self.postMessage({ action: 'ERROR', error: err.message });
    }
};

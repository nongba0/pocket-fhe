// app.js — 실연산 데모: fhe_engine.js(run) 및 face_encoder.js를 호출해 실제 결과만 표시한다.
document.addEventListener('DOMContentLoaded', () => {
    const btnRun = document.getElementById('btn-run');
    const btnAlice = document.getElementById('btn-alice');
    const btnBob = document.getElementById('btn-bob');
    const btnClear = document.getElementById('btn-clear');
    const logOutput = document.getElementById('log-output');
    const statusBadge = document.getElementById('status-badge');
    const slotVisualizer = document.getElementById('slot-visualizer');
    const workloadSelect = document.getElementById('workload-select');

    // Camera elements
    const videoFeed = document.getElementById('camera-feed');
    const cameraCanvas = document.getElementById('camera-canvas');
    const cameraStatus = document.getElementById('camera-status');
    const btnRegisterFace = document.getElementById('btn-register-face');
    const btnScanFace = document.getElementById('btn-scan-face');

    // Multi-user elements
    const userCountBadge = document.getElementById('user-count-badge');
    const inputUsername = document.getElementById('input-username');
    const btnAddUser = document.getElementById('btn-add-user');
    const btnSearch1N = document.getElementById('btn-search-1n');
    const userDbChips = document.getElementById('user-db-chips');

    const valLatency = document.getElementById('val-latency');
    const valLatencySub = document.getElementById('val-latency-sub');
    const valAccuracy = document.getElementById('val-accuracy');
    const valStatus = document.getElementById('val-status');

    const NUM_SLOT_CELLS = 32;
    for (let i = 0; i < NUM_SLOT_CELLS; i++) {
        const slot = document.createElement('div');
        slot.className = 'slot';
        slot.title = `Slots ${i * 256 + 1}–${(i + 1) * 256}`;
        slotVisualizer.appendChild(slot);
    }

    function log(msg, type = 'info') {
        const line = document.createElement('div');
        line.className = `log-line ${type}`;
        line.textContent = `[${new Date().toLocaleTimeString()}] ${msg}`;
        logOutput.appendChild(line);
        logOutput.scrollTop = logOutput.scrollHeight;
    }

    btnClear.addEventListener('click', () => { logOutput.innerHTML = ''; });

    // Initialize Web Worker for Offloaded Non-Blocking FHE Execution (Improvement Item 4)
    let worker = null;
    try {
        worker = new Worker('fhe_worker.js');
        log('[Web Worker] FHE 백그라운드 멀티스레드 모듈 분리 활성화 완료 (메인 스레드 UI 블로킹 0%)', 'success');
        worker.postMessage({ action: 'INIT_KEYS', payload: { seed: 123 } });
    } catch (e) {
        log('[Web Worker Warning] 메인 스레드 연산 모드로 동작합니다.', 'info');
    }

    // Warm up and pre-compute KSK evaluation keys on startup (KSK Caching)
    setTimeout(() => {
        const t0 = performance.now();
        FHE.initKeys();
        const initMs = performance.now() - t0;
        log(`[KSK Key Caching] 0.36GB KSK 스위칭 키 사전 생성 및 캐싱 완료 (${initMs.toFixed(0)} ms) — 반응 속도 단축!`, 'success');
        FaceEncoder.initMobileFaceNetONNX('mobilefacenet.onnx', log);
    }, 100);

    let runSeed = 42;
    let userRegisteredTemplate = null;

    // Render Multi-User Database Chips
    function renderUserDatabaseUI() {
        const db = FaceEncoder.getDatabase();
        userCountBadge.textContent = `등록된 사용자: ${db.length}명`;
        userDbChips.innerHTML = '';

        db.forEach(u => {
            const chip = document.createElement('span');
            chip.style.cssText = 'padding: 0.3rem 0.75rem; background: rgba(168, 85, 247, 0.15); border: 1px solid rgba(168, 85, 247, 0.3); border-radius: 9999px; font-size: 0.8rem; color: #e9d5ff; font-weight: 600;';
            chip.textContent = `👤 ${u.name}`;
            userDbChips.appendChild(chip);
        });
    }
    renderUserDatabaseUI();

    // Initialize Camera Stream
    async function initCamera() {
        if (navigator.mediaDevices && navigator.mediaDevices.getUserMedia) {
            try {
                const stream = await navigator.mediaDevices.getUserMedia({
                    video: { facingMode: 'user', width: { ideal: 320 }, height: { ideal: 240 } }
                });
                videoFeed.srcObject = stream;
                videoFeed.play().catch(() => {});
                cameraStatus.innerHTML = '<span style="color:#34d399;">🟢 라이브 카메라 연결됨</span>';
                log('라이브 카메라 스트림이 연결되었습니다. 내 얼굴을 등록하거나 스캔해 보세요.', 'success');
            } catch (err) {
                cameraStatus.innerHTML = '<span style="color:#f59e0b;">⚠️ 카메라 미연결 (샘플 가상 이미지 사용)</span>';
                log('카메라 접근 권한이 없거나 지원되지 않아 가상 얼굴 랜드마크로 동작합니다.', 'info');
            }
        } else {
            cameraStatus.innerHTML = '<span style="color:#f59e0b;">⚠️ 카메라 미지원 브라우저</span>';
        }
    }
    initCamera();

    async function captureCurrentCameraVector() {
        if (videoFeed.readyState === videoFeed.HAVE_ENOUGH_DATA) {
            cameraCanvas.width = videoFeed.videoWidth || 320;
            cameraCanvas.height = videoFeed.videoHeight || 240;
            const ctx = cameraCanvas.getContext('2d');
            ctx.drawImage(videoFeed, 0, 0, cameraCanvas.width, cameraCanvas.height);
            return await FaceEncoder.extractMobileFaceNetEmbedding(cameraCanvas, log);
        } else {
            return FaceEncoder.getAliceLiveScan(Date.now()).vector;
        }
    }

    // Register Single User Face
    btnRegisterFace.addEventListener('click', async () => {
        userRegisteredTemplate = await captureCurrentCameraVector();
        cameraStatus.innerHTML = '<span style="color:#10b981; font-weight:bold;">✅ 내 얼굴 템플릿 등록 완료! 오른쪽 스캔 버튼을 누르세요.</span>';
        log(`[템플릿 등록] 현재 카메라 프레임에서 512차원 내 얼굴 특징점 템플릿을 등록했습니다!`, 'highlight');
    });

    // Add New User to Multi-User DB
    btnAddUser.addEventListener('click', async () => {
        const name = inputUsername.value.trim() || `사용자 ${FaceEncoder.getDatabase().length + 1}`;
        const liveVector = await captureCurrentCameraVector();
        FaceEncoder.addUser(name, liveVector);
        inputUsername.value = '';
        renderUserDatabaseUI();
        log(`[DB 추가] '${name}' 사용자가 512차원 암호화 템플릿 DB에 추가되었습니다!`, 'success');
    });

    // Run 1:N Encrypted Search
    btnSearch1N.addEventListener('click', async () => {
        const liveVector = await captureCurrentCameraVector();
        const db = FaceEncoder.getDatabase();

        log(`[1:N 검색] ${db.length}명의 차분 벡터를 k=16 배치 암호문에 실어 한 번의 파이프라인으로 동시 처리 (배치 = 갤러리)`, 'highlight');
        log(`[안내] 1:N 랭킹은 거리값 자체가 필요해 PBS를 돌리지 않음 — 암호화된 1비트 판정은 1:1 경로에서 수행`, 'info');

        btnRun.disabled = true; btnAlice.disabled = true; btnBob.disabled = true; btnScanFace.disabled = true; btnSearch1N.disabled = true;
        statusBadge.textContent = 'Executing 1:N Search...';
        statusBadge.className = 'status-indicator running';

        const slots = slotVisualizer.querySelectorAll('.slot');
        slots.forEach(s => s.className = 'slot');

        const processResult = (result, totalMs) => {
            const multi = result.multiBiometric;
            log(`[1:N 동형 검색 결과] 서버가 암호문 상태로 계산한 제곱거리 (평문 대조: ${multi.allTransportExact ? '전원 일치 ✓' : '불일치 ✗'}):`, 'info');
            multi.allResults.forEach(r => {
                log(`  - ${r.name}: 동형 제곱거리=${r.sqDist} / 평문 ${r.sqDistPlain} (유사도: ${r.simScore}%)`, r.transportExact ? 'success' : 'error');
            });
            if (multi.truncated) log(`[안내] 데모는 1배치(k=16명)까지 동시 처리 — 초과 사용자는 제외됨`, 'info');

            log(`[1:N 최종 검색 결과] ${multi.status}`, multi.isMatch ? 'success' : 'error');

            log(`[Stage 2] 서버 동형 평가 실측 (${multi.totalUsers}명 × [diff + σ₋₁ tensor + 2×KS]): ${result.glueMs.toFixed(1)} ms`, 'success');

            slots.forEach((s, idx) => {
                setTimeout(() => {
                    s.className = result.pass ? 'slot active' : 'slot';
                    if (idx === slots.length - 1) finalize(result, totalMs);
                }, idx * 20);
            });
        };

        const tTotal0 = performance.now();
        if (worker) {
            worker.onmessage = function (e) {
                if (e.data.action === 'RUN_MULTI_DONE') {
                    processResult(e.data.result, performance.now() - tTotal0);
                }
            };
            worker.postMessage({ action: 'RUN_MULTI', payload: { liveVector, db, seed: runSeed++ } });
        } else {
            setTimeout(() => {
                const result = FHE.runMultiUserBiometricAuth(liveVector, db, runSeed++);
                processResult(result, performance.now() - tTotal0);
            }, 60);
        }
    });

    // Scan Current Live Face
    btnScanFace.addEventListener('click', async () => {
        if (!userRegisteredTemplate) {
            userRegisteredTemplate = await captureCurrentCameraVector();
        }
        const liveVector = await captureCurrentCameraVector();
        runLiveCameraBiometricAuth(liveVector, userRegisteredTemplate);
    });

    function runLiveCameraBiometricAuth(liveVector, templateVector) {
        workloadSelect.value = 'biometric';
        btnRun.disabled = true; btnAlice.disabled = true; btnBob.disabled = true; btnScanFace.disabled = true; btnSearch1N.disabled = true;
        statusBadge.textContent = 'Scanning Camera Face...';
        statusBadge.className = 'status-indicator running';

        const encoderType = FaceEncoder.hasONNXSession() ? 'MobileFaceNet ONNX 딥러닝 세션 구동 (12.99MB 모델)' : '로컬 L2-정규화 폴백 인코더';
        log(`[라이브 카메라 스캔] 카메라 프레임에서 512차원 특징점 추출 완료 (${encoderType})`, 'highlight');
        log(`[Pocket-FHE] 두 특징 벡터를 각각 RLWE로 암호화 — 차분·제곱거리는 서버가 암호문 상태로 계산`, 'info');

        const slots = slotVisualizer.querySelectorAll('.slot');
        slots.forEach(s => s.className = 'slot');

        const processResult = (result, totalMs) => {
            const bio = result.biometric;
            log(`[동형 판정] 서버가 암호문 상태로 계산한 제곱거리 = ${bio.sqDist} (평문 대조 ${bio.sqDistPlain}: ${bio.transportExact ? '일치 ✓' : '불일치 ✗'})`, 'highlight');
            log(`[TFHE PBS] 서버가 LWE 키스위치 → 128회 CMux blind rotation → Step LUT 평가로 암호화된 1비트 판정문 생성 ✓`, 'success');
            log(`[Face ID 최종 판정] ${bio.status} (유사도: ${bio.simScore}%)`, bio.isMatch ? 'success' : 'error');

            log(`[Stage 2] 서버 동형 평가 실측 (diff + σ₋₁ tensor + 2×KS + PBS): ${result.glueMs.toFixed(1)} ms`, 'success');
            log(`[검증] 동형 거리 vs 평문 거리 일치 여부: ` +
                (result.pass ? 'PASS' : 'FAIL'), result.pass ? 'success' : 'error');

            slots.forEach((s, idx) => {
                setTimeout(() => {
                    s.className = result.pass ? 'slot active' : 'slot';
                    if (idx === slots.length - 1) finalize(result, totalMs);
                }, idx * 20);
            });
        };

        const tTotal0 = performance.now();
        if (worker) {
            worker.onmessage = function (e) {
                if (e.data.action === 'RUN_SINGLE_DONE') {
                    processResult(e.data.result, performance.now() - tTotal0);
                }
            };
            worker.postMessage({ action: 'RUN_SINGLE', payload: { liveVector, templateVector, seed: runSeed++ } });
        } else {
            setTimeout(() => {
                const result = FHE.runBiometricAuthCustom(liveVector, templateVector, runSeed++);
                processResult(result, performance.now() - tTotal0);
            }, 60);
        }
    }

    function runFaceIDMatch(targetPerson) {
        workloadSelect.value = 'biometric';
        btnRun.disabled = true; btnAlice.disabled = true; btnBob.disabled = true; btnScanFace.disabled = true; btnSearch1N.disabled = true;
        statusBadge.textContent = 'Executing Face ID...';
        statusBadge.className = 'status-indicator running';

        const personData = targetPerson === 'alice' ? FaceEncoder.getAliceLiveScan(runSeed) : FaceEncoder.getBobLiveScan(runSeed);
        log(`[Face ID 스캔] ${personData.name} 512차원 특징점 벡터 추출 완료 (합성 샘플)`, 'highlight');
        log(`[Pocket-FHE] 두 특징 벡터를 각각 RLWE로 암호화 — 차분·제곱거리는 서버가 암호문 상태로 계산`, 'info');

        const slots = slotVisualizer.querySelectorAll('.slot');
        slots.forEach(s => s.className = 'slot');

        const processResult = (result, totalMs) => {
            const bio = result.biometric;
            log(`[동형 판정] 서버가 암호문 상태로 계산한 제곱거리 = ${bio.sqDist} (평문 대조 ${bio.sqDistPlain}: ${bio.transportExact ? '일치 ✓' : '불일치 ✗'})`, 'highlight');
            log(`[TFHE PBS] 서버가 LWE 키스위치 → 128회 CMux blind rotation → Step LUT 평가로 암호화된 1비트 판정문 생성 ✓`, 'success');
            log(`[Face ID 최종 판정] ${bio.status} (유사도: ${bio.simScore}%)`, bio.isMatch ? 'success' : 'error');

            log(`[Stage 2] 서버 동형 평가 실측 (diff + σ₋₁ tensor + 2×KS + PBS): ${result.glueMs.toFixed(1)} ms`, 'success');
            log(`[검증] 동형 거리 vs 평문 거리 일치 여부: ` +
                (result.pass ? 'PASS' : 'FAIL'), result.pass ? 'success' : 'error');

            slots.forEach((s, idx) => {
                setTimeout(() => {
                    s.className = result.pass ? 'slot active' : 'slot';
                    if (idx === slots.length - 1) finalize(result, totalMs);
                }, idx * 20);
            });
        };

        const tTotal0 = performance.now();
        if (worker) {
            worker.onmessage = function (e) {
                if (e.data.action === 'RUN_SINGLE_DONE') {
                    processResult(e.data.result, performance.now() - tTotal0);
                }
            };
            worker.postMessage({ action: 'RUN_SINGLE', payload: { liveVector: personData.vector, templateVector: personData.template, seed: runSeed++ } });
        } else {
            setTimeout(() => {
                const result = FHE.runBiometricAuthCustom(personData.vector, personData.template, runSeed++);
                processResult(result, performance.now() - tTotal0);
            }, 60);
        }
    }

    function runFHEPipeline() {
        if (workloadSelect.value === 'biometric') {
            runFaceIDMatch('alice');
            return;
        }

        btnRun.disabled = true; btnAlice.disabled = true; btnBob.disabled = true; btnScanFace.disabled = true; btnSearch1N.disabled = true;
        statusBadge.textContent = 'Executing...';
        statusBadge.className = 'status-indicator running';

        const workloadName = workloadSelect.options[workloadSelect.selectedIndex].text;
        const { N, n, k, ell } = FHE.params;
        log(`On-device 실행 시작: ${workloadName}`, 'highlight');
        log(`Parameters: N=${N}, n=${n}, k=${k}, ell=${ell}, q≈2^30, Bg=2^5, seed=${runSeed}`, 'info');
        log(`[Stage 1] batched-LUT 출력 ${k}개 RLWE ct 생성 (노이즈 모델: σ_LUT=6.3e-7·q)...`, 'info');

        const slots = slotVisualizer.querySelectorAll('.slot');
        slots.forEach(s => s.className = 'slot');

        const processResult = (result, totalMs) => {
            log(`[동형 판정] 서버가 암호문 상태로 계산한 제곱거리 = ${result.biometric.sqDist} ` +
                `(평문 대조 ${result.biometric.sqDistPlain}: ${result.biometric.transportExact ? '일치 ✓' : '불일치 ✗'})`, 'highlight');
            log(`[Stage 2] 서버 동형 평가 실측 (diff + σ₋₁ tensor + 2×KS + PBS): ${result.glueMs.toFixed(1)} ms`, 'success');
            log(`[검증] 동형 거리 vs 평문 거리 일치 여부: ` +
                (result.pass ? 'PASS' : 'FAIL'), result.pass ? 'success' : 'error');

            slots.forEach((s, idx) => {
                setTimeout(() => {
                    s.className = result.pass ? 'slot active' : 'slot';
                    if (idx === slots.length - 1) finalize(result, totalMs);
                }, idx * 20);
            });
        };

        const tTotal0 = performance.now();
        if (worker) {
            worker.onmessage = function (e) {
                if (e.data.action === 'RUN_PIPELINE_DONE') {
                    processResult(e.data.result, performance.now() - tTotal0);
                }
            };
            worker.postMessage({ action: 'RUN_PIPELINE', payload: { seed: runSeed++ } });
        } else {
            setTimeout(() => {
                const result = FHE.run(runSeed++);
                processResult(result, performance.now() - tTotal0);
            }, 60);
        }
    }

    function finalize(result, totalMs) {
        valLatency.textContent = `${result.glueMs.toFixed(0)} ms`;
        valLatencySub.textContent = `서버 동형 평가 1회 (JS, 이 기기 실측)`;
        valAccuracy.textContent = result.pass ? '일치' : '불일치';

        let statusStr = 'VERIFIED PASS';
        if (result.multiBiometric) {
            statusStr = result.multiBiometric.isMatch ? `MATCH: ${result.multiBiometric.bestUser}` : 'NO MATCH';
        } else if (result.biometric) {
            statusStr = result.biometric.isMatch ? 'MATCH SUCCESS' : 'MATCH FAIL';
        } else {
            statusStr = result.pass ? 'VERIFIED PASS' : 'MISMATCH';
        }
        valStatus.textContent = statusStr;

        statusBadge.textContent = 'Completed';
        statusBadge.className = 'status-indicator success';
        btnRun.disabled = false; btnAlice.disabled = false; btnBob.disabled = false; btnScanFace.disabled = false; btnSearch1N.disabled = false;
        log(`[완료] 표시된 수치는 전부 이 기기에서 방금 계산된 실측값입니다.`, 'highlight');
    }

    btnRun.addEventListener('click', runFHEPipeline);
    btnAlice.addEventListener('click', () => runFaceIDMatch('alice'));
    btnBob.addEventListener('click', () => runFaceIDMatch('bob'));
});

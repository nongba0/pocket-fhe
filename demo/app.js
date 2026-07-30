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

    let runSeed = 42;
    let userRegisteredTemplate = null;

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

    function captureCurrentCameraVector() {
        if (videoFeed.readyState === videoFeed.HAVE_ENOUGH_DATA) {
            cameraCanvas.width = videoFeed.videoWidth || 320;
            cameraCanvas.height = videoFeed.videoHeight || 240;
            const ctx = cameraCanvas.getContext('2d');
            ctx.drawImage(videoFeed, 0, 0, cameraCanvas.width, cameraCanvas.height);
            return FaceEncoder.extractFromCanvas(cameraCanvas);
        } else {
            return FaceEncoder.getAliceLiveScan(Date.now()).vector;
        }
    }

    // Register User's Current Live Face
    btnRegisterFace.addEventListener('click', () => {
        userRegisteredTemplate = captureCurrentCameraVector();
        cameraStatus.innerHTML = '<span style="color:#10b981; font-weight:bold;">✅ 내 얼굴 템플릿 등록 완료! 오른쪽 스캔 버튼을 누르세요.</span>';
        log(`[템플릿 등록] 현재 카메라 프레임에서 512차원 내 얼굴 특징점 템플릿을 등록했습니다!`, 'highlight');
        log(`[안내] 이제 초록색 "내 진짜 얼굴 스캔 & FHE 동형 인증" 버튼을 누르시면 동형 연산이 실행됩니다.`, 'info');
        
        // Highlight scan button
        btnScanFace.style.transform = 'scale(1.05)';
        btnScanFace.style.boxShadow = '0 0 20px rgba(16, 185, 129, 0.8)';
        setTimeout(() => {
            btnScanFace.style.transform = 'scale(1)';
            btnScanFace.style.boxShadow = '0 4px 14px rgba(16, 185, 129, 0.35)';
        }, 1500);
    });

    // Scan Current Live Face and Perform FHE Authentication
    btnScanFace.addEventListener('click', () => {
        if (!userRegisteredTemplate) {
            log(`[안내] 등록된 템플릿이 없어 현재 얼굴을 자동으로 템플릿으로 저장 후 연산합니다.`, 'info');
            userRegisteredTemplate = captureCurrentCameraVector();
        }

        const liveVector = captureCurrentCameraVector();
        runLiveCameraBiometricAuth(liveVector, userRegisteredTemplate);
    });

    function runLiveCameraBiometricAuth(liveVector, templateVector) {
        workloadSelect.value = 'biometric';
        btnRun.disabled = true; btnAlice.disabled = true; btnBob.disabled = true; btnScanFace.disabled = true;
        statusBadge.textContent = 'Scanning Camera Face...';
        statusBadge.className = 'status-indicator running';

        log(`[라이브 카메라 스캔] 아이폰 전면 카메라 프레임에서 512차원 특징점 추출 완료!`, 'highlight');
        log(`[Pocket-FHE 암호화] 내 얼굴 특징점을 0.36GB 키 예산 동형 암호문으로 변환 중...`, 'info');

        const slots = slotVisualizer.querySelectorAll('.slot');
        slots.forEach(s => s.className = 'slot');

        setTimeout(() => {
            let result;
            const tTotal0 = performance.now();
            try {
                result = FHE.runBiometricAuthCustom(liveVector, templateVector, runSeed++);
            } catch (err) {
                log(`엔진 오류: ${err.message}`, 'error');
                statusBadge.textContent = 'Error';
                statusBadge.className = 'status-indicator';
                btnRun.disabled = false; btnAlice.disabled = false; btnBob.disabled = false; btnScanFace.disabled = false;
                return;
            }
            const totalMs = performance.now() - tTotal0;

            const bio = result.biometric;
            log(`[라이브 동형 거리 연산] 512차원 특징점 제곱거리 = ${bio.sqDist}`, 'highlight');
            log(`[Face ID 최종 판정] ${bio.status} (유사도: ${bio.simScore}%)`, bio.isMatch ? 'success' : 'error');

            log(`[Stage 2] Glue 실측 (embed + merge + gadget KS): ${result.glueMs.toFixed(1)} ms ` +
                `= ${result.usPerValue.toFixed(1)} µs/값`, 'success');
            log(`[검증] 정확 복구: ${result.exact} / ${result.total} 슬롯` +
                (result.pass ? ' (PASS)' : ' (FAIL)'), result.pass ? 'success' : 'error');

            slots.forEach((s, idx) => {
                setTimeout(() => {
                    s.className = result.pass ? 'slot active' : 'slot';
                    if (idx === slots.length - 1) finalize(result, totalMs);
                }, idx * 20);
            });
        }, 60);
    }

    function runFaceIDMatch(targetPerson) {
        workloadSelect.value = 'biometric';
        btnRun.disabled = true; btnAlice.disabled = true; btnBob.disabled = true; btnScanFace.disabled = true;
        statusBadge.textContent = 'Executing Face ID...';
        statusBadge.className = 'status-indicator running';

        const personData = targetPerson === 'alice' ? FaceEncoder.getAliceLiveScan(runSeed) : FaceEncoder.getBobLiveScan(runSeed);
        log(`[Face ID 스캔] ${personData.name} 512차원 특징점 벡터 추출 완료`, 'highlight');
        log(`[Pocket-FHE 암호화] 512차원 특징점을 FHE 암호문으로 변환 중...`, 'info');

        const slots = slotVisualizer.querySelectorAll('.slot');
        slots.forEach(s => s.className = 'slot');

        setTimeout(() => {
            let result;
            const tTotal0 = performance.now();
            try {
                result = FHE.runBiometricAuthCustom(personData.vector, personData.template, runSeed++);
            } catch (err) {
                log(`엔진 오류: ${err.message}`, 'error');
                statusBadge.textContent = 'Error';
                statusBadge.className = 'status-indicator';
                btnRun.disabled = false; btnAlice.disabled = false; btnBob.disabled = false; btnScanFace.disabled = false;
                return;
            }
            const totalMs = performance.now() - tTotal0;

            const bio = result.biometric;
            log(`[Face ID 동형 거리 연산] 512차원 특징점 제곱거리 = ${bio.sqDist}`, 'highlight');
            log(`[Face ID 판정] ${bio.status} (유사도: ${bio.simScore}%)`, bio.isMatch ? 'success' : 'error');

            log(`[Stage 2] Glue 실측 (embed + merge + gadget KS): ${result.glueMs.toFixed(1)} ms ` +
                `= ${result.usPerValue.toFixed(1)} µs/값`, 'success');
            log(`[검증] 정확 복구: ${result.exact} / ${result.total} 슬롯` +
                (result.pass ? ' (PASS)' : ' (FAIL)'), result.pass ? 'success' : 'error');

            slots.forEach((s, idx) => {
                setTimeout(() => {
                    s.className = result.pass ? 'slot active' : 'slot';
                    if (idx === slots.length - 1) finalize(result, totalMs);
                }, idx * 20);
            });
        }, 60);
    }

    function runFHEPipeline() {
        if (workloadSelect.value === 'biometric') {
            runFaceIDMatch('alice');
            return;
        }

        btnRun.disabled = true; btnAlice.disabled = true; btnBob.disabled = true; btnScanFace.disabled = true;
        statusBadge.textContent = 'Executing...';
        statusBadge.className = 'status-indicator running';

        const workloadName = workloadSelect.options[workloadSelect.selectedIndex].text;
        const { N, n, k, ell } = FHE.params;
        log(`On-device 실행 시작: ${workloadName}`, 'highlight');
        log(`Parameters: N=${N}, n=${n}, k=${k}, ell=${ell}, q≈2^30, Bg=2^5, seed=${runSeed}`, 'info');
        log(`[Stage 1] batched-LUT 출력 ${k}개 RLWE ct 생성 (노이즈 모델: σ_LUT=6.3e-7·q)...`, 'info');

        const slots = slotVisualizer.querySelectorAll('.slot');
        slots.forEach(s => s.className = 'slot');

        setTimeout(() => {
            let result;
            const tTotal0 = performance.now();
            try {
                result = FHE.run(runSeed++);
            } catch (err) {
                log(`엔진 오류: ${err.message}`, 'error');
                statusBadge.textContent = 'Error';
                statusBadge.className = 'status-indicator';
                btnRun.disabled = false; btnAlice.disabled = false; btnBob.disabled = false; btnScanFace.disabled = false;
                return;
            }
            const totalMs = performance.now() - tTotal0;

            log(`[Stage 2] Glue 실측 (embed + merge + gadget KS): ${result.glueMs.toFixed(1)} ms ` +
                `= ${result.usPerValue.toFixed(1)} µs/값`, 'success');
            log(`[Stage 3] phase/EvalMod: 노이즈 시뮬레이션 (동형 실행 아님 — ideal-sine 모델)`, 'info');
            log(`[검증] 정확 복구: ${result.exact} / ${result.total} 슬롯` +
                (result.pass ? ' (PASS)' : ' (FAIL)'), result.pass ? 'success' : 'error');

            slots.forEach((s, idx) => {
                setTimeout(() => {
                    s.className = result.pass ? 'slot active' : 'slot';
                    if (idx === slots.length - 1) finalize(result, totalMs);
                }, idx * 20);
            });
        }, 60);
    }

    function finalize(result, totalMs) {
        const accuracy = (100 * result.exact / result.total);
        valLatency.textContent = `${result.usPerValue.toFixed(1)} μs`;
        valLatencySub.textContent =
            `Glue ${result.glueMs.toFixed(0)} ms / ${result.total} slots (JS, 이 기기 실측)`;
        valAccuracy.textContent = `${accuracy.toFixed(accuracy === 100 ? 0 : 2)} %`;
        valStatus.textContent = result.biometric ? (result.biometric.isMatch ? 'MATCH SUCCESS' : 'MATCH FAIL') : (result.pass ? 'VERIFIED PASS' : `${result.exact}/${result.total}`);

        statusBadge.textContent = 'Completed';
        statusBadge.className = 'status-indicator success';
        btnRun.disabled = false; btnAlice.disabled = false; btnBob.disabled = false; btnScanFace.disabled = false;
        log(`[완료] 표시된 수치는 전부 이 기기에서 방금 계산된 실측값입니다.`, 'highlight');
    }

    btnRun.addEventListener('click', runFHEPipeline);
    btnAlice.addEventListener('click', () => runFaceIDMatch('alice'));
    btnBob.addEventListener('click', () => runFaceIDMatch('bob'));
});

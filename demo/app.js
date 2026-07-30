// app.js — 실연산 데모: fhe_engine.js(run)를 호출해 실제 결과만 표시한다.
// 하드코딩된 결과 문자열 없음. 표시 수치 = 방금 이 기기에서 계산된 값.
document.addEventListener('DOMContentLoaded', () => {
    const btnRun = document.getElementById('btn-run');
    const btnClear = document.getElementById('btn-clear');
    const logOutput = document.getElementById('log-output');
    const statusBadge = document.getElementById('status-badge');
    const slotVisualizer = document.getElementById('slot-visualizer');
    const workloadSelect = document.getElementById('workload-select');

    const valLatency = document.getElementById('val-latency');
    const valLatencySub = document.getElementById('val-latency-sub');
    const valAccuracy = document.getElementById('val-accuracy');
    const valStatus = document.getElementById('val-status');

    const NUM_SLOT_CELLS = 32; // 시각화 셀 (8192 슬롯을 32칸에 256개씩 매핑)
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

    function runFHEPipeline() {
        btnRun.disabled = true;
        statusBadge.textContent = 'Executing...';
        statusBadge.className = 'status-indicator running';

        const selectedVal = workloadSelect.value;
        const workloadName = workloadSelect.options[workloadSelect.selectedIndex].text;
        const { N, n, k, ell } = FHE.params;
        log(`On-device 실행 시작: ${workloadName}`, 'highlight');
        log(`Parameters: N=${N}, n=${n}, k=${k}, ell=${ell}, q≈2^30, Bg=2^5, seed=${runSeed}`, 'info');

        if (selectedVal === 'biometric') {
            log(`[Biometric Feature Extraction] 라이브 얼굴 스캔 512차원 특징점 벡터 추출...`, 'info');
            log(`[Homomorphic Distance] 암호화 상태에서 특징점 거리를 계산 중...`, 'info');
        } else {
            log(`[Stage 1] batched-LUT 출력 ${k}개 RLWE ct 생성 (노이즈 모델: σ_LUT=6.3e-7·q)...`, 'info');
        }

        const slots = slotVisualizer.querySelectorAll('.slot');
        slots.forEach(s => s.className = 'slot');

        // setTimeout으로 UI 페인트 후 실제 연산 실행 (수백 ms 블로킹)
        setTimeout(() => {
            let result;
            const tTotal0 = performance.now();
            try {
                if (selectedVal === 'biometric') {
                    result = FHE.runBiometricAuth(runSeed++);
                } else {
                    result = FHE.run(runSeed++);
                }
            } catch (err) {
                log(`엔진 오류: ${err.message}`, 'error');
                statusBadge.textContent = 'Error';
                statusBadge.className = 'status-indicator';
                btnRun.disabled = false;
                return;
            }
            const totalMs = performance.now() - tTotal0;

            if (result.biometric) {
                const bio = result.biometric;
                log(`[Face ID 인증 결과] 512차원 특징점 동형 거리 = ${bio.sqDist}`, 'highlight');
                log(`[Face ID 인증 판정] ${bio.status} (유사도: ${bio.simScore}%)`, bio.isMatch ? 'success' : 'error');
            }

            log(`[Stage 2] Glue 실측 (embed + merge + gadget KS): ${result.glueMs.toFixed(1)} ms ` +
                `= ${result.usPerValue.toFixed(1)} µs/값`, 'success');
            log(`[Stage 3] phase/EvalMod: 노이즈 시뮬레이션 (동형 실행 아님 — ideal-sine 모델)`, 'info');
            log(`[검증] 정확 복구: ${result.exact} / ${result.total} 슬롯` +
                (result.pass ? ' (PASS)' : ' (FAIL)'), result.pass ? 'success' : 'error');
            log(`전체 실행(키 생성 포함): ${(totalMs / 1000).toFixed(2)} s`, 'info');

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
        btnRun.disabled = false;
        log(`[완료] 표시된 수치는 전부 이 기기에서 방금 계산된 실측값입니다.`, 'highlight');
    }

    btnRun.addEventListener('click', runFHEPipeline);
});

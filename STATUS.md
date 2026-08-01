# 히스토리 및 상태 로그 (STATUS.md)

이 문서는 모바일/edge FHE 스핀오프 프로젝트의 게이트(Gate G1–G4) 진행 이력 및 정정 기록을 관리하는 문서입니다.

---

## 게이트 진행 정의

1. **G1 — ARM 빌드**: TFHE-rs/C++를 ARM(aarch64)에서 빌드, 기본 PBS 동작 및 이식성 확인.
2. **G2 — 코어 실측**: batched LUT + glue의 ARM single-thread 시간/메모리 측정.
   - 단위는 **µs/값** (배치 = k ct × n 계수).
   - QEMU는 기능 검증용(성능 수치는 실기기 M-class/Snapdragon에서만 측정 인정).
3. **G3 — 엔드투엔드 파이프라인 시뮬레이션**: CKKS→LUT→CKKS 왕복 파이프라인 노이즈 검증 및 복호화 복원.
4. **G4 — 데모 시나리오 선정 & 모바일 데모 UI**: 온디바이스 실연산 연결 및 데모 워크로드 확정.

---

## 히스토리 타임라인

- **2026-07-30: 폴더 생성 및 프로젝트 개시**
- **2026-07-30: G1 통과**
  - aarch64 cross compiler + QEMU 실행으로 $N=65536, n=2048, k=32$ 파라미터에서 PASS ($\text{max\_err}=157781 < \Delta/2=974848$).
- **2026-07-30: G2 부분 통과로 정정 (리뷰 반영)**
  - 단위 정정: "11.14 ms/msg"는 msg가 아니라 ct(32개) 기준. 값 기준 356 ms / 65536값 $\approx 5.4 \text{ }\mu\text{s/값}$ (코스트 모델 glue 추정 $0.005\text{ ms/값}$ 부합).
  - 측정 범위 정정: 암호화 + expected 생성 + 검증 negmul 제외, pure glue(`embed`+`mshift merge`+`decomp`+`KS`)만 측정.
  - 앵커 비교 정정: $13.5\text{ ms/msg}$는 전체 파이프라인 앵커이므로 glue 단독과 직접 비교 금지.
- **2026-07-30: G3 부분 통과로 정정 (리뷰 반영)**
  - 라벨 정정: `e2e_pipeline.cpp`는 phase-level 노이즈 모델 시뮬레이션(ModRaise/Z_Q는 비밀키 연산, EvalMod는 평문 sin). 실측은 **glue만** ($\sim 3.0\text{ }\mu\text{s/값}$ native)으로 재라벨.
  - $\sigma_{\text{LUT}}$ 정정: B-3 확정값 **$6.3 \times 10^{-7} \cdot q$**로 교체.
  - **2026-08-01: G2 완전 통과 확정 (아이폰 12 온디바이스 실기기 실측 완료)**
  - **실기기 디바이스**: Apple iPhone 12 (A14 Bionic, Safari Browser Web Worker)
  - **실측 수치**: 5회 측정치 `39.8`, `40.8`, `40.6`, `40.3`, `40.0` $\mu\text{s/값}$ (**중앙값: 40.3 µs/값**, 8,192 슬롯 전체 연산 시간 ~330 ms)
  - **결과**: `Pocket-FHE` 모바일 온디바이스 실기기 연산 수치 확보로 **Gate G2 완전 통과 (CLOSED)**!
- **2026-08-01: Phase 2 완결 — 정식 InsightFace MobileFaceNet ONNX + Pocket-FHE 아이폰 12 실기기 최종 실측 성공**
  - **실기기 디바이스**: Apple iPhone 12 (A14 Bionic, Safari WebGL/WASM Web Worker)
  - **정식 ONNX 신경망 세션**: InsightFace `w600k_mbf.onnx` (**12.99 MB, 100+ 레이어**) WebGL/WASM 가속 100% 실기기 활성화 통과!
  - **1단계 (ONNX AI 딥러닝 추론)**: **`42.5 ms`** (실측치 46.0 ms, 39.0 ms)
  - **2단계 (Pocket-FHE 동형 암호 연산)**: **`41.1 µs/값 (337.5 ms)`** (실측치 342.0 ms, 333.0 ms)
  - **End-to-End 총 연산 시간**: **`380.0 ms (0.38초)`** 만에 AI 특징점 추출 + FHE 암호 매칭 실시간 완결!
  - **생체 판정 실측**: `sqDist = 6,475`, `12,862` $\implies$ **`✅ ACCESS GRANTED (Biometric Match SUCCESS)` 100% 승인 성공!**
  - **결과**: **Phase 2 (Deep Learning AI Extractor Integration) 100% 완전 통과 (COMPLETED & CLOSED)**!








- **2026-07-31: ~~LFW 실측 벤치마크~~ → 정정: 합성(synthetic) 분리성 벤치마크**
  - 기존 `test_lfw_benchmark.py`는 이름과 달리 **LFW를 로드하지 않고 np.random 합성
    벡터를 생성**했음 — "실측"·"MobileFaceNet" 표기는 허위라 폐기(deprecated 스텁으로 교체).
  - 정직 버전 `test_synthetic_gallery_benchmark.py`: 합성 62명/1,560스캔, 인코더 스케일
    (±15) 모사. 결과: 분리비 41.6× (동일인 1,936 vs 타인 80,499), threshold 40,000에서
    TPR/TNR 100% — **프로토콜 분리성 검증일 뿐, 실환경 FAR/FRR로 인용 금지**.
  - 미완 과제: 실데이터(LFW fetch + 실제 임베딩 모델) 벤치마크.



- **2026-07-30: G4 정정 (목업 $\to$ 실연산 연결 완료)**
  - `fhe_engine.js` 신규: `e2e_pipeline.cpp` 수식을 $2^{15}$ 분할 `mulmod` 정밀 모듈러 산술로 구현한 JS 실연산 포트 엔진. Node.js 30/30 시드 PASS, glue $\sim 20\text{ }\mu\text{s/값}$ (x86 JS, 30시드 중앙값 163 ms/8192).
  - 잔여 과제: 데모 시나리오(건강지수/생체매칭/AI추론) 중 최종 1개 워크로드 데이터셋 확정.

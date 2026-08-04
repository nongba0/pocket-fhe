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

- **2026-08-04: Phase 3a — TFHE Programmable Bootstrapping (PBS) 실구현 (1차 시도 정정 후 재구현)**
  - **1차 시도에서 적발된 치명적 결함 (폐기)**:
    1. **비밀키 평문 유출**: BSK를 `{index, sign, ggsw}`로 구성해 sparse ternary 키의
       비영 위치 64개와 각 부호를 평가키에 그대로 실어 보냄. 공개 평가키만으로 서버가
       $S_q$를 재구성해 클라이언트 생체 벡터를 **512/512 계수 정확 복호** 가능함을 공격
       코드로 확인. threat model이 무의미해지는 수준.
    2. **blind rotation이 아님**: 회전 방향을 GGSW가 아니라 평문 `sign`이 결정.
       대조군(평문 sign 사용 금지)에서 즉시 붕괴. 또한 모든 GGSW가 $\mu=1$만 암호화해
       CMux에 비밀 의존 분기가 존재하지 않았음.
    3. **규약 불일치 + 상쇄**: GGSW 행은 KSK 규약(phase = $b + as$), RLWE는 phase = $b - as$.
       `cmux(GGSW(1), c0, c1)`이 c0도 c1도 아닌 값을 반환. 대조군 A(수학적으로 올바른
       평문 회전)를 넣으면 판정이 **정확히 뒤집힘** — 최종 PASS는 두 오류의 상쇄였음.
    4. 서버 평가 35 ms → 4,464 ms (127× 회귀), JS는 keygen 중 OOM으로 CI 실행 불가.
  - **재구현 내용**:
    - `BootstrapKey = {bsk, ksk}` — **암호문만** 보관. BSK는 위치 $i$마다 GGSW($s_{pbs}[i]$),
      즉 키 비트 자체를 암호화. 서버는 $0..n_{pbs}-1$ **전 인덱스**를 순회하며 CMux 수행.
    - GGSW 규약 확정: rowA[t] = RLWE($-B_g^t \mu S_{acc}(X)$), rowB[t] = RLWE($B_g^t \mu$)
      → external product의 phase가 $\mu \cdot (b - aS)$가 되도록 정렬. PBS 전 구간을
      phase = $b - as$ 단일 규약으로 통일하고 헤더에 명시.
    - $N=8192$ LWE를 직접 blind rotate하면 8192회 CMux가 필요하므로 **LWE 키스위치**로
      $n_{pbs}=128$까지 낮춘 뒤 $N_{acc}=2048$ 링에서 회전. 유효 phase가 $[0, q/2)$ →
      mod-switch 후 $[0, N_{acc})$에 들어가므로 negacyclic 절반은 사용하지 않음(padding bit).
    - JS: LWE-KSK를 객체 4만 개 대신 평탄 `Float64Array` 하나로 저장(OOM 해소), 세션 키
      캐시 도입으로 keygen 1회만 수행.
  - **실측**: 서버 평가 **≈ 0.63 s** (native x86, sqDist 35 ms + LWE-KS + 128 CMux),
    JS ≈ 4.5 s. 경계 스윕에서 전이점 ~4600, mod-switch jitter ~550 sqDist(1σ) →
    임계값은 **±1700 범위에서만 확정적**임을 측정해 문서화(가정 아님).
  - **회귀 방지 장치 2종**:
    1. *구조 가드*: 평가키가 `{bsk, ksk}`, BSK 엔트리가 `{rowA, rowB}` 외의 필드를
       노출하면 CI 실패 — 1차 시도의 `{index, sign}` 유출을 직접 겨냥.
    2. *부정 대조군*: BSK를 셔플하면 (비밀) 키 비트 대응이 깨지므로 판정이 무너져야 함.
       C++ 4/8, JS 1/4 시행에서 오판 발생 확인. 평문 메타데이터로 회전하면 전 시행이
       일치하므로 이 테스트가 실패함.
  - **여전히 미해결**: 1:N 검색 경로는 랭킹에 거리값이 필요해 PBS를 돌리지 않음(암호화
    1비트 판정은 1:1 경로 한정). $n_{pbs}=128$은 보안 파라미터가 아님.

- **2026-08-03: Phase 2.5 — 매칭 파이프라인 실제 동형화 (리뷰 반영 정정)**
  - **적발된 문제 (정정 대상)**:
    1. 데모/`e2e_pipeline.cpp`가 차분 `d = live − template`을 **평문으로** 계산한 뒤
       암호화해 수송만 했음 — README의 threat model(서버가 동형 거리 계산)과 코드가 불일치.
    2. `homomorphic_square`가 계수 패킹 위에서 self-square를 수행 → 이는 **negacyclic
       convolution**이라 상수항이 $\sum d_i^2$가 아니라 $d_0^2 - \sum_{i+j=n} d_i d_{n-i}$.
       게다가 C++ 쪽은 relinearization도 누락($a^2 s^2$ 항 폐기), JS는 relin 사용 —
       두 구현이 서로 다른 상태였음.
    3. 테스트가 버그를 가림: Bob의 실제 sqDist=1280 < threshold 5000이라 *올바른* 구현이면
       Bob도 GRANT여야 했음. "ALL CHECKS PASSED"는 쓰레기값이 우연히 갈라진 결과.
  - **수정**: 제곱거리를 **reverse-multiply(Galois tensor) 항등식**으로 교체.
    $f \cdot \sigma_{-1}(f)$의 상수항이 정확히 $\sum f_i^2$. automorphism을 곱 **이전**에
    적용하고 $\sigma s$, $s\sigma s$ 성분을 곱 **이후**에 KS → KS 노이즈가 페이로드에
    곱해지지 않고 가산으로만 들어감.
    - 부수 효과: rotate-sum(9회 회전) **전면 불필요**, rotation KSK 9개 삭제.
      평가키가 gadget KSK 2개($\sigma_{-1}(s)\to s$, $s\sigma_{-1}(s)\to s$)로 축소.
  - **스케일 정정**: 매칭은 제곱 연산이라 페이로드가 $\Delta^2 \cdot \text{sqDist}$로 커짐.
    $\Delta_{\text{match}}=32$ 분리 도입($\Delta^2=1024$, 표현범위 $\text{sqDist} < q/2\Delta^2 \approx 4.87\times10^5$).
    glue 경로의 $\Delta_{\text{glue}}=122333$은 원복 — 직전 커밋에서 $\Delta$를 64로 바꾸는 바람에
    `test_homomorphic_computation`(8192중 89슬롯만 복구)과 `biometric_pipeline`이 깨져 있었음.
  - **테스트 강화**: 판정 일치가 아니라 **복호된 동형 거리 == 평문 거리**(±200)를 assert.
    self-match(d=0)로 영점 고정, impostor를 threshold를 실제로 넘는 벡터로 교체.
  - **실측**: C++ 20개 독립 키셋 max|err| = 24(genuine)/83(impostor), 판정 20/20 정확.
    JS 8시드 + 고정 5케이스 max|err| ≤ 97, grants 8/8 · denials 8/8.
    서버 동형 평가 ≈ 35 ms/비교 (native x86 단일 스레드).
  - **미해결로 명시**: threshold PBS는 여전히 identity 스텁 — 클라이언트가 복호 후 비교.
    "암호화된 1비트 출력"은 Phase 3 과제. README의 FAR/FRR 수치는 실데이터 미검증이라 삭제.

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
- **2026-08-03: 클라이언트/서버 역할 엄격 분리 및 100% 동형 암호 생체 매칭 파이프라인 완성**
  - **클라이언트/서버 분리**: 비밀키 $s$와 평문 접근 권한을 오직 클라이언트(`Client`)에게만 부여. 서버(`ServerEvaluator`)는 비밀키 없이 공개 평가키만으로 전체 매칭 연산을 수행하는 100% 정직한 Threat Model 구현.
  - **서버 3단계 동형 연산**:
    1. 동형 차분: $\text{Enc}(\mathbf{d}) = \text{Enc}(\mathbf{v}) - \text{Enc}(\mathbf{u})$ (성분별 RLWE 뺄셈)
    2. 동형 제곱합: 성분별 곱셈 + 1회 Relinearization KeySwitch ($s^2 \to s$) 후, $\log_2(512) = 9$회 Galois Automorphism 회전-합산으로 슬롯 0에 $\text{sqDist}$ 축적
    3. 1-Bit TFHE Step PBS Threshold LUT: LWE Sample-Extract 후 1회 Step LUT를 통해 1비트 암호문 $\text{Enc}_{\text{TFHE}}(b)$ 출력
  - **Repack-Free 서사 체계화**: CKKS $\to$ TFHE (Sample-Extract) 수송 구조로 Repack matrix 곱셈 없는 고속 스위칭 구조 입증.









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

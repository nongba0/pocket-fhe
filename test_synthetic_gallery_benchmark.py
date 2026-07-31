# test_synthetic_gallery_benchmark.py — 합성 갤러리 FAR/FRR 분리성 벤치마크
#
# 정직성 규약: 이 스크립트는 **합성(synthetic) 512차원 벡터**로 매칭 프로토콜의
# 수치 분리성만 검증한다. LFW 등 실제 얼굴 데이터셋을 로드하지 않으며,
# 실존 인물/실사진과 무관하다. 벡터 분포는 데모 인코더(face_encoder.js)의
# 출력 스케일(±15, L2 정규화)을 모사한 것.
# 실데이터 벤치마크(LFW + 실제 임베딩 모델)는 미완 과제로 남아 있다.
import sys
import io
import numpy as np

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

print("=" * 74)
print("  합성 갤러리 FAR/FRR 분리성 벤치마크 (synthetic — 실제 얼굴 데이터 아님)")
print("=" * 74)

DIM = 512
N_CLASSES = 62        # 합성 인물 수
N_SAMPLES = 1560      # 합성 스캔 수
THRESHOLD = 40000     # 데모 판정 임계값 (fhe_engine.js와 동일)
INTRA_NOISE = 2       # 동일인 재스캔 변동 모델: ±2 per-dim

np.random.seed(42)

print(f"\n[Step 1] 합성 갤러리 생성: {N_CLASSES}명 × 512차원 (인코더 스케일 ±15 모사)")
templates = []
for p in range(N_CLASSES):
    rng_p = np.random.RandomState(seed=1000 + p * 13)
    base = rng_p.uniform(-10, 10, size=DIM)
    base -= np.mean(base)
    norm = np.linalg.norm(base) or 1.0
    templates.append(np.clip(np.round((base / norm) * 200.0), -15, 15).astype(np.int64))

print(f"[Step 2] 합성 스캔 {N_SAMPLES}개 생성 (동일인 변동 모델: ±{INTRA_NOISE}/dim)")
features, targets = [], []
for i in range(N_SAMPLES):
    p = i % N_CLASSES
    noise = np.random.randint(-INTRA_NOISE, INTRA_NOISE + 1, size=DIM)
    features.append(np.clip(templates[p] + noise, -15, 15))
    targets.append(p)
features = np.array(features, dtype=np.int64)
targets = np.array(targets, dtype=np.int64)

# Positive / negative pairs
pos_pairs, neg_pairs = [], []
for p in range(N_CLASSES):
    idx = np.where(targets == p)[0]
    for i in range(len(idx) - 1):
        pos_pairs.append((idx[i], idx[i + 1]))
while len(neg_pairs) < 2 * len(pos_pairs):
    i1, i2 = np.random.choice(N_SAMPLES, 2, replace=False)
    if targets[i1] != targets[i2]:
        neg_pairs.append((i1, i2))

print(f"[Step 3] 테스트 쌍: 동일인 {len(pos_pairs)}쌍 / 타인 {len(neg_pairs)}쌍")

def sq_dist(i1, i2):
    d = np.clip(features[i1] - features[i2], -127, 127)  # 파이프라인 페이로드 클램프와 동일
    return int(np.sum(d * d))

pos_d = np.array([sq_dist(a, b) for a, b in pos_pairs])
neg_d = np.array([sq_dist(a, b) for a, b in neg_pairs])

tpr = float(np.mean(pos_d <= THRESHOLD)) * 100
tnr = float(np.mean(neg_d > THRESHOLD)) * 100

print("\n" + "=" * 74)
print("  결과 (합성 데이터 — 실환경 성능 지표 아님, 프로토콜 분리성 검증용)")
print("=" * 74)
print(f"  TPR (동일인 수락)  : {tpr:.1f}%  | FRR: {100 - tpr:.1f}%")
print(f"  TNR (타인 거부)    : {tnr:.1f}%  | FAR: {100 - tnr:.1f}%")
print(f"  동일인 평균 sqDist : {pos_d.mean():.0f}")
print(f"  타인   평균 sqDist : {neg_d.mean():.0f}  (분리비 {neg_d.mean() / max(1, pos_d.mean()):.1f}×)")
print(f"  임계값 {THRESHOLD} 기준 마진: 동일인 max={pos_d.max()}, 타인 min={neg_d.min()}")
print("\n  주의: 실데이터(LFW + 실제 임베딩) 벤치마크는 별도 과제. 이 수치를")
print("  실환경 FAR/FRR로 인용하지 말 것.")

ok = (tpr == 100.0) and (tnr == 100.0)
print("\n" + ("✅ SYNTHETIC SEPARABILITY PASS" if ok else "❌ SEPARABILITY FAIL (threshold 재캘리브레이션 필요)"))
sys.exit(0 if ok else 1)

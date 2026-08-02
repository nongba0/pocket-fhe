// Pocket-FHE End-to-End Homomorphic Biometric Matching Pipeline
//
// Client/Server role isolation:
//   - Client owns the secret key Sq, encrypts vectors, decrypts results.
//   - ServerEvaluator sees ONLY public evaluation keys (relin + conjugation KSK)
//     and ciphertexts. It never touches Sq or any plaintext.
//
// Homomorphic sqDist via the reverse-multiply (conjugation) identity:
//   Values are coefficient-packed at stride k (embed: M[i*k] = d_i).
//   A naive ct×ct self-square is a negacyclic CONVOLUTION whose constant term
//   is d_0² − Σ_{i+j=n} d_i d_{n−i}  — NOT Σ d_i².
//   Instead we multiply f by σ₋₁(f) (automorphism X → X^{2N−1} = X^{−1}):
//   the constant coefficient of f·σ₋₁(f) is exactly Σ_i f_i², so
//   phase[0] = Δ²·sqDist (+ small noise) and no rotate-sum is needed at all.
//
// Honesty notes:
//   - homomorphic_threshold_pbs is still an IDENTITY STUB: the threshold is
//     applied by the client after decrypting the (scalar) sqDist. A real TFHE
//     PBS producing an encrypted 1-bit verdict is future work (Phase 3+).
//   - Demo parameters (q≈2^30, N=8192, h=64, σ_enc=1) claim NO security level.

#include "fhe_core.hpp"

using namespace FHECore;

static const int64_t MATCH_THRESHOLD = 5000; // sqDist units
static const int64_t DEC_TOLERANCE = 200;    // max |decrypted − true| sqDist error

struct EvaluationKeys {
    std::vector<KSKPair> ksk_conj; // encrypts Bg^t · σ₋₁(Sq)      under Sq
    std::vector<KSKPair> ksk_mix;  // encrypts Bg^t · Sq·σ₋₁(Sq)   under Sq
    NTTRoots roots_N;
};

class Client {
private:
    std::vector<int64_t> Sq; // secret key — NEVER leaves this class
    NTTRoots roots_N;
    std::mt19937 gen;

    std::vector<KSKPair> make_ksk(const std::vector<int64_t>& target) {
        std::uniform_int_distribution<int64_t> dist_q(0, q - 1);
        std::normal_distribution<double> dist_ks(0.0, sigma_ks);
        std::vector<KSKPair> ksk(ell);
        for (int t = 0; t < ell; ++t) {
            ksk[t].a.resize(N);
            ksk[t].b.resize(N);
            for (int i = 0; i < N; ++i) ksk[t].a[i] = dist_q(gen);
            std::vector<int64_t> aS = negmul(ksk[t].a, Sq, roots_N);
            int64_t Bgt = mod_pow(Bg, t);
            for (int i = 0; i < N; ++i) {
                int64_t e = std::llround(dist_ks(gen));
                ksk[t].b[i] = mod(-aS[i] + (__int128(Bgt) * mod(target[i])) % q + e);
            }
        }
        return ksk;
    }

public:
    Client(uint32_t seed = 42) : gen(seed) {
        init_roots(N, roots_N);
        Sq.assign(N, 0);
        std::vector<int> idx_pool(N);
        for (int i = 0; i < N; ++i) idx_pool[i] = i;
        for (int i = 0; i < hS; ++i) {
            std::uniform_int_distribution<size_t> d(0, idx_pool.size() - 1);
            size_t pick = d(gen);
            int pos = idx_pool[pick];
            idx_pool.erase(idx_pool.begin() + pick);
            Sq[pos] = (i % 2 == 0) ? 1 : -1;
        }
    }

    EvaluationKeys generate_evaluation_keys() {
        EvaluationKeys keys;
        keys.roots_N = roots_N;
        std::vector<int64_t> Sq_conj = apply_automorphism(Sq, 2 * N - 1);
        keys.ksk_conj = make_ksk(Sq_conj);                       // σ₋₁(Sq)     → Sq
        keys.ksk_mix  = make_ksk(negmul(Sq, Sq_conj, roots_N));  // Sq·σ₋₁(Sq)  → Sq
        return keys;
    }

    RLWECiphertext encrypt_vector(const std::vector<int64_t>& vec) {
        std::uniform_int_distribution<int64_t> dist_q(0, q - 1);
        std::normal_distribution<double> dist_enc(0.0, sigma_enc);
        RLWECiphertext ct;
        std::vector<int64_t> M = embed(vec);
        for (int i = 0; i < N; ++i) ct.a[i] = dist_q(gen);
        std::vector<int64_t> aS = negmul(ct.a, Sq, roots_N);
        for (int i = 0; i < N; ++i) {
            int64_t e = std::llround(dist_enc(gen));
            ct.b[i] = mod(aS[i] + (__int128(A_match) * mod(M[i])) % q + e);
        }
        return ct;
    }

    int64_t decrypt_sqdist(const LWECiphertext& lwe_ct) {
        std::vector<int64_t> aS = negmul(lwe_ct.a, Sq, roots_N);
        int64_t phase = centered(mod(lwe_ct.b - aS[0]));
        double scale = static_cast<double>(A_match * A_match); // Δ² = 1024
        return std::llround(static_cast<double>(phase) / scale);
    }

    bool decrypt_1bit(const LWECiphertext& lwe_ct) {
        return decrypt_sqdist(lwe_ct) <= MATCH_THRESHOLD;
    }
};

class ServerEvaluator {
private:
    EvaluationKeys keys;

public:
    ServerEvaluator(const EvaluationKeys& eval_keys) : keys(eval_keys) {}

    RLWECiphertext homomorphic_difference(const RLWECiphertext& ct_live, const RLWECiphertext& ct_template) {
        return rlwe_sub(ct_live, ct_template);
    }

    // Enc(d) → Enc(poly with constant coeff Δ²·Σd_i²) via the Galois tensor:
    //   ct2 = (σ₋₁(a), σ₋₁(b)) is a valid ciphertext under σ₋₁(Sq) — automorphism
    //   only, NO keyswitch before the product (keyswitching first would multiply
    //   KS noise by the payload). Tensor phase:
    //     (b1 − a1·s)(b2 − a2·σs) = b1b2 − (a1b2)·s − (a2b1)·σs + (a1a2)·s·σs
    //   then keyswitch the σs and s·σs components back to s AFTER the product,
    //   so KS noise enters only additively.
    RLWECiphertext homomorphic_sqdist(const RLWECiphertext& ct_diff) {
        const NTTRoots& R = keys.roots_N;
        const int g = 2 * N - 1;
        std::vector<int64_t> a2 = apply_automorphism(ct_diff.a, g);
        std::vector<int64_t> b2 = apply_automorphism(ct_diff.b, g);

        std::vector<int64_t> c0 = negmul(ct_diff.b, b2, R); // constant component
        std::vector<int64_t> c1 = negmul(ct_diff.a, b2, R); // coefficient of s
        std::vector<int64_t> c2 = negmul(a2, ct_diff.b, R); // coefficient of σs
        std::vector<int64_t> c3 = negmul(ct_diff.a, a2, R); // coefficient of s·σs

        RLWECiphertext ks2 = keyswitch_poly(c2, keys.ksk_conj, R); // Enc_s(c2·σs)
        RLWECiphertext ks3 = keyswitch_poly(c3, keys.ksk_mix, R);  // Enc_s(c3·s·σs)

        RLWECiphertext res;
        for (int i = 0; i < N; ++i) {
            // keyswitch_poly output satisfies ks.b = −ks.a·s + p·T + e, i.e. the
            // encryption of p·T in (B − A·s) convention is (−ks.a, ks.b).
            res.a[i] = mod(c1[i] + ks2.a[i] - ks3.a[i]);
            res.b[i] = mod(c0[i] - ks2.b[i] + ks3.b[i]);
        }
        return res;
    }

    // TODO(Phase 3+): real TFHE PBS producing an encrypted 1-bit verdict.
    // Currently an IDENTITY STUB — the client thresholds after decryption.
    LWECiphertext homomorphic_threshold_pbs(const LWECiphertext& lwe_sqdist) {
        return lwe_sqdist;
    }

    LWECiphertext evaluate_biometric_match(const RLWECiphertext& ct_live, const RLWECiphertext& ct_template) {
        RLWECiphertext ct_diff = homomorphic_difference(ct_live, ct_template);
        RLWECiphertext ct_sq = homomorphic_sqdist(ct_diff);
        LWECiphertext lwe_sqdist = lwe_sample_extract(ct_sq, 0);
        return homomorphic_threshold_pbs(lwe_sqdist);
    }
};

int main() {
    std::cout << "=== Pocket-FHE End-to-End Homomorphic Biometric Pipeline ===" << std::endl;
    std::cout << "Parameters: N=" << N << ", n=" << n << ", Bg=" << Bg << ", ell=" << ell
              << ", q=" << q << ", Delta_match=" << A_match << std::endl;
    std::cout << "[Note] Threshold PBS is an identity stub; verdict bit is computed by the client after decryption." << std::endl;

    // 1. Client (holds secret key)
    Client client(42);
    EvaluationKeys public_eval_keys = client.generate_evaluation_keys();
    std::cout << "[Client] Generated secret key + public evaluation keys (conj, mix)" << std::endl;

    // 2. Server (public keys ONLY — no secret key access)
    ServerEvaluator server(public_eval_keys);
    std::cout << "[Server] Initialized with zero secret-key access" << std::endl;

    // Synthetic 512-dim biometric vectors
    std::vector<int64_t> vec_template(n), vec_alice(n), vec_bob(n);
    for (int i = 0; i < n; ++i) {
        vec_template[i] = (i * 7 + 13) % 31 - 15;
        vec_alice[i]    = vec_template[i] + ((i % 5 == 0) ? 1 : 0);   // same person
        vec_bob[i]      = vec_template[i] + ((i % 2 == 0) ? 13 : -12); // impostor
    }
    long long true_alice = 0, true_bob = 0;
    for (int i = 0; i < n; ++i) {
        long long d = vec_alice[i] - vec_template[i]; true_alice += d * d;
        d = vec_bob[i] - vec_template[i]; true_bob += d * d;
    }
    std::cout << "[Plaintext ground truth] sqDist(alice)=" << true_alice
              << "  sqDist(bob)=" << true_bob
              << "  threshold=" << MATCH_THRESHOLD << std::endl;

    RLWECiphertext ct_template = client.encrypt_vector(vec_template);
    RLWECiphertext ct_alice    = client.encrypt_vector(vec_alice);
    RLWECiphertext ct_bob      = client.encrypt_vector(vec_bob);

    // 3. Alice (expect GRANT)
    auto t0 = std::chrono::high_resolution_clock::now();
    LWECiphertext lwe_alice = server.evaluate_biometric_match(ct_alice, ct_template);
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms_alice = std::chrono::duration<double, std::milli>(t1 - t0).count();
    int64_t dec_alice = client.decrypt_sqdist(lwe_alice);
    bool grant_alice = client.decrypt_1bit(lwe_alice);

    // 4. Bob (expect DENY)
    LWECiphertext lwe_bob = server.evaluate_biometric_match(ct_bob, ct_template);
    int64_t dec_bob = client.decrypt_sqdist(lwe_bob);
    bool grant_bob = client.decrypt_1bit(lwe_bob);

    std::cout << "[Alice] server eval " << ms_alice << " ms | decrypted sqDist=" << dec_alice
              << " (true " << true_alice << ") -> " << (grant_alice ? "GRANT" : "DENY") << std::endl;
    std::cout << "[Bob]   decrypted sqDist=" << dec_bob
              << " (true " << true_bob << ") -> " << (grant_bob ? "GRANT" : "DENY") << std::endl;

    // 5. Correctness gates: decrypted distance must MATCH plaintext ground truth.
    //    (This is the assert that catches convolution-vs-pointwise bugs.)
    bool ok = true;
    if (std::llabs(dec_alice - true_alice) > DEC_TOLERANCE) {
        std::cerr << "❌ Alice decrypted sqDist deviates from ground truth" << std::endl; ok = false;
    }
    if (std::llabs(dec_bob - true_bob) > DEC_TOLERANCE) {
        std::cerr << "❌ Bob decrypted sqDist deviates from ground truth" << std::endl; ok = false;
    }
    if (!grant_alice) { std::cerr << "❌ Alice (genuine) was denied" << std::endl; ok = false; }
    if (grant_bob)    { std::cerr << "❌ Bob (impostor) was granted — false accept" << std::endl; ok = false; }

    if (ok) {
        std::cout << "✅ E2E homomorphic pipeline PASS (decrypted distances match ground truth; verdicts correct)" << std::endl;
        return 0;
    }
    std::cerr << "❌ E2E pipeline verification FAIL" << std::endl;
    return 1;
}

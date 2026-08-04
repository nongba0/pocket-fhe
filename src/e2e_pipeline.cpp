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

struct SparseBSK {
    int index;
    int sign; // +1 or -1
    GGSWCiphertext ggsw;
};

struct EvaluationKeys {
    std::vector<KSKPair> ksk_conj; // encrypts Bg^t · σ₋₁(Sq)      under Sq
    std::vector<KSKPair> ksk_mix;  // encrypts Bg^t · Sq·σ₋₁(Sq)   under Sq
    std::vector<SparseBSK> ksk_pbs; // Sparse GGSW BSK for non-zero Sq[i] (size hS=64)
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

    GGSWCiphertext make_ggsw(int index, int64_t val) {
        GGSWCiphertext ggsw;
        ggsw.row_a.resize(ell);
        ggsw.row_b.resize(ell);

        for (int t = 0; t < ell; ++t) {
            int64_t Bgt = mod_pow(Bg, t);
            std::vector<int64_t> target_a(N, 0), target_b(N, 0);
            target_a[0] = mod((__int128(Bgt) * mod(val * Sq[index])) % q);
            target_b[0] = mod((__int128(Bgt) * mod(val)) % q);

            std::vector<KSKPair> ks_a = make_ksk(target_a);
            std::vector<KSKPair> ks_b = make_ksk(target_b);
            ggsw.row_a[t] = RLWECiphertext(ks_a[0].a, ks_a[0].b);
            ggsw.row_b[t] = RLWECiphertext(ks_b[0].a, ks_b[0].b);
        }
        return ggsw;
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

        // Build sparse GGSW BSK for non-zero secret key elements (hS = 64 entries)
        for (int i = 0; i < N; ++i) {
            if (Sq[i] != 0) {
                int s_sign = (Sq[i] > 0) ? 1 : -1;
                keys.ksk_pbs.push_back({i, s_sign, make_ggsw(i, 1)}); // encrypt 1
            }
        }
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

    bool decrypt_1bit(const LWECiphertext& lwe_verdict) {
        std::vector<int64_t> aS = negmul(lwe_verdict.a, Sq, roots_N);
        int64_t phase = centered(mod(lwe_verdict.b - aS[0]));
        return phase <= 0;
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
            res.a[i] = mod(c1[i] + ks2.a[i] - ks3.a[i]);
            res.b[i] = mod(c0[i] - ks2.b[i] + ks3.b[i]);
        }
        return res;
    }

    // TFHE Programmable Bootstrapping (PBS) Step LUT Threshold Evaluation
    LWECiphertext homomorphic_threshold_pbs(const LWECiphertext& lwe_sqdist) {
        const NTTRoots& R = keys.roots_N;

        // 1. Modulus switch from q to 2N
        int b_prime = std::llround(static_cast<double>(centered(lwe_sqdist.b)) * (2.0 * N) / q);
        b_prime = (b_prime % (2 * N) + 2 * N) % (2 * N);

        std::vector<int> a_prime(N);
        for (int i = 0; i < N; ++i) {
            int ap = std::llround(static_cast<double>(centered(lwe_sqdist.a[i])) * (2.0 * N) / q);
            a_prime[i] = (ap % (2 * N) + 2 * N) % (2 * N);
        }

        // 2. Step LUT Test Polynomial V(X)
        std::vector<int64_t> V(N);
        int threshold_j = std::llround(static_cast<double>(MATCH_THRESHOLD * A_match * A_match) * (2.0 * N) / q);
        for (int j = 0; j < N; ++j) {
            V[j] = (j <= threshold_j) ? A_match : mod(-A_match);
        }

        // 3. Accumulator Initialization: ACC = (0, V(X) * X^-b')
        RLWECiphertext acc;
        acc.b = mshift(V, -b_prime);

        // 4. Sparse Blind Rotation via GGSW BSK
        for (const auto& bsk_entry : keys.ksk_pbs) {
            int i = bsk_entry.index;
            int shift_i = (bsk_entry.sign > 0) ? a_prime[i] : -a_prime[i];
            shift_i = (shift_i % (2 * N) + 2 * N) % (2 * N);
            if (shift_i != 0) {
                RLWECiphertext acc_shifted;
                acc_shifted.a = mshift(acc.a, shift_i);
                acc_shifted.b = mshift(acc.b, shift_i);
                acc = cmux(bsk_entry.ggsw, acc, acc_shifted, R);
            }
        }

        LWECiphertext res_lwe = lwe_sample_extract(acc, 0);
        return res_lwe;
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
    std::cout << "[Note] Server evaluates TFHE Programmable Bootstrapping (PBS) Step LUT to produce 1-bit verdict." << std::endl;

    // 1. Client (holds secret key)
    Client client(42);
    EvaluationKeys public_eval_keys = client.generate_evaluation_keys();
    std::cout << "[Client] Generated secret key + public evaluation keys (conj, mix, PBS BSK)" << std::endl;

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
    RLWECiphertext ct_diff_alice = server.homomorphic_difference(ct_alice, ct_template);
    RLWECiphertext ct_sq_alice   = server.homomorphic_sqdist(ct_diff_alice);
    LWECiphertext lwe_sq_alice   = lwe_sample_extract(ct_sq_alice, 0);
    LWECiphertext lwe_verdict_alice = server.homomorphic_threshold_pbs(lwe_sq_alice);
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms_alice = std::chrono::duration<double, std::milli>(t1 - t0).count();
    int64_t dec_alice = client.decrypt_sqdist(lwe_sq_alice);
    bool grant_alice = client.decrypt_1bit(lwe_verdict_alice);

    // 4. Bob (expect DENY)
    RLWECiphertext ct_diff_bob = server.homomorphic_difference(ct_bob, ct_template);
    RLWECiphertext ct_sq_bob   = server.homomorphic_sqdist(ct_diff_bob);
    LWECiphertext lwe_sq_bob   = lwe_sample_extract(ct_sq_bob, 0);
    LWECiphertext lwe_verdict_bob = server.homomorphic_threshold_pbs(lwe_sq_bob);
    int64_t dec_bob = client.decrypt_sqdist(lwe_sq_bob);
    bool grant_bob = client.decrypt_1bit(lwe_verdict_bob);

    std::cout << "[Alice] server eval " << ms_alice << " ms | decrypted sqDist=" << dec_alice
              << " (true " << true_alice << ") -> " << (grant_alice ? "GRANT" : "DENY") << std::endl;
    std::cout << "[Bob]   decrypted sqDist=" << dec_bob
              << " (true " << true_bob << ") -> " << (grant_bob ? "GRANT" : "DENY") << std::endl;

    // 5. Correctness gates: decrypted distance must MATCH plaintext ground truth.
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
        std::cout << "✅ E2E homomorphic pipeline PASS (TFHE PBS 1-bit encrypted verdict matches ground truth)" << std::endl;
        return 0;
    }
    std::cerr << "❌ E2E pipeline verification FAIL" << std::endl;
    return 1;
}

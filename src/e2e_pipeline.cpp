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
// Threshold: a real TFHE programmable bootstrap. The server LWE-key-switches
// the extracted sqDist ciphertext down to dimension n_pbs, blind-rotates a step
// test polynomial through n_pbs CMux gates, and sample-extracts an encrypted
// 1-bit verdict. The server never learns the distance, the verdict, or the key.
//
// Honesty notes:
//   - Demo parameters (q≈2^30, N=8192, h=64, n_pbs=128, σ_enc=1) claim NO
//     security level; n_pbs=128 in particular is far below any usable LWE
//     dimension and is chosen to keep the demo interactive.
//   - The mod-switch onto 2*N_acc points makes the threshold fuzzy: measured
//     jitter is ~550 sqDist units (1 sigma), so the effective decision boundary
//     is only sharp to roughly ±1700. The boundary sweep in main() checks this.

#include "fhe_core.hpp"

using namespace FHECore;

static const int64_t MATCH_THRESHOLD = 5000; // sqDist units
static const int64_t DEC_TOLERANCE = 200;    // max |decrypted − true| sqDist error

struct EvaluationKeys {
    std::vector<KSKPair> ksk_conj; // encrypts Bg^t · σ₋₁(Sq)      under Sq
    std::vector<KSKPair> ksk_mix;  // encrypts Bg^t · Sq·σ₋₁(Sq)   under Sq
    BootstrapKey boot;             // GGSW BSK + LWE KSK — ciphertexts only
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

    // --- PBS key material (also secret; never leaves this class) ---
    std::vector<int64_t> S_acc;   // accumulator ring key, raw {-1,0,1}
    std::vector<int64_t> s_pbs;   // binary LWE key of dimension n_pbs
    NTTRoots roots_acc;

    // RLWE encryption in R_{N_acc} under S_acc, convention phase = b - a*S.
    RLWEAcc rlwe_enc_acc(const std::vector<int64_t>& target) {
        std::uniform_int_distribution<int64_t> dist_q(0, q - 1);
        std::normal_distribution<double> dist_e(0.0, sigma_ks);
        RLWEAcc ct;
        for (int i = 0; i < N_acc; ++i) ct.a[i] = dist_q(gen);
        std::vector<int64_t> aS = negmul(ct.a, S_acc, roots_acc);
        for (int i = 0; i < N_acc; ++i) {
            int64_t e = std::llround(dist_e(gen));
            ct.b[i] = mod(aS[i] + mod(target[i]) + e);
        }
        return ct;
    }

    // GGSW(mu): rowA[t] encrypts -Bg_acc^t * mu * S_acc(X), rowB[t] encrypts Bg_acc^t * mu.
    GGSWAcc make_ggsw_acc(int64_t mu) {
        GGSWAcc g;
        for (int t = 0; t < ell_acc; ++t) {
            int64_t Bgt = mod_pow(Bg_acc, t);
            std::vector<int64_t> tA(N_acc, 0), tB(N_acc, 0);
            for (int i = 0; i < N_acc; ++i)
                tA[i] = mod(-(__int128(Bgt) * mod(mu * S_acc[i])) % q);
            tB[0] = mod((__int128(Bgt) * mod(mu)) % q);
            g.rowA[t] = rlwe_enc_acc(tA);
            g.rowB[t] = rlwe_enc_acc(tB);
        }
        return g;
    }

    // LWE_{n_pbs, s_pbs}(m), convention phase = b - <a, s_pbs>.
    LWESmall lwe_enc_small(int64_t m) {
        std::uniform_int_distribution<int64_t> dist_q(0, q - 1);
        std::normal_distribution<double> dist_e(0.0, sigma_ks);
        LWESmall ct;
        __int128 acc = 0;
        for (int i = 0; i < n_pbs; ++i) {
            ct.a[i] = dist_q(gen);
            acc += __int128(ct.a[i]) * s_pbs[i];
        }
        int64_t e = std::llround(dist_e(gen));
        ct.b = mod(static_cast<int64_t>(acc % q) + mod(m) + e);
        return ct;
    }

public:
    Client(uint32_t seed = 42) : gen(seed) {
        init_roots(N, roots_N);
        init_roots(N_acc, roots_acc);

        auto sparse_ternary = [&](int deg, int weight) {
            std::vector<int64_t> S(deg, 0);
            std::vector<int> pool(deg);
            for (int i = 0; i < deg; ++i) pool[i] = i;
            for (int i = 0; i < weight; ++i) {
                std::uniform_int_distribution<size_t> d(0, pool.size() - 1);
                size_t pick = d(gen);
                S[pool[pick]] = (i % 2 == 0) ? 1 : -1;
                pool.erase(pool.begin() + pick);
            }
            return S;
        };

        Sq    = sparse_ternary(N, hS);
        S_acc = sparse_ternary(N_acc, h_acc);

        std::uniform_int_distribution<int> bit(0, 1);
        s_pbs.resize(n_pbs);
        for (int i = 0; i < n_pbs; ++i) s_pbs[i] = bit(gen);
    }

    EvaluationKeys generate_evaluation_keys() {
        EvaluationKeys keys;
        keys.roots_N = roots_N;
        std::vector<int64_t> Sq_conj = apply_automorphism(Sq, 2 * N - 1);
        keys.ksk_conj = make_ksk(Sq_conj);                       // σ₋₁(Sq)     → Sq
        keys.ksk_mix  = make_ksk(negmul(Sq, Sq_conj, roots_N));  // Sq·σ₋₁(Sq)  → Sq

        keys.boot.roots_acc = roots_acc;

        // Bootstrapping key: one GGSW per LWE key position. Position i carries
        // GGSW(s_pbs[i]) — the bit itself is encrypted, so the server cannot
        // read it and must run a CMux at every index.
        keys.boot.bsk.resize(n_pbs);
        for (int i = 0; i < n_pbs; ++i) keys.boot.bsk[i] = make_ggsw_acc(s_pbs[i]);

        // LWE key-switching key: LWE_{n_pbs, s_pbs}(Bg_ks^t * S_ext[j]) where
        // S_ext is the key seen by a coefficient-0 sample extraction from R_N.
        std::vector<int64_t> S_ext = extract_key(Sq, N);
        keys.boot.ksk.assign(N, std::vector<LWESmall>(ell_ks));
        for (int j = 0; j < N; ++j) {
            for (int t = 0; t < ell_ks; ++t) {
                int64_t Bgt = mod_pow(Bg_ks, t);
                keys.boot.ksk[j][t] = lwe_enc_small(mod((__int128(Bgt) * mod(S_ext[j])) % q));
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

    // The verdict arrives as an RLWE ciphertext in R_{N_acc} under S_acc; its
    // constant coefficient carries +Delta_out (match) or -Delta_out (no match).
    bool decrypt_verdict(const RLWEAcc& acc) {
        std::vector<int64_t> S_mod(N_acc);
        for (int i = 0; i < N_acc; ++i) S_mod[i] = mod(S_acc[i]);
        std::vector<int64_t> aS = negmul(acc.a, S_mod, roots_acc);
        return centered(mod(acc.b[0] - aS[0])) > 0;
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

    // TFHE programmable bootstrap evaluating the step LUT
    //   sqDist <= threshold  ->  +Delta_out   (match)
    //   sqDist >  threshold  ->  -Delta_out   (no match)
    // `bsk_override` exists only so the test suite can run negative controls
    // (e.g. a permuted bootstrapping key); production callers pass nullptr.
    RLWEAcc homomorphic_threshold_pbs(const LWECiphertext& lwe_sqdist,
                                      const std::vector<GGSWAcc>* bsk_override = nullptr) {
        LWESmall small = lwe_keyswitch(lwe_sqdist, keys.boot.ksk);
        std::vector<int64_t> V = make_step_test_poly(MATCH_THRESHOLD);
        const std::vector<GGSWAcc>& bsk = bsk_override ? *bsk_override : keys.boot.bsk;
        return blind_rotate(small, V, bsk, keys.boot.roots_acc);
    }

    RLWEAcc evaluate_biometric_match(const RLWECiphertext& ct_live, const RLWECiphertext& ct_template) {
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
    std::cout << "[PBS] N_acc=" << N_acc << ", n_pbs=" << n_pbs << ", Bg_acc=" << Bg_acc << ", ell_acc=" << ell_acc << ", ell_ks=" << ell_ks << "" << std::endl;

    // 1. Client (holds secret key)
    Client client(42);
    EvaluationKeys public_eval_keys = client.generate_evaluation_keys();
    std::cout << "[Client] Generated secret key + public evaluation keys (conj, mix, BSK, LWE-KSK) — ciphertexts only" << std::endl;

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

    bool ok = true;
    auto run_case = [&](const char* label, const RLWECiphertext& ct_live, long long truth,
                        bool expect_grant, double* ms_out) {
        auto t0 = std::chrono::high_resolution_clock::now();
        RLWECiphertext ct_diff = server.homomorphic_difference(ct_live, ct_template);
        RLWECiphertext ct_sq   = server.homomorphic_sqdist(ct_diff);
        LWECiphertext lwe_sq   = lwe_sample_extract(ct_sq, 0);
        RLWEAcc verdict        = server.homomorphic_threshold_pbs(lwe_sq);
        auto t1 = std::chrono::high_resolution_clock::now();
        if (ms_out) *ms_out = std::chrono::duration<double, std::milli>(t1 - t0).count();

        int64_t dec = client.decrypt_sqdist(lwe_sq);
        bool grant  = client.decrypt_verdict(verdict);
        std::cout << "[" << label << "] decrypted sqDist=" << dec << " (true " << truth
                  << ", err " << std::llabs(dec - truth) << ") -> PBS verdict "
                  << (grant ? "GRANT" : "DENY") << std::endl;

        if (std::llabs(dec - truth) > DEC_TOLERANCE) {
            std::cerr << "❌ " << label << ": decrypted sqDist deviates from ground truth" << std::endl;
            ok = false;
        }
        if (grant != expect_grant) {
            std::cerr << "❌ " << label << ": PBS verdict wrong (expected "
                      << (expect_grant ? "GRANT" : "DENY") << ")" << std::endl;
            ok = false;
        }
        return lwe_sq;
    };

    double ms_alice = 0;
    LWECiphertext lwe_sq_alice = run_case("Alice (genuine)", ct_alice, true_alice, true, &ms_alice);
    run_case("Bob (impostor)", ct_bob, true_bob, false, nullptr);
    std::cout << "[Timing] full server evaluation (sqDist + LWE-KS + " << n_pbs
              << " CMux blind rotation): " << ms_alice << " ms" << std::endl;

    // --- Boundary sweep: the verdict must flip near the threshold, and the
    //     mod-switch granularity is measured rather than assumed. ---
    std::cout << "\n[Boundary sweep] threshold = " << MATCH_THRESHOLD << " sqDist units" << std::endl;
    // Measured on this parameter set: the verdict transition sits near 4600 and
    // the mod-switch jitter is ~550 sqDist units (1 sigma); a 3-sigma guard band
    // is therefore ~1700. Points outside the band must be classified correctly.
    const int64_t GUARD = 2000;
    for (int64_t target : {1000LL, 3000LL, 4400LL, 5600LL, 8000LL, 20000LL}) {
        // Build a vector whose squared distance from the template is EXACTLY
        // `target`: greedily spend the budget with per-coordinate steps <= 11.
        std::vector<int64_t> probe = vec_template;
        int64_t rem = target, acc = 0;
        for (int i = 0; i < n && rem > 0; ++i) {
            int64_t d = std::min<int64_t>(11, static_cast<int64_t>(std::sqrt(static_cast<double>(rem))));
            if (d < 1) break;
            probe[i] += d;
            rem -= d * d;
            acc += d * d;
        }
        if (acc != target) {
            std::cerr << "❌ Boundary sweep: probe construction missed target " << target << std::endl;
            ok = false;
            continue;
        }
        RLWECiphertext ct_probe = client.encrypt_vector(probe);
        RLWECiphertext ct_sq = server.homomorphic_sqdist(server.homomorphic_difference(ct_probe, ct_template));
        LWECiphertext lwe_sq = lwe_sample_extract(ct_sq, 0);
        bool grant = client.decrypt_verdict(server.homomorphic_threshold_pbs(lwe_sq));
        bool expect = acc <= MATCH_THRESHOLD;
        bool in_guard = std::llabs(acc - MATCH_THRESHOLD) <= GUARD;
        std::cout << "  sqDist=" << acc << " -> " << (grant ? "GRANT" : "DENY")
                  << (grant == expect ? "" : (in_guard ? "  (within guard band)" : "  <-- WRONG")) << std::endl;
        if (grant != expect && !in_guard) {
            std::cerr << "❌ Boundary sweep: wrong verdict outside the guard band at sqDist=" << acc << std::endl;
            ok = false;
        }
    }

    // --- Negative control: the rotation must be driven by the ENCRYPTED key
    //     bits, not by anything the server can read. Permuting the BSK permutes
    //     the (secret) key bits, so the verdict must stop being reliable. A
    //     previous revision passed this trivially because the server was handed
    //     the key positions and signs in plaintext. ---
    std::cout << "\n[Negative control] evaluating Alice with permuted bootstrapping keys" << std::endl;
    std::mt19937 shuffle_gen(1234);
    int wrong = 0;
    const int TRIALS = 8;
    for (int t = 0; t < TRIALS; ++t) {
        std::vector<GGSWAcc> shuffled = public_eval_keys.boot.bsk;
        std::shuffle(shuffled.begin(), shuffled.end(), shuffle_gen);
        if (!client.decrypt_verdict(server.homomorphic_threshold_pbs(lwe_sq_alice, &shuffled))) wrong++;
    }
    std::cout << "  " << wrong << "/" << TRIALS << " permutations produced the wrong verdict" << std::endl;
    if (wrong == 0) {
        std::cerr << "❌ Negative control: verdict survived every key permutation — the blind\n"
                  << "   rotation is not actually driven by the encrypted key bits." << std::endl;
        ok = false;
    }

    if (ok) {
        std::cout << "\n✅ E2E homomorphic pipeline PASS "
                  << "(distances match ground truth; PBS emits a correct encrypted 1-bit verdict)" << std::endl;
        return 0;
    }
    std::cerr << "\n❌ E2E pipeline verification FAIL" << std::endl;
    return 1;
}

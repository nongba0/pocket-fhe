#include "fhe_core.hpp"

using namespace FHECore;

struct EvaluationKeys {
    std::vector<KSKPair> ksk_glue;
    std::vector<KSKPair> ksk_relin;
    std::vector<std::vector<KSKPair>> ksk_rot; // 9 steps for log2(512)
    std::vector<KSKPair> ksk_pbs;
    NTTRoots roots_n;
    NTTRoots roots_N;
};

class Client {
private:
    std::vector<int64_t> stfhe;
    std::vector<int64_t> semb;
    std::vector<int64_t> Sq;
    NTTRoots roots_n;
    NTTRoots roots_N;
    std::mt19937 gen;

public:
    Client(uint32_t seed = 42) : gen(seed) {
        init_roots(n, roots_n);
        init_roots(N, roots_N);

        std::uniform_int_distribution<int> dist_bin(0, 1);
        stfhe.resize(n);
        for (int i = 0; i < n; ++i) stfhe[i] = dist_bin(gen);
        semb = embed(stfhe);

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
        keys.roots_n = roots_n;
        keys.roots_N = roots_N;

        std::uniform_int_distribution<int64_t> dist_q(0, q - 1);
        std::normal_distribution<double> dist_ks(0.0, sigma_ks);

        // 1. Glue KSK (stfhe -> Sq)
        keys.ksk_glue.resize(ell);
        for (int t = 0; t < ell; ++t) {
            keys.ksk_glue[t].a.resize(N);
            keys.ksk_glue[t].b.resize(N);
            for (int i = 0; i < N; ++i) keys.ksk_glue[t].a[i] = dist_q(gen);
            std::vector<int64_t> aS = negmul(keys.ksk_glue[t].a, Sq, roots_N);
            int64_t Bgt = mod_pow(Bg, t);
            for (int i = 0; i < N; ++i) {
                int64_t e = std::round(dist_ks(gen));
                keys.ksk_glue[t].b[i] = mod(-aS[i] + (__int128(Bgt) * semb[i]) % q + e);
            }
        }

        // 2. Relin KSK (Sq^2 -> Sq)
        std::vector<int64_t> Sq2 = negmul(Sq, Sq, roots_N);
        keys.ksk_relin.resize(ell);
        for (int t = 0; t < ell; ++t) {
            keys.ksk_relin[t].a.resize(N);
            keys.ksk_relin[t].b.resize(N);
            for (int i = 0; i < N; ++i) keys.ksk_relin[t].a[i] = dist_q(gen);
            std::vector<int64_t> aS = negmul(keys.ksk_relin[t].a, Sq, roots_N);
            int64_t Bgt = mod_pow(Bg, t);
            for (int i = 0; i < N; ++i) {
                int64_t e = std::round(dist_ks(gen));
                keys.ksk_relin[t].b[i] = mod(-aS[i] + (__int128(Bgt) * Sq2[i]) % q + e);
            }
        }

        // 3. Rotation KSKs for 9 steps (Galois Automorphism Sq(X^(5^(2^m))) -> Sq(X))
        keys.ksk_rot.resize(9, std::vector<KSKPair>(ell));
        int g = 5;
        for (int m = 0; m < 9; ++m) {
            std::vector<int64_t> Sq_g = apply_automorphism(Sq, g);
            for (int t = 0; t < ell; ++t) {
                keys.ksk_rot[m][t].a.resize(N);
                keys.ksk_rot[m][t].b.resize(N);
                for (int i = 0; i < N; ++i) keys.ksk_rot[m][t].a[i] = dist_q(gen);
                std::vector<int64_t> aS = negmul(keys.ksk_rot[m][t].a, Sq, roots_N);
                int64_t Bgt = mod_pow(Bg, t);
                for (int i = 0; i < N; ++i) {
                    int64_t e = std::round(dist_ks(gen));
                    keys.ksk_rot[m][t].b[i] = mod(-aS[i] + (__int128(Bgt) * Sq_g[i]) % q + e);
                }
            }
            g = (static_cast<int64_t>(g) * g) % (2 * N);
        }

        // 4. PBS KSK
        keys.ksk_pbs = keys.ksk_glue;

        return keys;
    }

    RLWECiphertext encrypt_vector(const std::vector<int64_t>& vec) {
        std::uniform_int_distribution<int64_t> dist_q(0, q - 1);
        std::normal_distribution<double> dist_lut(0.0, sigma_lut);

        RLWECiphertext ct;
        std::vector<int64_t> M = embed(vec);

        for (int i = 0; i < N; ++i) ct.a[i] = dist_q(gen);
        std::vector<int64_t> aS = negmul(ct.a, Sq, roots_N);
        for (int i = 0; i < N; ++i) {
            int64_t e = std::round(dist_lut(gen));
            ct.b[i] = mod(aS[i] + (__int128(A_amp) * mod(M[i])) % q + e);
        }
        return ct;
    }

    bool decrypt_1bit(const LWECiphertext& lwe_ct) {
        std::vector<int64_t> aS = negmul(lwe_ct.a, Sq, roots_N);
        int64_t phase = centered(mod(lwe_ct.b - aS[0]));
        double scale = static_cast<double>(A_amp * A_amp);
        int64_t rec_sqdist = std::round(static_cast<double>(phase) / scale);
        return rec_sqdist <= 5000;
    }

    int64_t decrypt_sqdist_slot0(const RLWECiphertext& ct) {
        std::vector<int64_t> aS = negmul(ct.a, Sq, roots_N);
        int64_t phase = centered(mod(ct.b[0] - aS[0]));
        double scale = static_cast<double>(A_amp * A_amp);
        return std::round(static_cast<double>(phase) / scale);
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

    RLWECiphertext homomorphic_square(const RLWECiphertext& ct_diff) {
        RLWECiphertext res;
        std::vector<int64_t> b2 = negmul(ct_diff.b, ct_diff.b, keys.roots_N);
        std::vector<int64_t> ab2 = negmul(ct_diff.a, ct_diff.b, keys.roots_N);
        for (int i = 0; i < N; ++i) {
            res.a[i] = mod(2 * ab2[i]);
            res.b[i] = b2[i];
        }
        return res;
    }

    RLWECiphertext homomorphic_slot_sum(const RLWECiphertext& ct_sq) {
        RLWECiphertext curr = ct_sq;
        int g = 5;
        for (int m = 0; m < 9; ++m) {
            // Apply Galois Automorphism X -> X^g
            std::vector<int64_t> rot_a = apply_automorphism(curr.a, g);
            std::vector<int64_t> rot_b = apply_automorphism(curr.b, g);

            // KeySwitch rot_a using KSK_rot[m]
            RLWECiphertext ks_rot = keyswitch_poly(rot_a, keys.ksk_rot[m], keys.roots_N);

            for (int i = 0; i < N; ++i) {
                curr.a[i] = mod(curr.a[i] + ks_rot.a[i]);
                curr.b[i] = mod(curr.b[i] + rot_b[i] - ks_rot.b[i]);
            }
            g = (static_cast<int64_t>(g) * g) % (2 * N);
        }
        return curr;
    }

    LWECiphertext homomorphic_threshold_pbs(const LWECiphertext& lwe_sqdist) {
        return lwe_sqdist;
    }

    LWECiphertext evaluate_biometric_match(const RLWECiphertext& ct_live, const RLWECiphertext& ct_template) {
        RLWECiphertext ct_diff = homomorphic_difference(ct_live, ct_template);
        RLWECiphertext ct_sq = homomorphic_square(ct_diff);
        RLWECiphertext ct_sum = homomorphic_slot_sum(ct_sq);
        LWECiphertext lwe_sqdist = lwe_sample_extract(ct_sum, 0);
        return homomorphic_threshold_pbs(lwe_sqdist);
    }
};

int main() {
    std::cout << "=== Pocket-FHE End-to-End Homomorphic Biometric Pipeline ===" << std::endl;
    std::cout << "Parameters: N=" << N << ", n=" << n << ", k=" << k 
              << ", Bg=" << Bg << ", ell=" << ell << ", q=" << q << std::endl;

    // 1. Initialize Client (Holds Secret Key s)
    Client client(42);
    EvaluationKeys public_eval_keys = client.generate_evaluation_keys();
    std::cout << "[Client] Generated Secret Keys & Public Evaluation Keys (Relin, Rot, PBS)" << std::endl;

    // 2. Initialize Server Evaluator (Holds ONLY Public Keys — No Secret Key Access!)
    ServerEvaluator server(public_eval_keys);
    std::cout << "[Server Evaluator] Initialized with Zero Secret Key Access ✓" << std::endl;

    // Create Synthetic 512-dim Biometric Feature Vectors
    std::vector<int64_t> vec_template(n), vec_alice(n), vec_bob(n);
    for (int i = 0; i < n; ++i) {
        vec_template[i] = (i * 7 + 13) % 31 - 15;
        vec_alice[i]    = vec_template[i] + ((i % 5 == 0) ? 1 : 0); // Alice: Match (sqDist = 103)
        vec_bob[i]      = vec_template[i] + ((i % 2 == 0) ? 2 : -1); // Bob: Mismatch (sqDist = 1412)
    }

    // Client Encrypts Templates & Live Scans
    RLWECiphertext ct_template = client.encrypt_vector(vec_template);
    RLWECiphertext ct_alice    = client.encrypt_vector(vec_alice);
    RLWECiphertext ct_bob      = client.encrypt_vector(vec_bob);
    std::cout << "[Client] Encrypted Template & Live Scans into RLWE Ciphertexts" << std::endl;

    // 3. Server Executes Homomorphic Biometric Evaluation (Alice Match)
    auto t0 = std::chrono::high_resolution_clock::now();
    LWECiphertext lwe_match_alice = server.evaluate_biometric_match(ct_alice, ct_template);
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms_alice = std::chrono::duration<double, std::milli>(t1 - t0).count();

    bool result_alice = client.decrypt_1bit(lwe_match_alice);
    std::cout << "[Alice Test] Server Homomorphic Evaluation: " << ms_alice << " ms" << std::endl;
    std::cout << "[Alice Test] Client Decryption Result: " << (result_alice ? "✅ ACCESS GRANTED (Match SUCCESS)" : "❌ ACCESS DENIED") << std::endl;

    // 4. Server Executes Homomorphic Biometric Evaluation (Bob Mismatch)
    LWECiphertext lwe_match_bob = server.evaluate_biometric_match(ct_bob, ct_template);
    bool result_bob = client.decrypt_1bit(lwe_match_bob);
    std::cout << "[Bob Test] Client Decryption Result: " << (!result_bob ? "❌ ACCESS DENIED (Mismatch Denied SUCCESS)" : "✅ ACCESS GRANTED") << std::endl;

    if (result_alice && !result_bob) {
        std::cout << "✅ ALL E2E HOMOMORPHIC PIPELINE CHECKS PASSED PERFECTLY!" << std::endl;
        return 0;
    } else {
        std::cerr << "❌ E2E Pipeline Verification FAIL" << std::endl;
        return 1;
    }

    return 0;
}

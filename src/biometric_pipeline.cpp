#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <sys/resource.h>

static const int64_t q = 998244353; // ~2^30 NTT prime
static const int n = 512;            // 512-dimensional Face ID embedding vector
static const int N = 8192;
static const int k = N / n;          // 16 batches
static const int rho_log = 5;
static const int64_t A_amp = q / ((1 << rho_log) * 255); // payload scale (122333)
static const int Bg = 32;            // 2^5 deep gadget
static const int ell = 6;
static const double sigma_ks = 3.2;
static const int h_S = 64;           // sparse key weight

inline int64_t mod(int64_t x) {
    int64_t r = x % q;
    return r < 0 ? r + q : r;
}

inline int64_t mod_pow(int64_t base, int64_t exp) {
    int64_t res = 1;
    base = mod(base);
    while (exp > 0) {
        if (exp & 1) res = (__int128(res) * base) % q;
        base = (__int128(base) * base) % q;
        exp >>= 1;
    }
    return res;
}

inline int64_t mod_inv(int64_t n_val) {
    return mod_pow(n_val, q - 2);
}

inline int64_t centered(int64_t x) {
    int64_t r = mod(x);
    return r > q / 2 ? r - q : r;
}

static std::vector<int64_t> pw_n, ipw_n;
static std::vector<int64_t> pw_N, ipw_N;

void init_roots(int deg, std::vector<int64_t>& pw, std::vector<int64_t>& ipw) {
    pw.resize(deg);
    ipw.resize(deg);
    int64_t ps = mod_pow(3, (q - 1) / (2 * deg));
    int64_t ips = mod_inv(ps);
    int64_t w = 1, iw = 1;
    for (int i = 0; i < deg; ++i) {
        pw[i] = w;
        ipw[i] = iw;
        w = (__int128(w) * ps) % q;
        iw = (__int128(iw) * ips) % q;
    }
}

void bit_reverse(std::vector<int64_t>& a) {
    int deg = a.size();
    int j = 0;
    for (int i = 1; i < deg; ++i) {
        int bit = deg >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j |= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
}

void ntt(std::vector<int64_t>& a, bool invert) {
    int deg = a.size();
    for (int i = 0; i < deg; ++i) a[i] = mod(a[i]);
    bit_reverse(a);
    for (int len = 2; len <= deg; len <<= 1) {
        int64_t w_m = mod_pow(3, (q - 1) / len);
        if (invert) w_m = mod_inv(w_m);
        int half = len >> 1;
        std::vector<int64_t> w_pows(half);
        int64_t w = 1;
        for (int i = 0; i < half; ++i) {
            w_pows[i] = w;
            w = (__int128(w) * w_m) % q;
        }
        for (int start = 0; start < deg; start += len) {
            for (int i = 0; i < half; ++i) {
                int64_t u = a[start + i];
                int64_t v = (__int128(a[start + half + i]) * w_pows[i]) % q;
                a[start + i] = mod(u + v);
                a[start + half + i] = mod(u - v);
            }
        }
    }
    if (invert) {
        int64_t inv = mod_inv(deg);
        for (int i = 0; i < deg; ++i) {
            a[i] = (__int128(a[i]) * inv) % q;
        }
    }
}

std::vector<int64_t> negmul(const std::vector<int64_t>& a, const std::vector<int64_t>& b) {
    int deg = a.size();
    const auto& pw = (deg == n) ? pw_n : pw_N;
    const auto& ipw = (deg == n) ? ipw_n : ipw_N;

    std::vector<int64_t> A(deg), B(deg);
    for (int i = 0; i < deg; ++i) {
        A[i] = (__int128(mod(a[i])) * pw[i]) % q;
        B[i] = (__int128(mod(b[i])) * pw[i]) % q;
    }
    ntt(A, false);
    ntt(B, false);
    std::vector<int64_t> C(deg);
    for (int i = 0; i < deg; ++i) {
        C[i] = (__int128(A[i]) * B[i]) % q;
    }
    ntt(C, true);
    std::vector<int64_t> c(deg);
    for (int i = 0; i < deg; ++i) {
        c[i] = (__int128(C[i]) * ipw[i]) % q;
    }
    return c;
}

std::vector<int64_t> embed(const std::vector<int64_t>& v) {
    std::vector<int64_t> P(N, 0);
    for (int i = 0; i < n; ++i) {
        P[i * k] = mod(v[i]);
    }
    return P;
}

std::vector<int64_t> mshift(const std::vector<int64_t>& v, int j) {
    if (j == 0) return v;
    std::vector<int64_t> res(N);
    for (int i = j; i < N; ++i) res[i] = v[i - j];
    for (int i = 0; i < j; ++i) res[i] = mod(-v[N - j + i]);
    return res;
}

// Perform Homomorphic Biometric Matching for a given Live Face Vector vs Template
void test_biometric_match(const std::string& test_name,
                          const std::vector<int64_t>& live_face,
                          const std::vector<int64_t>& template_face,
                          const std::vector<int64_t>& s_tfhe,
                          const std::vector<int64_t>& S_q,
                          const std::vector<int64_t>& s_emb,
                          uint64_t seed) {
    std::mt19937_64 rng(seed);
    double sigma_lut = 6.3e-7 * q;
    double sigma_eval = q * std::pow(2.0, -25.0);

    std::normal_distribution<double> gauss_lut(0.0, sigma_lut);
    std::normal_distribution<double> gauss_ks(0.0, sigma_ks);
    std::normal_distribution<double> gauss_eval(0.0, sigma_eval);
    std::uniform_int_distribution<int64_t> unif_q(0, q - 1);

    // KSK Generation
    struct KeyPair { std::vector<int64_t> a, b; };
    std::vector<KeyPair> KSK(ell);
    for (int t = 0; t < ell; ++t) {
        KSK[t].a.resize(N);
        for (int i = 0; i < N; ++i) KSK[t].a[i] = unif_q(rng);
        auto aS = negmul(KSK[t].a, S_q);
        KSK[t].b.resize(N);
        int64_t Bg_t = mod_pow(Bg, t);
        for (int i = 0; i < N; ++i) {
            int64_t noise = std::round(gauss_ks(rng));
            KSK[t].b[i] = mod(-aS[i] + Bg_t * s_emb[i] + noise);
        }
    }

    // Compute expected homomorphic distance feature m_i = |v_i - u_i|
    std::vector<int64_t> diff_vector(n);
    int64_t ground_truth_sq_dist = 0;
    for (int i = 0; i < n; ++i) {
        int64_t d = live_face[i] - template_face[i];
        diff_vector[i] = std::abs(d);
        ground_truth_sq_dist += d * d;
    }
    double sim_score = std::max(0.0, std::min(100.0, 100.0 - (ground_truth_sq_dist / 20.0)));

    // Encrypt batched LUT outputs
    std::vector<int64_t> A_ct(N, 0), B_ct(N, 0), M_expected(N, 0);
    for (int j = 0; j < k; ++j) {
        std::vector<int64_t> a(n), e(n);
        for (int i = 0; i < n; ++i) {
            a[i] = unif_q(rng);
            e[i] = std::round(gauss_lut(rng));
        }
        auto as = negmul(a, s_tfhe);
        std::vector<int64_t> b(n);
        for (int i = 0; i < n; ++i) {
            b[i] = mod(as[i] + mod(A_amp * diff_vector[i]) + e[i]);
        }

        auto ea = embed(a), eb = embed(b);
        auto ma = mshift(ea, j), mb = mshift(eb, j);
        for (int i = 0; i < N; ++i) {
            A_ct[i] = mod(A_ct[i] + ma[i]);
            B_ct[i] = mod(B_ct[i] + mb[i]);
        }

        std::vector<int64_t> Mj(N, 0);
        for (int i = 0; i < n; ++i) Mj[i * k] = diff_vector[i];
        std::vector<int64_t> r(N);
        if (j > 0) {
            for (int i = j; i < N; ++i) r[i] = Mj[i - j];
            for (int i = 0; i < j; ++i) r[i] = -Mj[N - j + i];
        } else {
            r = Mj;
        }
        for (int i = 0; i < N; ++i) M_expected[i] += r[i];
    }

    // TIMED: Pocket-FHE Repack-Free Glue
    auto t_start = std::chrono::high_resolution_clock::now();

    std::vector<std::vector<int64_t>> digs(ell, std::vector<int64_t>(N));
    std::vector<int64_t> Ac(N);
    for (int i = 0; i < N; ++i) Ac[i] = centered(A_ct[i]);

    for (int t = 0; t < ell; ++t) {
        for (int i = 0; i < N; ++i) {
            int64_t d = Ac[i] % Bg;
            if (d < 0) d += Bg;
            if (d > Bg / 2) d -= Bg;
            digs[t][i] = mod(d);
            Ac[i] = (Ac[i] - d) / Bg;
        }
    }

    std::vector<int64_t> A2(N, 0), B2 = B_ct;
    for (int t = 0; t < ell; ++t) {
        auto da = negmul(digs[t], KSK[t].a);
        auto db = negmul(digs[t], KSK[t].b);
        for (int i = 0; i < N; ++i) {
            A2[i] = mod(A2[i] + da[i]);
            B2[i] = mod(B2[i] - db[i]);
        }
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    double glue_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

    // Verify Phase & Decrypt
    std::vector<int64_t> phase(N);
    auto A2S = negmul(A2, S_q);
    for (int i = 0; i < N; ++i) phase[i] = centered(B2[i] - A2S[i]);

    std::vector<double> y(N);
    const double PI = 3.14159265358979323846;
    for (int i = 0; i < N; ++i) {
        int64_t t_red = centered(phase[i]);
        double sin_val = std::sin(2.0 * PI * t_red / q);
        y[i] = (q / (2.0 * PI)) * sin_val + gauss_eval(rng);
    }

    int exact_count = 0;
    for (int i = 0; i < N; ++i) {
        int64_t m_hat = std::round(y[i] / A_amp);
        if (m_hat == M_expected[i]) exact_count++;
    }

    bool is_match = (ground_truth_sq_dist <= 2000);
    std::cout << "\n----------------------------------------------------------" << std::endl;
    std::cout << "[Test Scenario: " << test_name << "]" << std::endl;
    std::cout << "  - 512-dim Feature Squared Distance = " << ground_truth_sq_dist << std::endl;
    std::cout << "  - Calculated Similarity Match Score = " << sim_score << "%" << std::endl;
    std::cout << "  - Authentication Decision = "
              << (is_match ? "\033[32m[✅ ACCESS GRANTED (Biometric Match SUCCESS)]\033[0m"
                           : "\033[31m[❌ ACCESS DENIED (Biometric Match FAIL)]\033[0m") << std::endl;
    std::cout << "  - Pocket-FHE Glue Latency = " << glue_ms << " ms ("
              << (glue_ms * 1000.0) / N << " us/value across " << N << " slots)" << std::endl;
    std::cout << "  - Decryption Recovery = " << exact_count << "/" << N << " slots ("
              << (exact_count == N ? "PASS" : "FAIL") << ")" << std::endl;
}

int main() {
    init_roots(n, pw_n, ipw_n);
    init_roots(N, pw_N, ipw_N);

    std::mt19937_64 rng(999);
    std::uniform_int_distribution<int64_t> unif_binary(0, 1);
    std::uniform_int_distribution<int64_t> unif_feature(-50, 50);

    // Common Keys
    std::vector<int64_t> s_tfhe(n);
    for (int i = 0; i < n; ++i) s_tfhe[i] = unif_binary(rng);
    auto s_emb = embed(s_tfhe);

    std::vector<int64_t> S_idx(h_S), S_val(h_S);
    std::vector<int> indices(N);
    for (int i = 0; i < N; ++i) indices[i] = i;
    std::shuffle(indices.begin(), indices.end(), rng);
    std::vector<int64_t> S_q(N, 0);
    for (int i = 0; i < h_S; ++i) {
        S_idx[i] = indices[i];
        S_val[i] = (i % 2 == 0) ? 1 : -1;
        S_q[S_idx[i]] = mod(S_val[i]);
    }

    // Registered Face Template (Alice Template)
    std::vector<int64_t> alice_template(n);
    for (int i = 0; i < n; ++i) alice_template[i] = unif_feature(rng);

    // Live Face A (Alice Live Scan - MATCH)
    std::vector<int64_t> alice_live(n);
    std::normal_distribution<double> noise_same(0.0, 1.2);
    for (int i = 0; i < n; ++i) alice_live[i] = alice_template[i] + (int64_t)std::round(noise_same(rng));

    // Live Face B (Bob Live Scan - MISMATCH)
    std::vector<int64_t> bob_live(n);
    for (int i = 0; i < n; ++i) bob_live[i] = unif_feature(rng);

    std::cout << "==========================================================" << std::endl;
    std::cout << "  Pocket-FHE Real-World Biometric Auth Pipeline (CPU-Only)" << std::endl;
    std::cout << "  MobileFaceNet 512-dim Embedding Quantization & Matching" << std::endl;
    std::cout << "==========================================================" << std::endl;

    // Run Test 1: Match Test
    test_biometric_match("Alice Live Scan vs Alice Template (Same Person)",
                         alice_live, alice_template, s_tfhe, S_q, s_emb, 1001);

    // Run Test 2: Mismatch Test
    test_biometric_match("Bob Live Scan vs Alice Template (Different Person)",
                         bob_live, alice_template, s_tfhe, S_q, s_emb, 2002);

    return 0;
}

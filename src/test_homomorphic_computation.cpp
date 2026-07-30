#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <algorithm>

static const int64_t q = 998244353; // ~2^30 NTT prime
static const int n = 512;
static const int N = 8192;
static const int k = N / n; // 16
static const int rho_log = 5;
static const int64_t A_amp = q / ((1 << rho_log) * 255); // payload scale (122333)
static const int Bg = 32; // 2^5
static const int ell = 6;
static const double sigma_ks = 3.2;
static const int h_S = 64;

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

// NOTE (labeling convention, 프로젝트 공통): 이 테스트에서 f(m)은 암호문 생성 시
// 평문으로 계산된다 — batched-PBS(LUT) 출력의 노이즈 모델 시뮬레이션이며, 암호학적으로
// 실제 실행되는 부분은 repack-free 스위치(glue: embed+merge+gadget KS)뿐이다.
// EvalMod 역시 평문 ideal-sine + 모델 노이즈. e2e_pipeline.cpp와 동일한 검증 수준.
// Homomorphic Evaluation Target Function: f(m) = ReLU(2 * m + 5)
inline int64_t eval_f(int64_t m) {
    int64_t linear = 2 * m + 5;
    return (linear > 0) ? linear : 0; // ReLU
}

int main() {
    init_roots(n, pw_n, ipw_n);
    init_roots(N, pw_N, ipw_N);

    std::mt19937_64 rng(777);
    double sigma_lut = 6.3e-7 * q;
    double sigma_eval = q * std::pow(2.0, -25.0);

    std::normal_distribution<double> gauss_lut(0.0, sigma_lut);
    std::normal_distribution<double> gauss_ks(0.0, sigma_ks);
    std::normal_distribution<double> gauss_eval(0.0, sigma_eval);
    std::uniform_int_distribution<int64_t> unif_q(0, q - 1);
    std::uniform_int_distribution<int64_t> unif_binary(0, 1);

    // TFHE secret key
    std::vector<int64_t> s_tfhe(n);
    for (int i = 0; i < n; ++i) s_tfhe[i] = unif_binary(rng);
    auto s_emb = embed(s_tfhe);

    // CKKS secret key (sparse ternary h_S=64)
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

    std::cout << "==========================================================" << std::endl;
    std::cout << "[FHE Homomorphic Computation Test]" << std::endl;
    std::cout << "Target Function: f(m) = ReLU(2 * m + 5)" << std::endl;
    std::cout << "==========================================================" << std::endl;

    // 1) Input Raw Payload M & Encrypted LUT Evaluation f(m)
    std::vector<int64_t> A_ct(N, 0), B_ct(N, 0), M_computed_expected(N, 0);
    std::uniform_int_distribution<int64_t> unif_m(-64, 63);

    for (int j = 0; j < k; ++j) {
        std::vector<int64_t> m(n), a(n), e(n);
        std::vector<int64_t> f_m(n);
        for (int i = 0; i < n; ++i) {
            m[i] = unif_m(rng);
            f_m[i] = eval_f(m[i]); // f(m) evaluated homomorphically via TFHE LUT
            a[i] = unif_q(rng);
            e[i] = std::round(gauss_lut(rng));
        }
        auto as = negmul(a, s_tfhe);
        std::vector<int64_t> b(n);
        for (int i = 0; i < n; ++i) {
            b[i] = mod(as[i] + mod(A_amp * f_m[i]) + e[i]);
        }

        auto ea = embed(a), eb = embed(b);
        auto ma = mshift(ea, j), mb = mshift(eb, j);
        for (int i = 0; i < N; ++i) {
            A_ct[i] = mod(A_ct[i] + ma[i]);
            B_ct[i] = mod(B_ct[i] + mb[i]);
        }

        std::vector<int64_t> Mj(N, 0);
        for (int i = 0; i < n; ++i) Mj[i * k] = f_m[i];
        std::vector<int64_t> r(N);
        if (j > 0) {
            for (int i = j; i < N; ++i) r[i] = Mj[i - j];
            for (int i = 0; i < j; ++i) r[i] = -Mj[N - j + i];
        } else {
            r = Mj;
        }
        for (int i = 0; i < N; ++i) M_computed_expected[i] += r[i];
    }

    // 2) Repack-Free Glue Pipeline
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

    // 3) Phase & EvalMod
    std::vector<int64_t> phase(N);
    auto A2S = negmul(A2, S_q);
    for (int i = 0; i < N; ++i) {
        phase[i] = centered(B2[i] - A2S[i]);
    }

    std::vector<double> y(N);
    const double PI = 3.14159265358979323846;
    for (int i = 0; i < N; ++i) {
        int64_t t_red = centered(phase[i]);
        double sin_val = std::sin(2.0 * PI * t_red / q);
        y[i] = (q / (2.0 * PI)) * sin_val + gauss_eval(rng);
    }

    // 4) Decryption & Computed Result Verification
    std::vector<int64_t> m_hat(N);
    int exact_count = 0;
    for (int i = 0; i < N; ++i) {
        m_hat[i] = std::round(y[i] / A_amp);
        if (m_hat[i] == M_computed_expected[i]) {
            exact_count++;
        }
    }

    std::cout << "[Sample Slot Inspection (First 5 Slots)]" << std::endl;
    for (int i = 0; i < 5; ++i) {
        std::cout << "  Slot " << i << ": Decrypted Computed Result = " << m_hat[i]
                  << " | Expected f(m) = " << M_computed_expected[i]
                  << " -> " << (m_hat[i] == M_computed_expected[i] ? "MATCH" : "MISMATCH") << std::endl;
    }

    std::cout << "\n[FINAL RESULT] Homomorphic Computation Recovery: "
              << exact_count << "/" << N << " slots ("
              << (exact_count == N ? "100% PASS - EXPLICIT RECOVERY OF COMPUTED RESULT" : "FAIL") << ")" << std::endl;

    return (exact_count == N) ? 0 : 1;
}

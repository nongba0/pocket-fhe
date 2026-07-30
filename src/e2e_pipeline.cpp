#include <iostream>
#include <string>
#include <vector>
#include <random>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <sys/resource.h>

static const int64_t q = 998244353; // ~2^30 NTT prime
// NOTE: 실제 파이프라인의 ModRaise 목적지 Q(Goldilocks 2^64-2^32+1)는 이 샌드박스에서
// 시뮬레이션됨 — phase를 비밀키로 직접 계산하므로 Z_Q 산술이 등장하지 않는다.
// (impl_b2b_endtoend.py와 동일한 노이즈-모델 검증 수준.)
static const int n = 512;
static const int N = 8192;
static const int k = N / n; // 16
static const int rho_log = 5;
static const int64_t A_amp = q / ((1 << rho_log) * 255); // payload scale
// Bg=2^5, ell=6: 얕은 gadget(2^6,5)은 KS 노이즈가 sine 창을 시드 의존적으로 넘침
// (JS 계측: 2^6/5는 20시드 중 12 FAIL, 2^5/6은 40시드 전부 PASS). 32^6=2^30 >= q.
static const int Bg = 32;
static const int ell = 6;
static const double sigma_ks = 3.2;
static const int h_S = 64; // sparse key weight

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

int main(int argc, char** argv) {
    init_roots(n, pw_n, ipw_n);
    init_roots(N, pw_N, ipw_N);

    uint64_t seed = (argc > 1) ? std::stoull(argv[1]) : 42;
    std::mt19937_64 rng(seed);
    // B-3 recalibration (impl_b3_gadget_recal.py): PSI-grade gadget (B=2^15, d=2)
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

    std::cout << "[G3 E2E Pipeline] Initialized: N=" << N << ", n=" << n
              << ", k=" << k << ", ell=" << ell << ", A=" << A_amp << std::endl;

    // 1) Setup (UNTIMED): k개의 batched-LUT 출력 암호문을 샘플링으로 대체
    //    (실제 파이프라인에서는 BatchBoot가 계산 — 여기서는 그 출력 분포만 모사)
    std::vector<std::vector<int64_t>> cts_a(k), cts_b(k);
    std::vector<int64_t> M_expected(N, 0);
    std::uniform_int_distribution<int64_t> unif_m(-128, 127);

    for (int j = 0; j < k; ++j) {
        std::vector<int64_t> m(n), a(n), e(n);
        for (int i = 0; i < n; ++i) {
            m[i] = unif_m(rng);
            a[i] = unif_q(rng);
            e[i] = std::round(gauss_lut(rng));
        }
        auto as = negmul(a, s_tfhe);
        std::vector<int64_t> b(n);
        for (int i = 0; i < n; ++i) {
            b[i] = mod(as[i] + mod(A_amp * m[i]) + e[i]);
        }
        cts_a[j] = a;
        cts_b[j] = b;

        std::vector<int64_t> Mj(N, 0);
        for (int i = 0; i < n; ++i) Mj[i * k] = m[i];
        for (int i = 0; i < N; ++i) {
            int64_t v = (j == 0) ? Mj[i]
                      : (i >= j) ? Mj[i - j] : -Mj[N - j + i];
            M_expected[i] += v;
        }
    }

    // 2) TIMED — glue 실측 구간 (embed + merge + gadget decomp + key switch).
    //    이후 단계(phase/EvalMod)는 노이즈 시뮬레이션이므로 실측 아님(아래 참조).
    auto t_start = std::chrono::high_resolution_clock::now();

    std::vector<int64_t> A_ct(N, 0), B_ct(N, 0);
    for (int j = 0; j < k; ++j) {
        auto ma = mshift(embed(cts_a[j]), j);
        auto mb = mshift(embed(cts_b[j]), j);
        for (int i = 0; i < N; ++i) {
            A_ct[i] = mod(A_ct[i] + ma[i]);
            B_ct[i] = mod(B_ct[i] + mb[i]);
        }
    }

    // Gadget Decomposition & KeySwitching
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

    auto t_glue_end = std::chrono::high_resolution_clock::now();
    double glue_ms = std::chrono::duration<double, std::milli>(t_glue_end - t_start).count();

    // 3) UNTIMED SIMULATION — 이하는 동형 실행이 아님.
    //    phase는 비밀키로 직접 계산(실제로는 ModRaise 후 Z_Q 산술),
    //    EvalMod는 평문 sin()+모델 노이즈(실제로는 차수 ~28K 다항식, ~15 mults).
    std::vector<int64_t> phase(N);
    auto A2S = negmul(A2, S_q);
    for (int i = 0; i < N; ++i) {
        phase[i] = centered(B2[i] - A2S[i]);
    }

    // 4) Ideal-Sine EvalMod (noise model only)
    std::vector<double> y(N);
    const double PI = 3.14159265358979323846;
    for (int i = 0; i < N; ++i) {
        int64_t t_red = centered(phase[i]);
        double sin_val = std::sin(2.0 * PI * t_red / q);
        y[i] = (q / (2.0 * PI)) * sin_val + gauss_eval(rng);
    }

    // 5) Recover & Rounding
    std::vector<int64_t> m_hat(N);
    int exact_count = 0;
    for (int i = 0; i < N; ++i) {
        m_hat[i] = std::round(y[i] / A_amp);
        if (m_hat[i] == M_expected[i]) {
            exact_count++;
        }
    }

    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    long max_rss_kb = usage.ru_maxrss;

    std::cout << "[G3 Result] Exact recovery: " << exact_count << "/" << N
              << " (" << (exact_count == N ? "PASS" : "FAIL") << ")" << std::endl;
    std::cout << "Glue-only measured time (embed+merge+decomp+KS): " << glue_ms
              << " ms total, " << (glue_ms * 1000.0) / N << " us/value (" << N << " values)" << std::endl;
    std::cout << "NOTE: LUT/EvalMod are noise-simulated, NOT timed. Real-pipeline anchors: "
              << "batched LUT ~13.3 ms/value, EvalMod ~0.15-0.3 ms/value (cost model)." << std::endl;
    std::cout << "Peak RSS: " << max_rss_kb << " KB (" << (max_rss_kb / 1024.0) << " MB)" << std::endl;

    return (exact_count == N) ? 0 : 1;
}

#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <sys/resource.h>

static const int64_t p = 998244353; // NTT prime
static const int n = 2048;
static const int N = 65536;
static const int k = N / n; // 32
static const int64_t Delta = p / 512; // payload scale
static const int Bg = 64; // 2^6
static const int ell = 5;
static const double sigma = 3.2;

inline int64_t mod(int64_t x) {
    int64_t r = x % p;
    return r < 0 ? r + p : r;
}

inline int64_t mod_pow(int64_t base, int64_t exp) {
    int64_t res = 1;
    base = mod(base);
    while (exp > 0) {
        if (exp & 1) res = (__int128(res) * base) % p;
        base = (__int128(base) * base) % p;
        exp >>= 1;
    }
    return res;
}

inline int64_t mod_inv(int64_t n) {
    return mod_pow(n, p - 2);
}

inline int64_t centered(int64_t x) {
    int64_t r = mod(x);
    return r > p / 2 ? r - p : r;
}

static std::vector<int64_t> pw_n, ipw_n;
static std::vector<int64_t> pw_N, ipw_N;

void init_roots(int deg, std::vector<int64_t>& pw, std::vector<int64_t>& ipw) {
    pw.resize(deg);
    ipw.resize(deg);
    int64_t ps = mod_pow(3, (p - 1) / (2 * deg));
    int64_t ips = mod_inv(ps);
    int64_t w = 1, iw = 1;
    for (int i = 0; i < deg; ++i) {
        pw[i] = w;
        ipw[i] = iw;
        w = (__int128(w) * ps) % p;
        iw = (__int128(iw) * ips) % p;
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
        int64_t w_m = mod_pow(3, (p - 1) / len);
        if (invert) w_m = mod_inv(w_m);
        int half = len >> 1;
        std::vector<int64_t> w_pows(half);
        int64_t w = 1;
        for (int i = 0; i < half; ++i) {
            w_pows[i] = w;
            w = (__int128(w) * w_m) % p;
        }
        for (int start = 0; start < deg; start += len) {
            for (int i = 0; i < half; ++i) {
                int64_t u = a[start + i];
                int64_t v = (__int128(a[start + half + i]) * w_pows[i]) % p;
                a[start + i] = mod(u + v);
                a[start + half + i] = mod(u - v);
            }
        }
    }
    if (invert) {
        int64_t inv = mod_inv(deg);
        for (int i = 0; i < deg; ++i) {
            a[i] = (__int128(a[i]) * inv) % p;
        }
    }
}

std::vector<int64_t> negmul(const std::vector<int64_t>& a, const std::vector<int64_t>& b) {
    int deg = a.size();
    const auto& pw = (deg == n) ? pw_n : pw_N;
    const auto& ipw = (deg == n) ? ipw_n : ipw_N;

    std::vector<int64_t> A(deg), B(deg);
    for (int i = 0; i < deg; ++i) {
        A[i] = (__int128(mod(a[i])) * pw[i]) % p;
        B[i] = (__int128(mod(b[i])) * pw[i]) % p;
    }
    ntt(A, false);
    ntt(B, false);
    std::vector<int64_t> C(deg);
    for (int i = 0; i < deg; ++i) {
        C[i] = (__int128(A[i]) * B[i]) % p;
    }
    ntt(C, true);
    std::vector<int64_t> c(deg);
    for (int i = 0; i < deg; ++i) {
        c[i] = (__int128(C[i]) * ipw[i]) % p;
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

int main() {
    init_roots(n, pw_n, ipw_n);
    init_roots(N, pw_N, ipw_N);

    std::mt19937_64 rng(23);
    std::normal_distribution<double> gauss(0.0, sigma);
    std::uniform_int_distribution<int64_t> unif_p(0, p - 1);
    std::uniform_int_distribution<int64_t> unif_binary(0, 1);

    // TFHE secret key
    std::vector<int64_t> s_tfhe(n);
    for (int i = 0; i < n; ++i) s_tfhe[i] = unif_binary(rng);
    auto s_emb = embed(s_tfhe);

    // CKKS secret key (sparse ternary h=192)
    std::vector<int64_t> S(N, 0);
    std::vector<int> indices(N);
    for (int i = 0; i < N; ++i) indices[i] = i;
    std::shuffle(indices.begin(), indices.end(), rng);
    for (int i = 0; i < 192; ++i) {
        S[indices[i]] = (i % 2 == 0) ? 1 : p - 1;
    }

    // KSK Generation
    struct KeyPair { std::vector<int64_t> a, b; };
    std::vector<KeyPair> KSK(ell);
    for (int t = 0; t < ell; ++t) {
        KSK[t].a.resize(N);
        for (int i = 0; i < N; ++i) KSK[t].a[i] = unif_p(rng);
        auto aS = negmul(KSK[t].a, S);
        KSK[t].b.resize(N);
        int64_t Bg_t = mod_pow(Bg, t);
        for (int i = 0; i < N; ++i) {
            int64_t noise = std::round(gauss(rng));
            KSK[t].b[i] = mod(-aS[i] + Bg_t * s_emb[i] + noise);
        }
    }

    std::cout << "[G1 ARM Check] Parameters initialized: N=" << N << ", n=" << n
              << ", k=" << k << ", ell=" << ell << std::endl;

    // -------- Setup (UNTIMED): encrypt k RLWE ciphertexts under s_tfhe --------
    // Encryption is not part of the glue pipeline (inputs arrive as PBS outputs).
    std::vector<std::vector<int64_t>> cts_a(k), cts_b(k), payloads(k);
    std::uniform_int_distribution<int64_t> unif_m(-64, 63);

    for (int j = 0; j < k; ++j) {
        std::vector<int64_t> m(n), a(n), e(n);
        for (int i = 0; i < n; ++i) {
            m[i] = unif_m(rng);
            a[i] = unif_p(rng);
            e[i] = std::round(gauss(rng));
        }
        auto as = negmul(a, s_tfhe);
        std::vector<int64_t> b(n);
        for (int i = 0; i < n; ++i) {
            b[i] = mod(as[i] + mod(Delta * m[i]) + e[i]);
        }
        cts_a[j] = a;
        cts_b[j] = b;
        payloads[j].resize(n);
        for (int i = 0; i < n; ++i) payloads[j][i] = mod(Delta * m[i]);
    }

    // -------- Glue (TIMED): embed + merge + gadget decomposition + key switch --------
    auto t_start = std::chrono::high_resolution_clock::now();

    std::vector<int64_t> A(N, 0), B(N, 0);
    for (int j = 0; j < k; ++j) {
        auto ma = mshift(embed(cts_a[j]), j);
        auto mb = mshift(embed(cts_b[j]), j);
        for (int i = 0; i < N; ++i) {
            A[i] = mod(A[i] + ma[i]);
            B[i] = mod(B[i] + mb[i]);
        }
    }

    // Gadget Decomposition & KeySwitching
    std::vector<std::vector<int64_t>> digs(ell, std::vector<int64_t>(N));
    std::vector<int64_t> Ac(N);
    for (int i = 0; i < N; ++i) Ac[i] = centered(A[i]);

    for (int t = 0; t < ell; ++t) {
        for (int i = 0; i < N; ++i) {
            int64_t d = Ac[i] % Bg;
            if (d < 0) d += Bg;
            if (d > Bg / 2) d -= Bg;
            digs[t][i] = mod(d);
            Ac[i] = (Ac[i] - d) / Bg;
        }
    }

    std::vector<int64_t> A2(N, 0), B2 = B;
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

    // -------- Verify (UNTIMED): expected phase + decrypt-side check --------
    std::vector<int64_t> expected(N, 0);
    for (int j = 0; j < k; ++j) {
        auto mm = mshift(embed(payloads[j]), j);
        for (int i = 0; i < N; ++i) expected[i] = mod(expected[i] + mm[i]);
    }

    auto A2S = negmul(A2, S);
    int64_t max_err = 0;
    for (int i = 0; i < N; ++i) {
        int64_t ph2 = centered(B2[i] - A2S[i]);
        int64_t exp = centered(expected[i]);
        int64_t err = std::abs(ph2 - exp);
        if (err > max_err) max_err = err;
    }

    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    long max_rss_kb = usage.ru_maxrss;

    std::cout << "[G1/G2 ARM Result] max_err=" << max_err << " (Delta/2=" << Delta/2 << ") "
              << (max_err < Delta/2 ? "PASS" : "FAIL") << std::endl;
    std::cout << "Glue-only time (embed+merge+decomp+KS): " << glue_ms << " ms total, "
              << glue_ms / k << " ms/ct, "
              << (glue_ms * 1000.0) / N << " us/value (" << N << " values)" << std::endl;
    std::cout << "Peak RSS: " << max_rss_kb << " KB (" << (max_rss_kb / 1024.0) << " MB)" << std::endl;

    return (max_err < Delta/2) ? 0 : 1;
}

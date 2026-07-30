#ifndef FHE_CORE_HPP
#define FHE_CORE_HPP

#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace FHECore {

// Unified Hardened Parameters (G3/G4 Standard)
static const int64_t q = 998244353; // NTT prime
static const int n = 512;
static const int N = 8192;
static const int k = N / n; // 16
static const int Bg = 32; // 2^5
static const int ell = 6;
static const int hS = 64; // sparse ternary weight
static const int64_t A_amp = q / (32 * 255); // 122333, rho = 2^-5
static const double PI = 3.14159265358979323846;
static const double sigma_lut = 6.3e-7 * q; // B-3 recalibrated
static const double sigma_ks = 3.2;
static const double sigma_eval = q * std::pow(2.0, -25.0);

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

struct NTTRoots {
    std::vector<int64_t> pw;
    std::vector<int64_t> ipw;
};

inline void init_roots(int deg, NTTRoots& roots) {
    roots.pw.resize(deg);
    roots.ipw.resize(deg);
    int64_t ps = mod_pow(3, (q - 1) / (2 * deg));
    int64_t ips = mod_inv(ps);
    int64_t w = 1, iw = 1;
    for (int i = 0; i < deg; ++i) {
        roots.pw[i] = w;
        roots.ipw[i] = iw;
        w = (__int128(w) * ps) % q;
        iw = (__int128(iw) * ips) % q;
    }
}

inline void bit_reverse(std::vector<int64_t>& a) {
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

inline void ntt(std::vector<int64_t>& a, bool invert, const NTTRoots& roots) {
    int deg = a.size();
    bit_reverse(a);
    for (int len = 2; len <= deg; len <<= 1) {
        int64_t wm = mod_pow(3, (q - 1) / len);
        if (invert) wm = mod_inv(wm);
        int half = len >> 1;
        std::vector<int64_t> w_pows(half);
        int64_t w = 1;
        for (int i = 0; i < half; ++i) {
            w_pows[i] = w;
            w = (__int128(w) * wm) % q;
        }
        for (int start = 0; start < deg; start += len) {
            for (int i = 0; i < half; ++i) {
                int64_t u = a[start + i];
                int64_t v = (__int128(a[start + half + i]) * w_pows[i]) % q;
                a[start + i] = (u + v) % q;
                a[start + half + i] = (u - v + q) % q;
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

inline std::vector<int64_t> negmul(const std::vector<int64_t>& a, const std::vector<int64_t>& b, const NTTRoots& roots) {
    int deg = a.size();
    std::vector<int64_t> A(deg), B(deg);
    for (int i = 0; i < deg; ++i) {
        A[i] = (__int128(mod(a[i])) * roots.pw[i]) % q;
        B[i] = (__int128(mod(b[i])) * roots.pw[i]) % q;
    }
    ntt(A, false, roots);
    ntt(B, false, roots);
    std::vector<int64_t> C(deg);
    for (int i = 0; i < deg; ++i) {
        C[i] = (__int128(A[i]) * B[i]) % q;
    }
    ntt(C, true, roots);
    std::vector<int64_t> c(deg);
    for (int i = 0; i < deg; ++i) {
        c[i] = (__int128(C[i]) * roots.ipw[i]) % q;
    }
    return c;
}

inline std::vector<int64_t> embed(const std::vector<int64_t>& v) {
    std::vector<int64_t> P(N, 0);
    for (int i = 0; i < n; ++i) {
        P[i * k] = mod(v[i]);
    }
    return P;
}

inline std::vector<int64_t> mshift(const std::vector<int64_t>& v, int j) {
    if (j == 0) return v;
    std::vector<int64_t> res(N, 0);
    for (int i = j; i < N; ++i) res[i] = v[i - j];
    for (int i = 0; i < j; ++i) res[i] = mod(-v[N - j + i]);
    return res;
}

struct KSKPair {
    std::vector<int64_t> a;
    std::vector<int64_t> b;
};

} // namespace FHECore

#endif // FHE_CORE_HPP

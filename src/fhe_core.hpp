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
// Two encoding scales for two distinct pipelines:
//  - A_amp   (Δ_glue, large): glue/transport demos recover m = phase/Δ, need Δ >> σ_lut.
//  - A_match (Δ_match, small): matching pipeline squares ciphertexts, so the
//    payload scales as Δ²·sqDist which must stay below q/2.
//    Δ_match=32 → Δ²=1024, representable sqDist < q/(2·1024) ≈ 4.87e5.
static const int64_t A_amp = q / (32 * 255); // 122333, rho = 2^-5 (glue/transport)
static const int64_t A_match = 32;           // matching pipeline (squared encoding)
static const double PI = 3.14159265358979323846;
static const double sigma_lut = 6.3e-7 * q; // B-3 recalibrated (glue/LUT noise model)
static const double sigma_ks = 1.0;
// Fresh-encryption noise for the matching pipeline. Kept small so that the
// ct×ct product noise (Σe_i² bias ≈ N·σ² and 2ΔΣd_ie_i cross term) stays far
// below Δ²=1024 per sqDist unit. Demo parameter — no security claimed.
static const double sigma_enc = 1.0;
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

struct RLWECiphertext {
    std::vector<int64_t> a;
    std::vector<int64_t> b;
    RLWECiphertext() : a(N, 0), b(N, 0) {}
    RLWECiphertext(const std::vector<int64_t>& _a, const std::vector<int64_t>& _b) : a(_a), b(_b) {}
};

struct LWECiphertext {
    std::vector<int64_t> a;
    int64_t b;
    LWECiphertext() : a(N, 0), b(0) {}
    LWECiphertext(const std::vector<int64_t>& _a, int64_t _b) : a(_a), b(_b) {}
};

inline LWECiphertext lwe_sample_extract(const RLWECiphertext& ct, int slot = 0) {
    return LWECiphertext(ct.a, ct.b[slot]);
}

inline RLWECiphertext rlwe_sub(const RLWECiphertext& ct1, const RLWECiphertext& ct2) {
    RLWECiphertext res;
    for (int i = 0; i < N; ++i) {
        res.a[i] = mod(ct1.a[i] - ct2.a[i]);
        res.b[i] = mod(ct1.b[i] - ct2.b[i]);
    }
    return res;
}

inline std::vector<int64_t> apply_automorphism(const std::vector<int64_t>& poly, int g) {
    std::vector<int64_t> res(N, 0);
    int twoN = 2 * N;
    for (int i = 0; i < N; ++i) {
        int idx = (static_cast<int64_t>(i) * g) % twoN;
        if (idx < 0) idx += twoN;
        if (idx < N) {
            res[idx] = poly[i];
        } else {
            res[idx - N] = mod(-poly[i]);
        }
    }
    return res;
}

inline RLWECiphertext keyswitch_poly(const std::vector<int64_t>& poly, const std::vector<KSKPair>& ksk, const NTTRoots& roots) {
    std::vector<std::vector<int64_t>> digs(ell, std::vector<int64_t>(N));
    std::vector<int64_t> Ac(N);
    for (int i = 0; i < N; ++i) Ac[i] = centered(poly[i]);
    for (int t = 0; t < ell; ++t) {
        for (int i = 0; i < N; ++i) {
            int64_t d = Ac[i] % Bg;
            if (d < 0) d += Bg;
            if (d > Bg / 2) d -= Bg;
            digs[t][i] = mod(d);
            Ac[i] = (Ac[i] - d) / Bg;
        }
    }
    RLWECiphertext res;
    for (int t = 0; t < ell; ++t) {
        std::vector<int64_t> da = negmul(digs[t], ksk[t].a, roots);
        std::vector<int64_t> db = negmul(digs[t], ksk[t].b, roots);
        for (int i = 0; i < N; ++i) {
            res.a[i] = (res.a[i] + da[i]) % q;
            res.b[i] = (res.b[i] + db[i]) % q;
        }
    }
    return res;
}

// Homomorphic automorphism X -> X^g: apply sigma_g to both components, then
// key-switch sigma_g(a) back to the base key s using a KSK that encrypts
// Bg^t * sigma_g(s) under s (b = -a*s + Bg^t*sigma_g(s) + e).
// Resulting phase: b' - a'*s = sigma_g(b - a*s) - e'.
inline RLWECiphertext rlwe_automorphism(const RLWECiphertext& ct, int g, const std::vector<KSKPair>& ksk, const NTTRoots& roots) {
    std::vector<int64_t> ra = apply_automorphism(ct.a, g);
    std::vector<int64_t> rb = apply_automorphism(ct.b, g);
    RLWECiphertext ks = keyswitch_poly(ra, ksk, roots);
    RLWECiphertext res;
    for (int i = 0; i < N; ++i) {
        res.a[i] = ks.a[i];
        res.b[i] = mod(rb[i] - ks.b[i]);
    }
    return res;
}

inline RLWECiphertext rlwe_mult_relin(const RLWECiphertext& ct1, const RLWECiphertext& ct2, const std::vector<KSKPair>& ksk_relin, const NTTRoots& roots) {
    // Tensor Product: (b1 - a1*s) * (b2 - a2*s) = b1*b2 - (a1*b2 + a2*b1)*s + a1*a2 * s^2
    std::vector<int64_t> b1b2 = negmul(ct1.b, ct2.b, roots);
    std::vector<int64_t> a1b2 = negmul(ct1.a, ct2.b, roots);
    std::vector<int64_t> a2b1 = negmul(ct2.a, ct1.b, roots);
    std::vector<int64_t> a1a2 = negmul(ct1.a, ct2.a, roots);

    std::vector<int64_t> a_cross(N);
    for (int i = 0; i < N; ++i) {
        a_cross[i] = mod(a1b2[i] + a2b1[i]);
    }

    // Relinearize a1a2 (multiplier for s^2) using ksk_relin (encrypts s^2 under s)
    RLWECiphertext ks_s2 = keyswitch_poly(a1a2, ksk_relin, roots);

    RLWECiphertext res;
    for (int i = 0; i < N; ++i) {
        res.a[i] = mod(a_cross[i] - ks_s2.a[i]);
        res.b[i] = mod(b1b2[i] + ks_s2.b[i]);
    }
    return res;
}

} // namespace FHECore

#endif // FHE_CORE_HPP

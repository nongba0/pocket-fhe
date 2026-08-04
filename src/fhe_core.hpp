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

// ---- TFHE Programmable Bootstrapping (PBS) stage parameters ----
// The sqDist stage produces an LWE ciphertext of dimension N=8192 under the
// sparse key Sq. Blind-rotating that directly would need 8192 CMux, so the
// ciphertext is first LWE-key-switched down to dimension n_pbs under a binary
// key, and the rotation happens in a smaller accumulator ring R_{N_acc}.
static const int N_acc   = 2048; // blind-rotation accumulator ring degree
static const int n_pbs   = 128;  // LWE dimension after key-switch (binary key)
static const int Bg_acc  = 256;  // GGSW gadget base (2^8)
static const int ell_acc = 4;    // 256^4 = 2^32 >= q
static const int Bg_ks   = 128;  // LWE key-switch gadget base (2^7)
static const int ell_ks  = 5;    // 128^5 = 2^35 >= q
static const int h_acc   = 64;   // sparse ternary weight of the accumulator key
static const int64_t Delta_out = q / 8; // encoding of the 1-bit verdict
// LUT resolution: the phase is mod-switched from q onto 2*N_acc points, so one
// LUT slot spans q/(2*N_acc*Delta_match^2) ≈ 238 sqDist units. Mod-switch
// rounding adds ~sqrt(n_pbs/2/12) ≈ 2.3 slots of jitter (measured: ~550 sqDist
// units, 1 sigma). The threshold is therefore only sharp to ~±1700 (3 sigma) —
// fine for decisions far from the boundary, and measured by the sweep test.

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
    j = (j % (2 * N) + 2 * N) % (2 * N);
    if (j == 0) return v;
    std::vector<int64_t> res(N, 0);
    if (j < N) {
        for (int i = j; i < N; ++i) res[i] = v[i - j];
        for (int i = 0; i < j; ++i) res[i] = mod(-v[N - j + i]);
    } else {
        int k_val = j - N;
        for (int i = k_val; i < N; ++i) res[i] = mod(-v[i - k_val]);
        for (int i = 0; i < k_val; ++i) res[i] = v[N - k_val + i];
    }
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

// Homomorphic automorphism X -> X^g
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
    std::vector<int64_t> b1b2 = negmul(ct1.b, ct2.b, roots);
    std::vector<int64_t> a1b2 = negmul(ct1.a, ct2.b, roots);
    std::vector<int64_t> a2b1 = negmul(ct2.a, ct1.b, roots);
    std::vector<int64_t> a1a2 = negmul(ct1.a, ct2.a, roots);

    std::vector<int64_t> a_cross(N);
    for (int i = 0; i < N; ++i) {
        a_cross[i] = mod(a1b2[i] + a2b1[i]);
    }

    RLWECiphertext ks_s2 = keyswitch_poly(a1a2, ksk_relin, roots);

    RLWECiphertext res;
    for (int i = 0; i < N; ++i) {
        res.a[i] = mod(a_cross[i] - ks_s2.a[i]);
        res.b[i] = mod(b1b2[i] + ks_s2.b[i]);
    }
    return res;
}

// ============================================================================
//  TFHE Programmable Bootstrapping (PBS)
//
//  CONVENTION for this section: every RLWE/LWE ciphertext satisfies
//        phase = b - <a, s>            (same as encrypt_vector / sample-extract)
//  The gadget KSKs used by the sqDist stage above use the OPPOSITE convention
//  (phase = b + a*s). Do not mix the two — that mismatch silently produced
//  garbage in an earlier revision.
//
//  SECURITY INVARIANT: the bootstrapping key is an array of GGSW ciphertexts
//  indexed by position only. It carries NO plaintext key material — no
//  positions-of-nonzeros, no signs. The server iterates over every index
//  0..n_pbs-1 and performs a CMux at each one; which branch is taken is
//  decided homomorphically by the GGSW, never by data the server can read.
//  An earlier revision shipped {index, sign} alongside each GGSW, which handed
//  the entire sparse secret key to the server.
// ============================================================================

// Signed (balanced) base-`base` decomposition of a centered residue.
inline void signed_decompose(int64_t x, int64_t base, int len, int64_t* out) {
    int64_t v = centered(x);
    for (int t = 0; t < len; ++t) {
        int64_t d = v % base;
        if (d < 0) d += base;
        if (d > base / 2) d -= base;
        out[t] = d;
        v = (v - d) / base;
    }
}

struct LWESmall {                 // dimension n_pbs, key s_pbs (binary)
    std::vector<int64_t> a;
    int64_t b;
    LWESmall() : a(n_pbs, 0), b(0) {}
};

struct RLWEAcc {                  // degree N_acc, key S_acc
    std::vector<int64_t> a, b;
    RLWEAcc() : a(N_acc, 0), b(N_acc, 0) {}
};

// GGSW(mu) under S_acc:
//   rowA[t] = RLWE(-Bg_acc^t * mu * S_acc(X))
//   rowB[t] = RLWE( Bg_acc^t * mu)
// so that <Decomp(a) , rowA> + <Decomp(b) , rowB> has phase mu*(b - a*S) .
struct GGSWAcc {
    std::vector<RLWEAcc> rowA, rowB;
    GGSWAcc() : rowA(ell_acc), rowB(ell_acc) {}
};

// Public evaluation material for bootstrapping. Contains ciphertexts only.
struct BootstrapKey {
    std::vector<GGSWAcc> bsk;                   // n_pbs entries: GGSW(s_pbs[i])
    std::vector<std::vector<LWESmall>> ksk;     // [N][ell_ks]: LWE(Bg_ks^t * S_ext[j])
    NTTRoots roots_acc;
};

// Key seen by an LWE sample-extracted at coefficient 0 of a degree-`deg` RLWE:
//   (a*S)[0] = a_0 S_0 - sum_{j>=1} a_j S_{deg-j}
inline std::vector<int64_t> extract_key(const std::vector<int64_t>& S, int deg) {
    std::vector<int64_t> E(deg);
    E[0] = S[0];
    for (int j = 1; j < deg; ++j) E[j] = -S[deg - j];
    return E;
}

// Multiply a polynomial by X^shift in R = Z_q[X]/(X^deg + 1). shift may be
// negative and is reduced mod 2*deg (X^deg = -1).
inline std::vector<int64_t> mono_mul(const std::vector<int64_t>& p, int shift, int deg) {
    int s = ((shift % (2 * deg)) + 2 * deg) % (2 * deg);
    std::vector<int64_t> r(deg, 0);
    for (int i = 0; i < deg; ++i) {
        int idx = i + s;
        bool neg = false;
        if (idx >= 2 * deg) idx -= 2 * deg;
        if (idx >= deg) { idx -= deg; neg = true; }
        r[idx] = neg ? mod(-p[i]) : mod(p[i]);
    }
    return r;
}

// LWE key-switch: LWE_{N, S_ext}(m) -> LWE_{n_pbs, s_pbs}(m).
//   a' = -sum_{j,t} d_{j,t} A_{j,t},   b' = b - sum_{j,t} d_{j,t} B_{j,t}
inline LWESmall lwe_keyswitch(const LWECiphertext& in,
                              const std::vector<std::vector<LWESmall>>& ksk) {
    LWESmall res;
    res.b = mod(in.b);
    std::vector<int64_t> dg(ell_ks);
    for (int j = 0; j < N; ++j) {
        signed_decompose(in.a[j], Bg_ks, ell_ks, dg.data());
        for (int t = 0; t < ell_ks; ++t) {
            if (dg[t] == 0) continue;
            int64_t d = mod(dg[t]);
            const LWESmall& K = ksk[j][t];
            for (int i = 0; i < n_pbs; ++i)
                res.a[i] = mod(res.a[i] - (__int128(d) * K.a[i]) % q);
            res.b = mod(res.b - (__int128(d) * K.b) % q);
        }
    }
    return res;
}

inline RLWEAcc external_product_acc(const RLWEAcc& ct, const GGSWAcc& g, const NTTRoots& roots) {
    std::vector<std::vector<int64_t>> da(ell_acc, std::vector<int64_t>(N_acc));
    std::vector<std::vector<int64_t>> db(ell_acc, std::vector<int64_t>(N_acc));
    std::vector<int64_t> tmp(ell_acc);
    for (int i = 0; i < N_acc; ++i) {
        signed_decompose(ct.a[i], Bg_acc, ell_acc, tmp.data());
        for (int t = 0; t < ell_acc; ++t) da[t][i] = mod(tmp[t]);
        signed_decompose(ct.b[i], Bg_acc, ell_acc, tmp.data());
        for (int t = 0; t < ell_acc; ++t) db[t][i] = mod(tmp[t]);
    }
    RLWEAcc res;
    for (int t = 0; t < ell_acc; ++t) {
        std::vector<int64_t> p1 = negmul(da[t], g.rowA[t].a, roots);
        std::vector<int64_t> p2 = negmul(da[t], g.rowA[t].b, roots);
        std::vector<int64_t> p3 = negmul(db[t], g.rowB[t].a, roots);
        std::vector<int64_t> p4 = negmul(db[t], g.rowB[t].b, roots);
        for (int i = 0; i < N_acc; ++i) {
            res.a[i] = mod(res.a[i] + p1[i] + p3[i]);
            res.b[i] = mod(res.b[i] + p2[i] + p4[i]);
        }
    }
    return res;
}

// CMux(GGSW(mu), c0, c1) = c0 + mu * (c1 - c0): selects c1 when mu = 1.
inline RLWEAcc cmux_acc(const GGSWAcc& g, const RLWEAcc& c0, const RLWEAcc& c1, const NTTRoots& roots) {
    RLWEAcc diff;
    for (int i = 0; i < N_acc; ++i) {
        diff.a[i] = mod(c1.a[i] - c0.a[i]);
        diff.b[i] = mod(c1.b[i] - c0.b[i]);
    }
    RLWEAcc prod = external_product_acc(diff, g, roots);
    RLWEAcc res;
    for (int i = 0; i < N_acc; ++i) {
        res.a[i] = mod(c0.a[i] + prod.a[i]);
        res.b[i] = mod(c0.b[i] + prod.b[i]);
    }
    return res;
}

// Blind rotation: ACC = V(X) * X^{-(b - sum a_i s_i)} = V(X) * X^{-phase}.
// The loop runs over EVERY index; the secret enters only through the GGSWs.
inline RLWEAcc blind_rotate(const LWESmall& lwe, const std::vector<int64_t>& testV,
                            const std::vector<GGSWAcc>& bsk, const NTTRoots& roots) {
    const int twoN = 2 * N_acc;
    auto mod_switch = [&](int64_t x) {
        long long v = std::llround(static_cast<double>(centered(x)) * twoN / static_cast<double>(q));
        return static_cast<int>(((v % twoN) + twoN) % twoN);
    };
    RLWEAcc acc;
    acc.b = mono_mul(testV, -mod_switch(lwe.b), N_acc);
    for (int i = 0; i < n_pbs; ++i) {
        int shift = mod_switch(lwe.a[i]);
        RLWEAcc shifted;
        shifted.a = mono_mul(acc.a, shift, N_acc);
        shifted.b = mono_mul(acc.b, shift, N_acc);
        acc = cmux_acc(bsk[i], acc, shifted, roots);
    }
    return acc;
}

// Step LUT: +Delta_out while sqDist <= threshold, -Delta_out above it.
// Valid phases occupy [0, q/2), which mod-switches into [0, N_acc), so the
// negacyclic half of the ring is never reached (the usual padding-bit trick).
inline std::vector<int64_t> make_step_test_poly(int64_t threshold_sqdist) {
    std::vector<int64_t> V(N_acc);
    double thr_phase = static_cast<double>(threshold_sqdist) * A_match * A_match;
    long long thr = std::llround(thr_phase * (2.0 * N_acc) / static_cast<double>(q));
    for (int j = 0; j < N_acc; ++j) V[j] = (j <= thr) ? mod(Delta_out) : mod(-Delta_out);
    return V;
}

} // namespace FHECore

#endif // FHE_CORE_HPP

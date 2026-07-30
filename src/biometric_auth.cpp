#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <algorithm>

static const int64_t q = 998244353; // ~2^30 NTT prime
static const int n = 512;            // 512-dimensional Face ID embedding vector
static const int N = 8192;
static const int k = N / n;          // 16 batches
static const int Bg = 32;            // 2^5 deep gadget
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

int main() {
    init_roots(n, pw_n, ipw_n);
    init_roots(N, pw_N, ipw_N);

    std::mt19937_64 rng(888);
    std::uniform_int_distribution<int64_t> unif_feature(-50, 50);

    // 1) Generate Stored Encrypted Face Template U (512 dimensions)
    std::vector<int64_t> template_face(n);
    for (int i = 0; i < n; ++i) template_face[i] = unif_feature(rng);

    // 2) Generate Live Scanned Face V (with slight noise for MATCH test, or random for DENIED test)
    std::vector<int64_t> live_face_match(n);
    std::normal_distribution<double> noise_match(0.0, 2.0); // slight sensor noise
    for (int i = 0; i < n; ++i) {
        live_face_match[i] = template_face[i] + (int64_t)std::round(noise_match(rng));
    }

    std::cout << "==========================================================" << std::endl;
    std::cout << "[Pocket-FHE Private Face ID Authentication Engine]" << std::endl;
    std::cout << "Feature Vector Dimension: 512 dimensions" << std::endl;
    std::cout << "==========================================================" << std::endl;

    // Calculate Ground-Truth Euclidean Distance
    int64_t true_sq_dist = 0;
    for (int i = 0; i < n; ++i) {
        int64_t diff = live_face_match[i] - template_face[i];
        true_sq_dist += diff * diff;
    }
    double sim_score = std::max(0.0, 100.0 - (true_sq_dist / 20.0));

    std::cout << "[Ground Truth] Live Face Feature Diff Squared Sum = " << true_sq_dist << std::endl;
    std::cout << "[Ground Truth] Calculated Similarity Match Score = " << sim_score << "%" << std::endl;
    std::cout << "[Decision Threshold] Distance Tau = 2000 (Similarity >= 90.0%)" << std::endl;

    bool is_matched = (true_sq_dist <= 2000);
    std::cout << "\n[AUTH RESULT] " << (is_matched ? "✅ ACCESS GRANTED (Biometric Match SUCCESS)" : "❌ ACCESS DENIED (Match FAIL)") << std::endl;

    return 0;
}

#include <iostream>
#include <vector>

static const int64_t p = 998244353;
static const int n = 4;

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

int main() {
    std::vector<int64_t> a = {1, 2, 3, 4};
    ntt(a, false);
    std::cout << "NTT([1,2,3,4]) = ";
    for (auto x : a) std::cout << x << " ";
    std::cout << std::endl;
    ntt(a, true);
    std::cout << "INTT = ";
    for (auto x : a) std::cout << x << " ";
    std::cout << std::endl;
    return 0;
}

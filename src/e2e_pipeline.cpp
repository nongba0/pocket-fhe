#include "fhe_core.hpp"

using namespace FHECore;

int main() {
    std::cout << "=== Pocket-FHE E2E Noise Model Pipeline (fhe_core.hpp Unified) ===" << std::endl;
    std::cout << "Parameters: N=" << N << ", n=" << n << ", k=" << k 
              << ", Bg=" << Bg << ", ell=" << ell << ", q=" << q << std::endl;

    NTTRoots roots_n, roots_N;
    init_roots(n, roots_n);
    init_roots(N, roots_N);

    std::mt19937 gen(42);
    std::uniform_int_distribution<int64_t> dist_q(0, q - 1);
    std::uniform_int_distribution<int> dist_bin(0, 1);
    std::normal_distribution<double> dist_lut(0.0, sigma_lut);
    std::normal_distribution<double> dist_ks(0.0, sigma_ks);
    std::normal_distribution<double> dist_eval(0.0, sigma_eval);

    std::vector<int64_t> stfhe(n);
    for (int i = 0; i < n; ++i) stfhe[i] = dist_bin(gen);
    std::vector<int64_t> semb = embed(stfhe);

    std::vector<int64_t> Sq(N, 0);
    std::vector<int> idx_pool(N);
    for (int i = 0; i < N; ++i) idx_pool[i] = i;
    for (int i = 0; i < hS; ++i) {
        std::uniform_int_distribution<size_t> d(0, idx_pool.size() - 1);
        size_t pick = d(gen);
        int pos = idx_pool[pick];
        idx_pool.erase(idx_pool.begin() + pick);
        Sq[pos] = (i % 2 == 0) ? 1 : -1;
    }

    std::vector<KSKPair> KSK(ell);
    for (int t = 0; t < ell; ++t) {
        KSK[t].a.resize(N);
        KSK[t].b.resize(N);
        for (int i = 0; i < N; ++i) KSK[t].a[i] = dist_q(gen);
        std::vector<int64_t> aS = negmul(KSK[t].a, Sq, roots_N);
        int64_t Bgt = mod_pow(Bg, t);
        for (int i = 0; i < N; ++i) {
            int64_t e = std::round(dist_ks(gen));
            KSK[t].b[i] = mod(-aS[i] + (__int128(Bgt) * semb[i]) % q + e);
        }
    }

    std::vector<std::vector<int64_t>> ctsA(k, std::vector<int64_t>(n));
    std::vector<std::vector<int64_t>> ctsB(k, std::vector<int64_t>(n));
    std::vector<int64_t> Mexp(N, 0);

    for (int j = 0; j < k; ++j) {
        std::vector<int64_t> m(n);
        for (int i = 0; i < n; ++i) {
            m[i] = (i * 3 + j * 7) % 256 - 128;
            ctsA[j][i] = dist_q(gen);
        }
        std::vector<int64_t> as = negmul(ctsA[j], stfhe, roots_n);
        for (int i = 0; i < n; ++i) {
            int64_t e = std::round(dist_lut(gen));
            ctsB[j][i] = mod(as[i] + (__int128(A_amp) * mod(m[i])) % q + e);
        }

        std::vector<int64_t> Mj(N, 0);
        for (int i = 0; i < n; ++i) Mj[i * k] = m[i];
        std::vector<int64_t> r = mshift(Mj, j);
        for (int i = 0; i < N; ++i) Mexp[i] += r[i];
    }

    // Pure Timed Glue Execution
    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<int64_t> Act(N, 0), Bct(N, 0);
    for (int j = 0; j < k; ++j) {
        std::vector<int64_t> ma = mshift(embed(ctsA[j]), j);
        std::vector<int64_t> mb = mshift(embed(ctsB[j]), j);
        for (int i = 0; i < N; ++i) {
            Act[i] = (Act[i] + ma[i]) % q;
            Bct[i] = (Bct[i] + mb[i]) % q;
        }
    }

    std::vector<std::vector<int64_t>> digs(ell, std::vector<int64_t>(N));
    std::vector<int64_t> Ac(N);
    for (int i = 0; i < N; ++i) Ac[i] = centered(Act[i]);
    for (int t = 0; t < ell; ++t) {
        for (int i = 0; i < N; ++i) {
            int64_t d = Ac[i] % Bg;
            if (d < 0) d += Bg;
            if (d > Bg / 2) d -= Bg;
            digs[t][i] = mod(d);
            Ac[i] = (Ac[i] - d) / Bg;
        }
    }

    std::vector<int64_t> A2(N, 0), B2 = Bct;
    for (int t = 0; t < ell; ++t) {
        std::vector<int64_t> da = negmul(digs[t], KSK[t].a, roots_N);
        std::vector<int64_t> db = negmul(digs[t], KSK[t].b, roots_N);
        for (int i = 0; i < N; ++i) {
            A2[i] = (A2[i] + da[i]) % q;
            B2[i] = (B2[i] - db[i] + q) % q;
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double glue_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Verification
    std::vector<int64_t> A2S = negmul(A2, Sq, roots_N);
    int exact = 0;
    for (int i = 0; i < N; ++i) {
        int64_t ph = centered(B2[i] - A2S[i]);
        double y = (q / (2.0 * PI)) * std::sin(2.0 * PI * ph / (double)q) + dist_eval(gen);
        int64_t rec = std::round(y / (double)A_amp);
        if (rec == Mexp[i]) exact++;
    }

    std::cout << "[E2E Pipeline Result] Exact Recovery: " << exact << " / " << N << " slots" << std::endl;
    std::cout << "[Native Glue Measured] Time: " << glue_ms << " ms (" 
              << (glue_ms * 1000.0 / N) << " us/value)" << std::endl;

    if (exact == N) {
        std::cout << "✅ E2E Pipeline Verification PASS (100% Slot Recovery)" << std::endl;
    } else {
        std::cerr << "❌ E2E Pipeline Verification FAIL" << std::endl;
        return 1;
    }

    return 0;
}

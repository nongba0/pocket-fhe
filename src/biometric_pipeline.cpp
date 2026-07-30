#include "fhe_core.hpp"

using namespace FHECore;

int main() {
    std::cout << "=== Pocket-FHE Standalone CPU Biometric Pipeline (fhe_core.hpp Unified) ===" << std::endl;

    NTTRoots roots_n, roots_N;
    init_roots(n, roots_n);
    init_roots(N, roots_N);

    std::mt19937 gen(888);
    std::uniform_int_distribution<int64_t> dist_q(0, q - 1);
    std::uniform_int_distribution<int> dist_bin(0, 1);
    std::normal_distribution<double> dist_ks(0.0, sigma_ks);

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

    // --- TEST 1: Alice Same-Person Match (Access Granted Expected) ---
    std::vector<int64_t> template_face(n);
    std::vector<int64_t> alice_live_face(n);
    std::normal_distribution<double> dist_noise(0.0, 1.2);

    int64_t sq_dist_alice_plain = 0;
    for (int i = 0; i < n; ++i) {
        template_face[i] = (i * 7) % 81 - 40;
        alice_live_face[i] = template_face[i] + std::round(dist_noise(gen));
        int64_t diff = alice_live_face[i] - template_face[i];
        sq_dist_alice_plain += diff * diff;
    }

    // Encrypt Alice Difference Vector
    std::vector<std::vector<int64_t>> ctsA(k, std::vector<int64_t>(n));
    std::vector<std::vector<int64_t>> ctsB(k, std::vector<int64_t>(n));
    std::vector<int64_t> Mexp_alice(N, 0);

    for (int j = 0; j < k; ++j) {
        std::vector<int64_t> m(n);
        for (int i = 0; i < n; ++i) {
            m[i] = (j == 0) ? (alice_live_face[i] - template_face[i]) : ((i + j) % 256 - 128);
            ctsA[j][i] = dist_q(gen);
        }
        std::vector<int64_t> as = negmul(ctsA[j], stfhe, roots_n);
        for (int i = 0; i < n; ++i) {
            ctsB[j][i] = mod(as[i] + (__int128(A_amp) * mod(m[i])) % q);
        }

        std::vector<int64_t> Mj(N, 0);
        for (int i = 0; i < n; ++i) Mj[i * k] = m[i];
        std::vector<int64_t> r = mshift(Mj, j);
        for (int i = 0; i < N; ++i) Mexp_alice[i] += r[i];
    }

    // Timed Glue for Alice
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

    std::vector<int64_t> A2S = negmul(A2, Sq, roots_N);
    int exact_alice = 0;
    int64_t sq_dist_alice_encrypted = 0;

    for (int i = 0; i < N; ++i) {
        int64_t ph = centered(B2[i] - A2S[i]);
        double y = (q / (2.0 * PI)) * std::sin(2.0 * PI * ph / (double)q);
        int64_t rec = std::round(y / (double)A_amp);
        if (rec == Mexp_alice[i]) exact_alice++;
        if (i % k == 0) sq_dist_alice_encrypted += rec * rec;
    }

    bool aliceMatch = sq_dist_alice_encrypted <= 40000;
    std::cout << "[Test 1: Alice Same-Person Match] Encrypted Dist: " << sq_dist_alice_encrypted 
              << " | Verdict: " << (aliceMatch ? "✅ ACCESS GRANTED" : "❌ DENIED") << std::endl;

    // --- TEST 2: Bob Different-Person Mismatch (Access Denied Expected) ---
    std::vector<int64_t> bob_live_face(n);
    int64_t sq_dist_bob_plain = 0;
    for (int i = 0; i < n; ++i) {
        bob_live_face[i] = (i * 19 + 11) % 81 - 40; // Completely different face
        int64_t diff = bob_live_face[i] - template_face[i];
        sq_dist_bob_plain += diff * diff;
    }

    std::vector<std::vector<int64_t>> ctsA_bob(k, std::vector<int64_t>(n));
    std::vector<std::vector<int64_t>> ctsB_bob(k, std::vector<int64_t>(n));
    std::vector<int64_t> Mexp_bob(N, 0);

    for (int j = 0; j < k; ++j) {
        std::vector<int64_t> m(n);
        for (int i = 0; i < n; ++i) {
            m[i] = (j == 0) ? (bob_live_face[i] - template_face[i]) : ((i + j) % 256 - 128);
            ctsA_bob[j][i] = dist_q(gen);
        }
        std::vector<int64_t> as = negmul(ctsA_bob[j], stfhe, roots_n);
        for (int i = 0; i < n; ++i) {
            ctsB_bob[j][i] = mod(as[i] + (__int128(A_amp) * mod(m[i])) % q);
        }

        std::vector<int64_t> Mj(N, 0);
        for (int i = 0; i < n; ++i) Mj[i * k] = m[i];
        std::vector<int64_t> r = mshift(Mj, j);
        for (int i = 0; i < N; ++i) Mexp_bob[i] += r[i];
    }

    std::vector<int64_t> Act_bob(N, 0), Bct_bob(N, 0);
    for (int j = 0; j < k; ++j) {
        std::vector<int64_t> ma = mshift(embed(ctsA_bob[j]), j);
        std::vector<int64_t> mb = mshift(embed(ctsB_bob[j]), j);
        for (int i = 0; i < N; ++i) {
            Act_bob[i] = (Act_bob[i] + ma[i]) % q;
            Bct_bob[i] = (Bct_bob[i] + mb[i]) % q;
        }
    }

    std::vector<std::vector<int64_t>> digs_bob(ell, std::vector<int64_t>(N));
    std::vector<int64_t> Ac_bob(N);
    for (int i = 0; i < N; ++i) Ac_bob[i] = centered(Act_bob[i]);
    for (int t = 0; t < ell; ++t) {
        for (int i = 0; i < N; ++i) {
            int64_t d = Ac_bob[i] % Bg;
            if (d < 0) d += Bg;
            if (d > Bg / 2) d -= Bg;
            digs_bob[t][i] = mod(d);
            Ac_bob[i] = (Ac_bob[i] - d) / Bg;
        }
    }

    std::vector<int64_t> A2_bob(N, 0), B2_bob = Bct_bob;
    for (int t = 0; t < ell; ++t) {
        std::vector<int64_t> da = negmul(digs_bob[t], KSK[t].a, roots_N);
        std::vector<int64_t> db = negmul(digs_bob[t], KSK[t].b, roots_N);
        for (int i = 0; i < N; ++i) {
            A2_bob[i] = (A2_bob[i] + da[i]) % q;
            B2_bob[i] = (B2_bob[i] - db[i] + q) % q;
        }
    }

    std::vector<int64_t> A2S_bob = negmul(A2_bob, Sq, roots_N);
    int exact_bob = 0;
    int64_t sq_dist_bob_encrypted = 0;

    for (int i = 0; i < N; ++i) {
        int64_t ph = centered(B2_bob[i] - A2S_bob[i]);
        double y = (q / (2.0 * PI)) * std::sin(2.0 * PI * ph / (double)q);
        int64_t rec = std::round(y / (double)A_amp);
        if (rec == Mexp_bob[i]) exact_bob++;
        if (i % k == 0) sq_dist_bob_encrypted += rec * rec;
    }

    bool bobMatch = sq_dist_bob_encrypted <= 40000;
    std::cout << "[Test 2: Bob Mismatch Denial] Encrypted Dist: " << sq_dist_bob_encrypted 
              << " | Verdict: " << (bobMatch ? "❌ GRANTED (ERROR)" : "✅ ACCESS DENIED (CORRECT)") << std::endl;

    // Dual-Path Assertion
    if (exact_alice == N && exact_bob == N && aliceMatch && !bobMatch) {
        std::cout << "✅ Biometric Pipeline Dual-Path PASS (Alice Match GRANTED & Bob Mismatch DENIED Verified)" << std::endl;
    } else {
        std::cerr << "❌ Biometric Pipeline Dual-Path Verification FAIL!" << std::endl;
        return 1;
    }

    return 0;
}

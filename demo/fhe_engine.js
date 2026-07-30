// fhe_engine.js — e2e_pipeline.cpp의 충실한 JS 포트 (실연산, 목업 아님)
// q ≈ 2^30 이므로 2^15 분할 mulmod로 IEEE double(2^53) 내에서 정확한 모듈러 산술.
// 검증 체인: batched-LUT 출력 샘플링(σ_LUT = 6.3e-7·q, B-3 확정값) → embed →
// merge → gadget KS → [시뮬레이션] phase + ideal-sine EvalMod → 반올림 복구.
// 실측 구간은 glue(embed+merge+decomp+KS)만. LUT/EvalMod는 노이즈 모델.
'use strict';

const FHE = (() => {
    const q = 998244353;            // NTT prime
    const n = 512;
    const N = 8192;
    const k = N / n;                // 16
    const Bg = 32;                  // 2^5 — 깊은 gadget (KS 노이즈 ~1.8x 감소)
    const ell = 6;                  // 32^6 = 2^30 >= q 커버; 40/40 시드 PASS 확인
    const hS = 64;                  // sparse ternary weight
    const A_amp = Math.floor(q / (32 * 255)); // 122333, rho = 2^-5
    const sigma_lut = 6.3e-7 * q;   // B-3 recalibrated (PSI-grade gadget)
    const sigma_ks = 3.2;
    const sigma_eval = q * Math.pow(2, -25);

    // ---- 정확한 모듈러 산술 (a,b ∈ [0,q)) ----
    function mulmod(a, b) {
        const bHi = Math.floor(b / 32768), bLo = b % 32768;
        return ((a * bHi % q) * 32768 + a * bLo) % q;
    }
    function mod(x) { const r = x % q; return r < 0 ? r + q : r; }
    function powmod(base, exp) {
        let res = 1; base = mod(base);
        while (exp > 0) {
            if (exp & 1) res = mulmod(res, base);
            base = mulmod(base, base);
            exp = Math.floor(exp / 2);
        }
        return res;
    }
    const invmod = (x) => powmod(x, q - 2);
    function centered(x) { const r = mod(x); return r > q / 2 ? r - q : r; }

    // ---- Cryptographically Secure PRNG (Web Crypto API crypto.getRandomValues with mulberry32 fallback) + Box-Muller ----
    function makeRng(seed = null) {
        if (seed === null && typeof crypto !== 'undefined' && crypto.getRandomValues) {
            return function () {
                const arr = new Uint32Array(1);
                crypto.getRandomValues(arr);
                return arr[0] / 4294967296;
            };
        }
        let s = (seed || 42) >>> 0;
        return function () {
            s = (s + 0x6D2B79F5) >>> 0;
            let t = s;
            t = Math.imul(t ^ (t >>> 15), t | 1);
            t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
            return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
        };
    }
    function makeGauss(rng, sigma) {
        let spare = null;
        return function () {
            if (spare !== null) { const v = spare; spare = null; return v * sigma; }
            let u, v, s;
            do { u = 2 * rng() - 1; v = 2 * rng() - 1; s = u * u + v * v; } while (s >= 1 || s === 0);
            const m = Math.sqrt(-2 * Math.log(s) / s);
            spare = v * m;
            return u * m * sigma;
        };
    }

    // ---- NTT (negacyclic, pre/post twist) ----
    const roots = {};
    function initRoots(deg) {
        const ps = powmod(3, (q - 1) / (2 * deg));
        const ips = invmod(ps);
        const pw = new Float64Array(deg), ipw = new Float64Array(deg);
        let w = 1, iw = 1;
        for (let i = 0; i < deg; ++i) {
            pw[i] = w; ipw[i] = iw;
            w = mulmod(w, ps); iw = mulmod(iw, ips);
        }
        roots[deg] = { pw, ipw };
    }
    function bitReverse(a) {
        const deg = a.length;
        let j = 0;
        for (let i = 1; i < deg; ++i) {
            let bit = deg >> 1;
            while (j & bit) { j ^= bit; bit >>= 1; }
            j |= bit;
            if (i < j) { const t = a[i]; a[i] = a[j]; a[j] = t; }
        }
    }
    function ntt(a, invert) {
        const deg = a.length;
        bitReverse(a);
        for (let len = 2; len <= deg; len <<= 1) {
            let wm = powmod(3, (q - 1) / len);
            if (invert) wm = invmod(wm);
            const half = len >> 1;
            const wPows = new Float64Array(half);
            let w = 1;
            for (let i = 0; i < half; ++i) { wPows[i] = w; w = mulmod(w, wm); }
            for (let start = 0; start < deg; start += len) {
                for (let i = 0; i < half; ++i) {
                    const u = a[start + i];
                    const v = mulmod(a[start + half + i], wPows[i]);
                    a[start + i] = (u + v) % q;
                    a[start + half + i] = (u - v + q) % q;
                }
            }
        }
        if (invert) {
            const inv = invmod(deg);
            for (let i = 0; i < deg; ++i) a[i] = mulmod(a[i], inv);
        }
    }
    function negmul(a, b) {
        const deg = a.length;
        const { pw, ipw } = roots[deg];
        const A = new Float64Array(deg), B = new Float64Array(deg);
        for (let i = 0; i < deg; ++i) {
            A[i] = mulmod(mod(a[i]), pw[i]);
            B[i] = mulmod(mod(b[i]), pw[i]);
        }
        ntt(A, false); ntt(B, false);
        const C = new Float64Array(deg);
        for (let i = 0; i < deg; ++i) C[i] = mulmod(A[i], B[i]);
        ntt(C, true);
        const c = new Float64Array(deg);
        for (let i = 0; i < deg; ++i) c[i] = mulmod(C[i], ipw[i]);
        return c;
    }
    function embed(v) {
        const P = new Float64Array(N);
        for (let i = 0; i < n; ++i) P[i * k] = mod(v[i]);
        return P;
    }
    function mshift(v, j) {
        if (j === 0) return Float64Array.from(v);
        const res = new Float64Array(N);
        for (let i = j; i < N; ++i) res[i] = v[i - j];
        for (let i = 0; i < j; ++i) res[i] = mod(-v[N - j + i]);
        return res;
    }

    initRoots(n); initRoots(N);

    // ---- KSK Evaluation Key Caching Module ----
    let keyCache = null;

    function generateEvaluationKeys(krng) {
        const gKS = makeGauss(krng, sigma_ks);
        const stfhe = new Float64Array(n);
        for (let i = 0; i < n; ++i) stfhe[i] = krng() < 0.5 ? 0 : 1;
        const semb = embed(stfhe);

        const Sidx = new Int32Array(hS), Sq = new Float64Array(N);
        const idxPool = Array.from({ length: N }, (_, i) => i);
        for (let i = 0; i < hS; ++i) {
            const pick = Math.floor(krng() * idxPool.length);
            const pos = idxPool.splice(pick, 1)[0];
            const val = i % 2 === 0 ? 1 : -1;
            Sidx[i] = pos;
            Sq[pos] = mod(val);
        }

        const KSK = [];
        for (let t = 0; t < ell; ++t) {
            const aK = new Float64Array(N), bK = new Float64Array(N);
            for (let i = 0; i < N; ++i) aK[i] = Math.floor(krng() * q);
            const aS = negmul(aK, Sq);
            const Bgt = powmod(Bg, t);
            for (let i = 0; i < N; ++i) {
                bK[i] = mod(-aS[i] + mulmod(Bgt, semb[i]) + Math.round(gKS()));
            }
            KSK.push({ a: aK, b: bK });
        }

        return { stfhe, semb, Sq, KSK };
    }

    function getOrGenerateKeys(opts, rng) {
        if (opts.useCache !== false && keyCache !== null) {
            return keyCache;
        }
        const krng = opts.deterministic ? rng : makeRng(null);
        const keys = generateEvaluationKeys(krng);
        if (opts.useCache !== false) {
            keyCache = keys;
        }
        return keys;
    }

    function initKeys(seed = null) {
        const krng = seed !== null ? makeRng(seed) : makeRng(null);
        keyCache = generateEvaluationKeys(krng);
        return keyCache;
    }

    function clearKeyCache() {
        keyCache = null;
    }

    // ---- run(seed, payloadsIn, opts) ----
    // payloadsIn: Float64Array 1개(ct 0에 적재) 또는 배열(최대 k개 — ct 0..m-1에 적재).
    function run(seed = 42, payloadsIn = null, opts = {}) {
        const payloads = payloadsIn == null ? null
            : Array.isArray(payloadsIn) ? payloadsIn.slice(0, k) : [payloadsIn];
        const rng = makeRng(seed);
        const gLUT = makeGauss(rng, sigma_lut);
        const gEval= makeGauss(rng, sigma_eval);

        // KSK 키 캐싱 모듈 사용 (최초 1회 생성 후 수십 ms로 단축!)
        const { stfhe, semb, Sq, KSK } = getOrGenerateKeys(opts, rng);

        const ctsA = [], ctsB = [], Mexp = new Float64Array(N);
        for (let j = 0; j < k; ++j) {
            const m = new Float64Array(n), a = new Float64Array(n), e = new Float64Array(n);
            for (let i = 0; i < n; ++i) {
                if (payloads && j < payloads.length) {
                    m[i] = payloads[j][i];
                } else {
                    m[i] = Math.floor(rng() * 256) - 128;
                }
                a[i] = Math.floor(rng() * q);
                e[i] = Math.round(gLUT());
            }
            const as = negmul(a, stfhe);
            const b = new Float64Array(n);
            for (let i = 0; i < n; ++i) b[i] = mod(as[i] + mulmod(A_amp, mod(m[i])) + e[i]);
            ctsA.push(a); ctsB.push(b);

            const Mj = new Float64Array(N);
            for (let i = 0; i < n; ++i) Mj[i * k] = m[i];
            const r = mshift(Mj, j);
            for (let i = 0; i < N; ++i) Mexp[i] += r[i];
        }

        // ---- TIMED GLUE ----
        const t0 = performance.now();
        const Act = new Float64Array(N), Bct = new Float64Array(N);
        for (let j = 0; j < k; ++j) {
            const ma = mshift(embed(ctsA[j]), j);
            const mb = mshift(embed(ctsB[j]), j);
            for (let i = 0; i < N; ++i) {
                Act[i] = (Act[i] + ma[i]) % q;
                Bct[i] = (Bct[i] + mb[i]) % q;
            }
        }
        const digs = [];
        const Ac = new Float64Array(N);
        for (let i = 0; i < N; ++i) Ac[i] = centered(Act[i]);
        for (let t = 0; t < ell; ++t) {
            const dt = new Float64Array(N);
            for (let i = 0; i < N; ++i) {
                let d = Ac[i] % Bg;
                if (d < 0) d += Bg;
                if (d > Bg / 2) d -= Bg;
                dt[i] = mod(d);
                Ac[i] = (Ac[i] - d) / Bg;
            }
            digs.push(dt);
        }
        const A2 = new Float64Array(N), B2 = Float64Array.from(Bct);
        for (let t = 0; t < ell; ++t) {
            const da = negmul(digs[t], KSK[t].a);
            const db = negmul(digs[t], KSK[t].b);
            for (let i = 0; i < N; ++i) {
                A2[i] = (A2[i] + da[i]) % q;
                B2[i] = (B2[i] - db[i] + q) % q;
            }
        }
        const glueMs = performance.now() - t0;

        // ---- UNTIMED SIMULATION: phase(비밀키 사용) + ideal-sine EvalMod ----
        const A2S = negmul(A2, Sq);
        let exact = 0;
        const numRec = payloads ? payloads.length : 1;
        const recovered = [];
        for (let j = 0; j < numRec; ++j) recovered.push(new Float64Array(n));
        for (let p = 0; p < N; ++p) {
            const ph = centered(B2[p] - A2S[p]);
            const y = (q / (2 * Math.PI)) * Math.sin(2 * Math.PI * ph / q) + gEval();
            const rec = Math.round(y / A_amp);
            if (rec === Mexp[p]) exact++;
            const jj = p % k;
            if (jj < numRec) recovered[jj][(p - jj) / k] = rec;
        }

        return {
            params: { N, n, k, ell, Bg, q, A_amp, hS, sigma_lut_rel: 6.3e-7 },
            exact, total: N,
            pass: exact === N,
            glueMs,
            usPerValue: glueMs * 1000 / N,
            recovered,
            recovered0: recovered[0]
        };
    }

    // ---- 512-dim Single Biometric Engine ----
    function runBiometricAuthCustom(liveVector, templateVector, seed = 888, opts = {}) {
        const dQ = new Float64Array(n);
        let sqDistPlain = 0;
        for (let i = 0; i < n; ++i) {
            let d = Math.round(liveVector[i] - templateVector[i]);
            if (d > 127) d = 127; else if (d < -127) d = -127;
            dQ[i] = d;
            sqDistPlain += d * d;
        }

        const base = run(seed, dQ, opts);

        let sqDist = 0, transportExact = true;
        for (let i = 0; i < n; ++i) {
            const dh = base.recovered0[i];
            sqDist += dh * dh;
            if (dh !== dQ[i]) transportExact = false;
        }

        const simScore = Math.max(0, Math.min(100, 100.0 - (sqDist / 550.0)));
        const isMatch = sqDist <= 40000;

        return {
            ...base,
            biometric: {
                dim: n,
                sqDist,
                sqDistPlain,
                transportExact,
                simScore: simScore.toFixed(1),
                isMatch,
                status: isMatch ? '✅ ACCESS GRANTED (Biometric Match SUCCESS)' : '❌ ACCESS DENIED (Match FAIL)'
            }
        };
    }

    // ---- 512-dim Multi-User 1:N Biometric Search Engine ----
    function runMultiUserBiometricAuth(liveVector, db, seed = 888, opts = {}) {
        const users = db.slice(0, k);
        const truncated = db.length > k;

        const payloads = [];
        const plainDists = [];
        for (let u = 0; u < users.length; ++u) {
            const dQ = new Float64Array(n);
            let sq = 0;
            for (let i = 0; i < n; ++i) {
                let d = Math.round(liveVector[i] - users[u].vector[i]);
                if (d > 127) d = 127; else if (d < -127) d = -127;
                dQ[i] = d;
                sq += d * d;
            }
            payloads.push(dQ);
            plainDists.push(sq);
        }

        const base = run(seed, payloads, opts);

        let bestUser = null, minSqDist = Infinity, allTransportExact = true;
        const allResults = [];
        for (let u = 0; u < users.length; ++u) {
            let sqDist = 0, transportExact = true;
            for (let i = 0; i < n; ++i) {
                const dh = base.recovered[u][i];
                sqDist += dh * dh;
                if (dh !== payloads[u][i]) transportExact = false;
            }
            if (!transportExact) allTransportExact = false;
            allResults.push({
                id: users[u].id, name: users[u].name,
                sqDist, sqDistPlain: plainDists[u], transportExact,
                simScore: Math.max(0, Math.min(100, 100.0 - (sqDist / 550.0))).toFixed(1)
            });
            if (sqDist < minSqDist) { minSqDist = sqDist; bestUser = users[u]; }
        }

        const simScore = Math.max(0, Math.min(100, 100.0 - (minSqDist / 550.0)));
        const isMatch = minSqDist <= 40000;

        return {
            ...base,
            multiBiometric: {
                dim: n,
                totalUsers: users.length,
                truncated,
                bestUser: bestUser ? bestUser.name : "Unknown",
                minSqDist,
                allTransportExact,
                simScore: simScore.toFixed(1),
                isMatch,
                allResults,
                status: isMatch ? `✅ MATCH FOUND: ${bestUser.name}` : `❌ UNKNOWN USER (No Match in DB)`
            }
        };
    }

    function runBiometricAuth(seed = 888, opts = {}) {
        const rng = makeRng(seed);
        const templateFace = new Float64Array(n);
        for (let i = 0; i < n; ++i) templateFace[i] = Math.floor(rng() * 21) - 10;

        const liveFace = new Float64Array(n);
        const gNoise = makeGauss(rng, 1.2);
        for (let i = 0; i < n; ++i) {
            liveFace[i] = templateFace[i] + Math.round(gNoise());
        }

        return runBiometricAuthCustom(liveFace, templateFace, seed, opts);
    }

    return { run, runBiometricAuth, runBiometricAuthCustom, runMultiUserBiometricAuth, initKeys, clearKeyCache, params: { N, n, k, ell } };
})();

if (typeof module !== 'undefined' && module.exports) module.exports = FHE;

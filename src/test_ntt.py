import numpy as np

p = 998244353

def bitrev(a):
    deg = len(a)
    j = 0
    a = a.copy()
    for i in range(1, deg):
        bit = deg >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    return a

def ntt(a, invert=False):
    deg = len(a)
    a = bitrev(a % p)
    length = 2
    while length <= deg:
        w_m = pow(3, (p - 1) // length, p)
        if invert:
            w_m = pow(w_m, -1, p)
        half = length // 2
        w_pows = np.empty(half, dtype=np.int64)
        w = 1
        for i in range(half):
            w_pows[i] = w
            w = (w * w_m) % p
        for start in range(0, deg, length):
            lo = a[start:start + half].astype(np.int64)
            hi = (a[start + half:start + length].astype(np.int64) * w_pows) % p
            a[start:start + half] = (lo + hi) % p
            a[start + half:start + length] = (lo - hi) % p
        length <<= 1
    if invert:
        inv = pow(int(deg), -1, p)
        a = (a * inv) % p
    return a

a = np.array([1, 2, 3, 4], dtype=np.int64)
A = ntt(a)
print("Python NTT([1,2,3,4]) =", A)
print("Python INTT =", ntt(A, invert=True))

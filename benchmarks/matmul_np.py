import time
import numpy as np

# Same shapes as benchmarks/matmul.fl: 10 timed iterations, avg ms.
for N in (100, 200, 500):
    a = (np.arange(N * N, dtype=np.float64).reshape(N, N) % 100) / 10.0
    b = (np.arange(N * N, dtype=np.float64).reshape(N, N) % 50) / 7.0
    for _ in range(3):  # warmup
        c = a @ b
    t0 = time.monotonic()
    for _ in range(10):
        c = a @ b
    t1 = time.monotonic()
    print(f"size {N}")
    print(f"avg ms = {(t1 - t0) / 10 * 1000:.4f}")
print("checksum:", float(c.sum()))

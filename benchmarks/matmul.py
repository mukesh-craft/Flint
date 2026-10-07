import time
for N in (100, 200, 500):
    a = [[(i * N + j) % 100 / 10.0 for j in range(N)] for i in range(N)]
    b = [[(i * N + j) % 50 / 7.0 for j in range(N)] for i in range(N)]
    # warmup
    for _ in range(2):
        c = [[sum(a[i][k] * b[k][j] for k in range(N)) for j in range(N)] for i in range(N)]
    t0 = time.monotonic()
    iters = 1 if N >= 200 else 3
    for _ in range(iters):
        c = [[sum(a[i][k] * b[k][j] for k in range(N)) for j in range(N)] for i in range(N)]
    t1 = time.monotonic()
    print(f"size {N}")
    print(f"avg ms = {(t1 - t0) / iters * 1000:.4f}")

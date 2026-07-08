import time

n = 100000000
t0 = time.monotonic_ns()
pi = 0.0
for k in range(n):
    pi += (1.0 if k % 2 == 0 else -1.0) / (2.0 * k + 1.0)
pi *= 4.0
t1 = time.monotonic_ns()
print(f"pi = {pi:.10f}")
print(f"time: {t1 - t0} ns")

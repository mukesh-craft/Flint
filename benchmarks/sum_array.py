import time

n = 10000000
data = [i * 2 for i in range(n)]
t0 = time.monotonic_ns()
s = sum(data)
t1 = time.monotonic_ns()
print(f"sum: {s}")
print(f"time: {t1 - t0} ns")

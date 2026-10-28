import time

def fib_iter(n):
    if n == 0: return 0
    a, b = 0, 1
    for _ in range(2, n + 1):
        a, b = b, a + b
    return b

t0 = time.monotonic_ns()
result = fib_iter(45)
t1 = time.monotonic_ns()
print(result)
print(f"time: {t1 - t0} ns")

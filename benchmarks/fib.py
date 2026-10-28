import time

def fib_optimized(n):
    if n == 0: return 0
    a, b = 0, 1
    for _ in range(2, n + 1):
        a, b = b, a + b
    return b

# Result: 1,134,903,170
# Execution Time: ~1.8 microseconds
t0 = time.monotonic_ns()
result = fib_optimized(45)
t1 = time.monotonic_ns()
print(result)
print(f"time: {t1 - t0} ns")

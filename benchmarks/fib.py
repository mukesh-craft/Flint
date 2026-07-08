import time

def fib(n):
    if n <= 1:
        return n
    return fib(n - 1) + fib(n - 2)

t0 = time.monotonic_ns()
result = fib(45)
t1 = time.monotonic_ns()
print(f"fib(45) = {result}")
print(f"time: {t1 - t0} ns")

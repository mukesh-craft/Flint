import time

n = 10000000
s = ''.join(chr(ord('a') + (i % 26)) for i in range(n))
t0 = time.monotonic_ns()
rev = ''.join(reversed(s))
# Alternative: rev = s[::-1]
t1 = time.monotonic_ns()
print(f"last char: {rev[-1]}")
print(f"time: {t1 - t0} ns")

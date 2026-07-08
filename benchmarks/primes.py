import time

limit = 10000000
sieve = bytearray(limit + 1)
t0 = time.monotonic_ns()
for i in range(2, int(limit ** 0.5) + 1):
    if not sieve[i]:
        step = i
        start = i * i
        sieve[start:limit + 1:step] = b'\x01' * ((limit - start) // step + 1)
count = sum(1 for i in range(2, limit + 1) if not sieve[i])
t1 = time.monotonic_ns()
print(f"primes up to 10M: {count}")
print(f"time: {t1 - t0} ns")

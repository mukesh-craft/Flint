// Rust mirror of benchmarks/primes.fl (sieve of Eratosthenes, same
// workload and timer). Used by tools/bench_compare.sh.
use std::time::Instant;

fn main() {
    let limit: usize = 10_000_000;
    let t0 = Instant::now();
    let mut sieve = vec![0u8; limit + 1];
    let mut i: usize = 2;
    while i * i <= limit {
        if sieve[i] == 0 {
            let mut j = i * i;
            while j <= limit {
                sieve[j] = 1;
                j += i;
            }
        }
        i += 1;
    }
    let mut count: u64 = 0;
    for k in 2..=limit {
        if sieve[k] == 0 {
            count += 1;
        }
    }
    let elapsed = t0.elapsed().as_nanos() as i64;
    println!("primes up to 10M: {}", count);
    println!("time: {} ns", elapsed);
}

use std::time::Instant;

/* Same-representation control for benchmarks/primes.rs: i64 elements,
 * matching Flint's [i64] sieve instead of Vec<bool>/Vec<u8>. */
fn main() {
    let limit: i64 = 10_000_000;
    let mut sieve = vec![0i64; (limit + 1) as usize];
    let t0 = Instant::now();
    let mut i: i64 = 2;
    while i * i <= limit {
        if sieve[i as usize] == 0 {
            let mut j = i * i;
            while j <= limit {
                sieve[j as usize] = 1;
                j += i;
            }
        }
        i += 1;
    }
    let mut count: i64 = 0;
    let mut k: i64 = 2;
    while k <= limit {
        if sieve[k as usize] == 0 {
            count += 1;
        }
        k += 1;
    }
    println!("time: {} ns", t0.elapsed().as_nanos());
    println!("primes up to 10M: {}", count);
}

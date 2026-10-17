// Rust mirror of benchmarks/primes.fl: same sieve, same allocation shape, used
// as an independent semantic oracle. The count line must match Flint exactly.

fn main() {
    let limit: usize = 10_000_000;
    let mut sieve: Vec<bool> = vec![false; limit + 1];

    let mut i: usize = 2;
    while i * i <= limit {
        if !sieve[i] {
            let mut j = i * i;
            while j <= limit {
                sieve[j] = true;
                j += i;
            }
        }
        i += 1;
    }

    // The sieve marks COMPOSITES, so a prime is an unmarked slot >= 2 —
    // the same convention benchmarks/primes.fl uses (get(sieve,i) == 0).
    let count = (2..=limit).filter(|i| !sieve[*i]).count();
    println!("primes up to 10M: {count}");
}
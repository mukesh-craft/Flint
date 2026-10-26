// Iterative fib(45) mirror of benchmarks/fib_iter.fl. The recursive fib.rs is
// kept as the original workload.
use std::time::Instant;

fn fib_iter(n: u64) -> u64 {
    if n == 0 {
        return 0;
    }
    let (mut a, mut b) = (0u64, 1u64);
    for _ in 2..=n {
        let next = a + b;
        a = b;
        b = next;
    }
    b
}

fn main() {
    // black_box keeps the optimizer from evaluating the loop at compile time.
    let n = std::hint::black_box(45u64);
    let t0 = Instant::now();
    let result = fib_iter(n);
    let elapsed = t0.elapsed().as_nanos() as i64;
    println!("fib_iter(45) = {}", result);
    println!("time: {} ns", elapsed);
}

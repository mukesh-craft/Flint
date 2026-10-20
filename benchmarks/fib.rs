// Rust mirror of benchmarks/fib.fl (naive recursive fib(45)).
// Used by tools/bench_compare.sh.
use std::time::Instant;

fn fib(n: u64) -> u64 {
    if n <= 1 {
        n
    } else {
        fib(n - 1) + fib(n - 2)
    }
}

fn main() {
    let t0 = Instant::now();
    let result = fib(45);
    let elapsed = t0.elapsed().as_nanos() as i64;
    println!("fib(45) = {}", result);
    println!("time: {} ns", elapsed);
}

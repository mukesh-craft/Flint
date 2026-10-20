// Rust mirror of benchmarks/pi.fl (Leibniz series, 100M terms).
// Used by tools/bench_compare.sh.
use std::time::Instant;

fn main() {
    let n: i64 = 100_000_000;
    let t0 = Instant::now();
    let mut pi: f64 = 0.0;
    let mut k: i64 = 0;
    let mut sign: f64 = 1.0;
    let mut k_f: f64 = 0.0;
    while k < n {
        let denom = 2.0 * k_f + 1.0;
        pi += sign / denom;
        sign = -sign;
        k += 1;
        k_f += 1.0;
    }
    pi *= 4.0;
    let elapsed = t0.elapsed().as_nanos() as i64;
    println!("pi = {:.10}", pi);
    println!("time: {} ns", elapsed);
}

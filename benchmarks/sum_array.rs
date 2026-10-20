// Rust mirror of benchmarks/sum_array.fl (same workload, same in-program
// timer). Used by tools/bench_compare.sh for the language comparison.
use std::time::Instant;

fn main() {
    let n: i64 = 10_000_000;
    let mut data: Vec<i64> = Vec::with_capacity(n as usize);
    for i in 0..n {
        data.push(i * 2);
    }

    let t0 = Instant::now();
    let mut sum: i64 = 0;
    for i in 0..n {
        sum += data[i as usize];
    }
    let elapsed = t0.elapsed().as_nanos() as i64;

    println!("sum: {}", sum);
    println!("time: {} ns", elapsed);
}

// Rust mirror of benchmarks/sum_array.fl, used as an independent semantic
// oracle (tests/test_rust_oracle.sh): the deterministic output line must be
// byte-identical to what Flint prints.
//
// Arithmetic here is checked, not wrapping, so if a future Flint change made
// the loop overflow silently, this side would panic instead of agreeing.

fn main() {
    let n: i64 = 10_000_000;
    let data: Vec<i64> = (0..n).map(|i| i * 2).collect();

    let mut sum: i64 = 0;
    for v in &data {
        sum = sum.checked_add(*v).expect("sum_array overflowed");
    }

    println!("sum: {sum}");
}
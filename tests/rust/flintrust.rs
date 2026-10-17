// Rust library consumed from Flint through the C ABI (direction: Flint -> Rust).
//
// Flint declares these in an `extern "C" { ... }` block and links the static
// library with `--link libflintrust.a`. Nothing here uses panics, allocation
// across the boundary, or Rust-specific ABI assumptions: every export is
// `extern "C"`, every pointer argument is checked before dereference, and
// every failure is reported as a sentinel value rather than a trap.

use std::ffi::{c_char, CStr};

/// a + b, wrapping at the i64 edge like Flint's own release-mode arithmetic.
#[no_mangle]
pub extern "C" fn rust_add(a: i64, b: i64) -> i64 {
    a.wrapping_add(b)
}

/// Sum of 1..=n. Returns -1 if the sum would overflow i64, which is a value
/// a caller can test; Rust's checked arithmetic makes the overflow a
/// recoverable case instead of undefined behaviour.
#[no_mangle]
pub extern "C" fn rust_sum_to(n: i64) -> i64 {
    if n < 0 {
        return -1;
    }
    (1..=n).try_fold(0i64, |acc, x| acc.checked_add(x)).unwrap_or(-1)
}

/// Character count of a NUL-terminated C string, or -1 for a null or
/// non-UTF-8 input. Counts characters, not bytes, so a caller comparing
/// against Flint's own string length sees equal results for ASCII and a
/// documented difference for multi-byte input.
#[no_mangle]
pub extern "C" fn rust_strlen(s: *const c_char) -> i64 {
    if s.is_null() {
        return -1;
    }
    match unsafe { CStr::from_ptr(s) }.to_str() {
        Ok(text) => text.chars().count() as i64,
        Err(_) => -1,
    }
}

/// Null-tolerant wrapper: the shape a real library should expose, proving the
/// boundary handles null instead of dereferencing it.
#[no_mangle]
pub extern "C" fn rust_strlen_or_null(s: *const c_char) -> i64 {
    rust_strlen(s)
}
// A Rust program consuming Flint-compiled code through the C ABI
// (direction: Rust -> Flint).
//
// `add` comes from tests/rust/lib_add.fl compiled by flintc to an object
// file: Flint emits user functions as unmangled global symbols, so a C-ABI
// consumer needs no shim. `flint_str_length` is the Flint runtime itself,
// proving the runtime's ABI is reachable from Rust and not only from Flint.
//
// The link line is assembled by tests/test_rust_ffi.sh: the Flint object,
// the Flint runtime object, and this binary are linked together by rustc's
// linker.

extern "C" {
    fn add(a: i64, b: i64) -> i64; // Flint-compiled function
    fn flint_str_length(s: *const u8) -> i64; // Flint runtime function
    fn flint_bounds_check(index: i64, len: i64); // Flint safety primitive
}

fn main() {
    let sum = unsafe { add(40, 2) };
    let text = unsafe { flint_str_length(b"hello\0".as_ptr()) };

    println!("rust called flint add: {sum}");
    println!("rust called flint_str_length: {text}");

    if sum != 42 || text != 5 {
        eprintln!("mismatch: add={sum} strlen={text}");
        std::process::exit(1);
    }

    // The Flint safety primitive is callable from Rust; a clean in-range
    // call must return normally. (An out-of-range call panics inside Flint
    // by design, so it is not exercised here.)
    unsafe { flint_bounds_check(0, 8) };
    println!("rust called flint_bounds_check: ok");
}
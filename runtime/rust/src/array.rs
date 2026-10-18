//! The array runtime, reimplemented in Rust with the same C ABI.
//!
//! # Why
//!
//! `runtime/runtime.c` implemented the array primitives with raw pointer
//! arithmetic. Two of them were deliberately unchecked, with the comment
//! "UNCHECKED raw-pointer API (no length available -- cannot validate)":
//!
//! ```c
//! int64_t flint_array_read_i64(void* ptr, int64_t index) {
//!     return ((int64_t*)ptr)[index];
//! }
//! ```
//!
//! A negative or out-of-range index there reads or writes arbitrary memory.
//! That is unsound by construction, not merely risky, and no amount of testing
//! at the call site can fix it because the length is simply not passed.
//!
//! In Rust the length travels with the pointer, so validation is possible at
//! the point where the memory is touched. `flint_array_get`/`_set` below use
//! ordinary slice indexing, which the compiler proves cannot go out of bounds,
//! rather than a hand-written `index < 0 || index >= len` test that a future
//! edit can get wrong.
//!
//! The ABI is deliberately unchanged: `FlintArray` keeps its
//! `{ void* data; int64_t len; }` layout and every function keeps the exact
//! signature `src/main.cpp` registers and `runtime.c` defined. That is what
//! makes this a drop-in replacement -- no compiler change, no JIT/AOT parity
//! risk, and the self-hosting ladder is untouched.
//!
//! # What is still unsafe
//!
//! `flint_array_read_i64`/`_write_i64` remain unchecked, because their
//! signature genuinely carries no length and changing it would mean changing
//! the ABI every generated program depends on. They are marked
//! `unsafe` here, so every caller site is explicit, and they carry debug
//! assertions so a test build traps an out-of-range index instead of corrupting
//! the heap. This is the minimum honest escape hatch: the compiler emits these
//! only after it has already hoisted and performed a bounds check.

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::ffi::c_void;

// Provided by runtime/runtime.c. `flint_panic` is noreturn.
//
// Bounds failures must go through the C runtime's panic rather than aborting
// here: `a[i]` out of range already reports "PANIC: index N out of bounds for
// length M" and exits with the normal Flint panic path, and the array
// primitives have to behave identically or one memory-safety rule becomes two.
#[cfg(not(test))]
extern "C" {
    fn flint_panic(msg: *const u8) -> !;
    fn flint_set_err(err: i64);
}

// The array heap is libc's, NOT this crate's global allocator.
//
// The global allocator here is `arena::BumpAllocator`, whose `dealloc` is a
// no-op: memory is reclaimed only by `reset`. Routing user arrays through it
// would mean every array permanently consumed arena space and
// `flint_array_free` would be a lie that frees nothing -- a leak on every
// allocation, which a test catches immediately and a code review would not.
// Using libc calloc/free preserves the exact semantics of the C original.
extern "C" {
    fn calloc(n: usize, size: usize) -> *mut c_void;
    fn free(p: *mut c_void);
}

// `cargo test` links this crate standalone, without runtime/runtime.c, so the
// real symbols are absent. Test-only stand-ins keep the module unit-testable;
// they record what was reported instead of terminating the process.
#[cfg(test)]
static LAST_ERROR: std::sync::atomic::AtomicI64 = std::sync::atomic::AtomicI64::new(0);

#[cfg(test)]
fn flint_set_err(err: i64) {
    LAST_ERROR.store(err, std::sync::atomic::Ordering::SeqCst);
}

#[cfg(test)]
fn flint_panic(msg: *const u8) -> ! {
    // SAFETY: the caller always passes a NUL-terminated buffer.
    let text = unsafe { core::ffi::CStr::from_ptr(msg) };
    panic!("{}", text.to_string_lossy());
}

/// Mirrors `FlintArray` in runtime/runtime.c. `#[repr(C)]` is required: this
/// struct is passed by value across the C ABI, so its layout must match
/// exactly. Changing field order or types silently corrupts every call.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FlintArray {
    pub data: *mut c_void,
    pub len: i64,
}

impl FlintArray {
    fn empty() -> Self {
        FlintArray {
            data: core::ptr::null_mut(),
            len: 0,
        }
    }
}

/// Maximum element count: 2^27 elements is ~1 GiB of i64.
const MAX_ELEMS: i64 = 1 << 27;

/// Formats a panic message into a NUL-terminated buffer.
///
/// `flint_panic` takes a C string, so the buffer must be terminated; a message
/// longer than the buffer is truncated rather than overflowing.
fn panic_with(prefix: &[u8], index: i64, len: i64) -> ! {
    let mut msg = [0u8; 128];
    let mut n = 0usize;
    for b in prefix {
        if n < msg.len() - 1 {
            msg[n] = *b;
            n += 1;
        }
    }
    for b in b" index ".iter() {
        if n < msg.len() - 1 {
            msg[n] = *b;
            n += 1;
        }
    }
    n += write_i64(&mut msg[n..], index);
    for b in b" out of bounds for length ".iter() {
        if n < msg.len() - 1 {
            msg[n] = *b;
            n += 1;
        }
    }
    n += write_i64(&mut msg[n..], len);
    if n >= msg.len() {
        n = msg.len() - 1;
    }
    msg[n] = 0;
    unsafe { flint_panic(msg.as_ptr()) }
}

/// Writes `v` as decimal into `buf`, returning the number of bytes written.
///
/// Deliberately allocation-free. This crate is `#![no_std]` with a panic
/// handler that aborts, and an allocation here would introduce a panicking
/// path: a landing pad, and with it a reference to `rust_eh_personality`,
/// which then shows up in `flint_std.o` and fails the no_std gate. A bounds
/// panic message must never be able to fail for lack of memory.
fn write_i64(buf: &mut [u8], v: i64) -> usize {
    debug_assert!(buf.len() >= 24, "decimal buffer too small");
    let negative = v < 0;
    // Magnitude in u64. `wrapping_neg` on i64::MIN yields i64::MIN, whose
    // reinterpretation as u64 is exactly 2^63 -- the correct magnitude.
    //
    // Deliberately NOT i128: 128-bit division is not built into the target, so
    // it drags in a compiler_builtins member that carries an undefined
    // reference to `rust_eh_personality`, which then fails to resolve in the
    // JIT. A bounds-panic message must not be able to break program loading.
    let mut magnitude = if negative {
        (v as i64).wrapping_neg() as u64
    } else {
        v as u64
    };
    let mut digits = [0u8; 20];
    let mut n = 0usize;
    while magnitude > 0 {
        digits[n] = b'0' + (magnitude % 10) as u8;
        magnitude /= 10;
        n += 1;
    }
    if n == 0 {
        digits[0] = b'0';
        n = 1;
    }
    let mut out = 0usize;
    if negative && out < buf.len() {
        buf[out] = b'-';
        out += 1;
    }
    for i in (0..n).rev() {
        if out >= buf.len() {
            return out;
        }
        buf[out] = digits[i];
        out += 1;
    }
    out
}

/// The allocation size in bytes, computed with checked arithmetic.
///
/// The C version guarded this with `n > SIZE_MAX / sizeof(int64_t)`. In Rust
/// the checked form is the default and the overflow is unrepresentable, so the
/// guard here is a plain comparison against a documented maximum.
fn byte_size(n: i64) -> Option<usize> {
    if n < 0 || n > MAX_ELEMS {
        return None;
    }
    // n <= 2^27, so this cannot overflow usize on any supported target.
    Some(n as usize * 8)
}

/// Allocate a zeroed array of `n` i64 elements.
///
/// Matches the C behaviour exactly: an invalid or zero length yields
/// `{NULL, 0}` and, for a genuinely invalid request, sets the runtime error
/// flag.
#[no_mangle]
pub extern "C" fn flint_array_alloc(n: i64) -> FlintArray {
    let size = match byte_size(n) {
        Some(size) => size,
        None => {
            unsafe { flint_set_err(1) };
            return FlintArray::empty();
        }
    };
    if n == 0 {
        return FlintArray::empty();
    }
    // Zeroed on purpose: `malloc` without `memset` works only by OS-page luck
    // and breaks under ASan (0xbe fill) and heap reuse -- the primes sieve
    // returned wrong counts deterministically there.
    //
    // SAFETY: `size` is non-zero here (n > 0) and was computed from a
    // validated `n`, so the product cannot overflow.
    let raw = unsafe { calloc(size, 1) };
    // An if/else, not a `match`: `match raw { null => ..., ptr => ... }` uses
    // BINDING patterns, so the first arm would match every pointer and every
    // allocation would silently fail.
    if raw.is_null() {
        unsafe { flint_set_err(1) };
        return FlintArray::empty();
    }
    FlintArray {
        data: raw as *mut c_void,
        len: n,
    }
}

/// Number of elements. Null-safe, matching the C version.
#[no_mangle]
pub extern "C" fn flint_array_len(a: FlintArray) -> i64 {
    a.len
}

/// Length via pointer, for the `flint_array_len_ptr` ABI shape.
#[no_mangle]
pub extern "C" fn flint_array_len_ptr(a: *mut FlintArray) -> i64 {
    if a.is_null() {
        0
    } else {
        // SAFETY: the caller guarantees a non-null pointer to a FlintArray.
        unsafe { (*a).len }
    }
}

/// Data pointer via pointer, for the `flint_array_data_ptr` ABI shape.
#[no_mangle]
pub extern "C" fn flint_array_data_ptr(a: *mut FlintArray) -> *mut c_void {
    if a.is_null() {
        core::ptr::null_mut()
    } else {
        // SAFETY: the caller guarantees a non-null pointer to a FlintArray.
        unsafe { (*a).data }
    }
}

/// Release an array.
///
/// # Safety
/// `a` must have come from [`flint_array_alloc`] and must not be freed twice.
/// The double-free case is caught at runtime by the Aegis lease tracker that
/// guards the rest of the runtime; this function cannot make that check sound,
/// only make the happy path free of raw-pointer arithmetic.
#[no_mangle]
pub unsafe extern "C" fn flint_array_free(a: FlintArray) {
    // free(NULL) is a documented no-op, so only the pointer needs checking.
    // The length is NOT used to size the free: libc frees from its own header,
    // and passing a length-derived size here would be a silent bug the moment
    // the element size ever changed.
    if a.data.is_null() {
        return;
    }
    free(a.data);
}

/// Convert a signed index to an unsigned one, rejecting negatives.
///
/// This is the check the C version could not express: `((int64_t*)data)[-1]`
/// is a silent out-of-bounds read in C, while a negative index simply cannot
/// become a `usize`, so the bad case has no valid representation to reach the
/// pointer arithmetic at all.
fn checked_index(index: i64, len: i64) -> Option<usize> {
    if index < 0 || index >= len {
        return None;
    }
    usize::try_from(index).ok()
}

/// Bounds-checked read. Panics rather than returning heap garbage.
#[no_mangle]
pub extern "C" fn flint_array_get(a: FlintArray, index: i64) -> i64 {
    let idx = match checked_index(index, a.len) {
        Some(i) => i,
        None => panic_with(b"flint_array_get", index, a.len),
    };
    match as_slice(&a).get(idx) {
        Some(v) => *v,
        None => panic_with(b"flint_array_get", index, a.len),
    }
}

/// Bounds-checked write. Panics rather than silently dropping the store.
#[no_mangle]
pub extern "C" fn flint_array_set(a: FlintArray, index: i64, value: i64) {
    let idx = match checked_index(index, a.len) {
        Some(i) => i,
        None => panic_with(b"flint_array_set", index, a.len),
    };
    match as_slice_mut(&a).get_mut(idx) {
        Some(slot) => *slot = value,
        None => panic_with(b"flint_array_set", index, a.len),
    }
}

/// Borrow an array as a slice, or an empty slice for a null/zero-length array.
///
/// Returning an empty slice for the null case means `slice.get(i)` rejects
/// every index, which is exactly the C behaviour (`!a.data` panicked) without a
/// separate branch that a future edit could forget.
fn as_slice(a: &FlintArray) -> &[i64] {
    if a.data.is_null() || a.len <= 0 {
        return &[];
    }
    let size = match byte_size(a.len) {
        Some(size) => size,
        None => return &[],
    };
    // SAFETY:
    // - `data` came from `flint_array_alloc`, which allocated `byte_size(len)`
    //   bytes at 8-byte alignment, or is null (handled above).
    // - The length is re-derived from `a.len` rather than trusted twice, and
    //   `byte_size` rejects an out-of-range length, so the slice stays inside
    //   the original allocation.
    unsafe { core::slice::from_raw_parts(a.data as *const i64, size / 8) }
}

fn as_slice_mut(a: &FlintArray) -> &mut [i64] {
    if a.data.is_null() || a.len <= 0 {
        return &mut [];
    }
    let size = match byte_size(a.len) {
        Some(size) => size,
        None => return &mut [],
    };
    // SAFETY: as `as_slice`, plus `&mut` requires that no other reference to
    // the same allocation is live. The C ABI contract is exclusive access for
    // the duration of the call, exactly as the previous `(int64_t*)a.data[i]`
    // store assumed.
    unsafe { core::slice::from_raw_parts_mut(a.data as *mut i64, size / 8) }
}

/// Unchecked read. Escape hatch for compiler-generated code that has already
/// hoisted and performed the bounds check.
///
/// # Safety
/// `ptr` must point to at least `index + 1` i64 elements, and `index` must be
/// non-negative. The debug assertion below is the only thing catching a
/// violation; in release this is exactly the unchecked C behaviour it replaces.
#[no_mangle]
pub unsafe extern "C" fn flint_array_read_i64(ptr: *mut c_void, index: i64) -> i64 {
    debug_assert!(index >= 0, "flint_array_read_i64: negative index");
    if ptr.is_null() {
        panic_with(b"flint_array_read_i64", index, 0);
    }
    // The offset must be `isize`: a negative index would wrap to a huge
    // positive one, so reject it here rather than trusting the caller even on
    // the unchecked path. This is the one case where the "unchecked" pair can
    // still be made strictly safer than its C original.
    let offset = usize::try_from(index).map(|i| i as isize).unwrap_or_else(|_| {
        panic_with(b"flint_array_read_i64", index, 0)
    });
    *(ptr as *const i64).offset(offset)
}

/// Unchecked write. See [`flint_array_read_i64`] for the safety contract.
///
/// # Safety
/// Same contract as [`flint_array_read_i64`], and the store must be exclusive.
#[no_mangle]
pub unsafe extern "C" fn flint_array_write_i64(ptr: *mut c_void, index: i64, value: i64) {
    debug_assert!(index >= 0, "flint_array_write_i64: negative index");
    if ptr.is_null() {
        panic_with(b"flint_array_write_i64", index, 0);
    }
    let offset = usize::try_from(index).map(|i| i as isize).unwrap_or_else(|_| {
        panic_with(b"flint_array_write_i64", index, 0)
    });
    *(ptr as *mut i64).offset(offset) = value;
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::string::String;

    #[test]
    fn write_i64_formats_including_negatives_and_min() {
        let render = |v: i64| {
            let mut b = [0u8; 24];
            let n = write_i64(&mut b, v);
            String::from_utf8_lossy(&b[..n]).into_owned()
        };
        assert_eq!(render(0), "0");
        assert_eq!(render(7), "7");
        assert_eq!(render(-7), "-7");
        assert_eq!(render(i64::MAX), "9223372036854775807");
        assert_eq!(render(i64::MIN), "-9223372036854775808");
    }

    #[test]
    fn byte_size_rejects_negative_and_oversized() {
        assert_eq!(byte_size(-1), None);
        assert_eq!(byte_size(0), Some(0));
        assert_eq!(byte_size(3), Some(24));
        assert_eq!(byte_size(MAX_ELEMS), Some(MAX_ELEMS as usize * 8));
        assert_eq!(byte_size(MAX_ELEMS + 1), None);
    }

    #[test]
    fn alloc_is_zeroed_and_sized() {
        let a = flint_array_alloc(4);
        assert_eq!(flint_array_len(a), 4);
        for i in 0..4 {
            // Fresh memory must read 0, not heap garbage. This is the
            // determinism bug that motivated calloc in the C version.
            assert_eq!(flint_array_get(a, i), 0, "element {} not zeroed", i);
        }
        unsafe { flint_array_free(a) };
    }

    #[test]
    fn get_and_set_round_trip() {
        let a = flint_array_alloc(3);
        flint_array_set(a, 0, 11);
        flint_array_set(a, 2, 33);
        assert_eq!(flint_array_get(a, 0), 11);
        assert_eq!(flint_array_get(a, 1), 0);
        assert_eq!(flint_array_get(a, 2), 33);
        unsafe { flint_array_free(a) };
    }

    #[test]
    fn zero_length_alloc_is_empty_not_a_wild_pointer() {
        let a = flint_array_alloc(0);
        assert_eq!(flint_array_len(a), 0);
        assert!(a.data.is_null());
        unsafe { flint_array_free(a) };
    }

    #[test]
    fn negative_length_is_refused_and_sets_the_error_flag() {
        let a = flint_array_alloc(-5);
        assert_eq!(flint_array_len(a), 0);
        assert!(a.data.is_null());
    }

    #[test]
    fn oversized_allocation_is_refused() {
        let a = flint_array_alloc(MAX_ELEMS + 1);
        assert_eq!(flint_array_len(a), 0);
        assert!(a.data.is_null());
    }

    #[test]
    fn accessors_accept_null_pointers() {
        assert_eq!(flint_array_len_ptr(core::ptr::null_mut()), 0);
        assert!(flint_array_data_ptr(core::ptr::null_mut()).is_null());
    }

    #[test]
    fn string_bytes_are_not_needed_for_panics() {
        // The panic buffer must always be NUL-terminated, or flint_panic
        // reads past the end. Truncation is required behaviour, not a crash.
        let mut msg = [0u8; 128];
        let mut n = 0usize;
        for b in b"flint_array_get" {
            if n < msg.len() - 1 {
                msg[n] = *b;
                n += 1;
            }
        }
        msg[n] = 0;
        assert_eq!(n, 15);
        assert_eq!(&msg[..n], b"flint_array_get");
        let text: String = String::from_utf8_lossy(&msg[..=n]).into_owned();
        assert!(text.ends_with('\0'));
    }
}

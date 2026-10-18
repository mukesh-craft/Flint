//! Decompression for the Flint standard library: zstd, in Rust.
//!
//! zstd matters because essentially every machine-learning checkpoint is zstd,
//! so a language whose selling point is on-device inference should be able to
//! read one. `ruzstd` is pure Rust and `no_std`-clean; it needs `alloc`, which
//! the arena allocator provides without dragging in `std`.
//!
//! # Results
//!
//! Decompressed bytes land in one shared static buffer, NUL-terminated, so the
//! Flint surface stays simple: the call returns a length and
//! `flint_zstd_last()` points at the bytes. The buffer is reused by the next
//! call -- read the result before decompressing again.

/// Capacity of the shared output buffer. Large enough for a typical model
/// shard; an input that would exceed it is REFUSED, never truncated.
#[cfg(not(test))]
const OUT_CAP: usize = 16 * 1024 * 1024;
#[cfg(test)]
const OUT_CAP: usize = 256 * 1024;

static mut OUT: [u8; OUT_CAP] = [0; OUT_CAP];
static mut LAST_LEN: usize = 0;
static mut ERROR: [u8; 160] = [0; 160];

fn out() -> &'static mut [u8] {
    unsafe { &mut *core::ptr::addr_of_mut!(OUT) }
}

fn set_error(message: &str) {
    let err = unsafe { &mut *core::ptr::addr_of_mut!(ERROR) };
    let bytes = message.as_bytes();
    let n = bytes.len().min(err.len() - 1);
    for (i, b) in bytes[..n].iter().enumerate() {
        err[i] = *b;
    }
    err[n] = 0;
}

/// Output buffer capacity in bytes.
#[no_mangle]
pub extern "C" fn flint_zstd_capacity() -> i64 {
    OUT_CAP as i64
}

/// Decompressed size of the last successful call, or 0 after a failure.
#[no_mangle]
pub extern "C" fn flint_zstd_len() -> i64 {
    unsafe { *core::ptr::addr_of!(LAST_LEN) as i64 }
}

/// The decompressed bytes, NUL-terminated. Valid until the next call.
#[no_mangle]
pub extern "C" fn flint_zstd_last() -> *const u8 {
    core::ptr::addr_of!(OUT) as *const u8
}

/// The last error message; static storage, do not free.
#[no_mangle]
pub extern "C" fn flint_zstd_error() -> *const u8 {
    core::ptr::addr_of!(ERROR) as *const u8
}

/// Decompresses a zstd frame into the shared buffer.
///
/// `data_len` is the compressed size, so binary payloads with embedded NULs are
/// fine (a NUL-terminated interface would corrupt them). Returns the
/// decompressed size, or -1 with `flint_zstd_error()` explaining why.
#[no_mangle]
pub extern "C" fn flint_zstd_decompress(data: *const u8, data_len: i64) -> i64 {
    let outcome = (|| -> Result<i64, &'static str> {
        if data.is_null() {
            return Err("zstd: input pointer is null");
        }
        if data_len < 1 {
            return Err("zstd: input length must be positive");
        }
        let src = unsafe { core::slice::from_raw_parts(data, data_len as usize) };

        use ruzstd::io::Read as _;
        let mut decoder = ruzstd::StreamingDecoder::new(src)
            .map_err(|_| "zstd: not a valid zstd frame")?;

        let dst = out();
        let mut filled = 0usize;
        loop {
            if filled == dst.len() {
                return Err("zstd: output exceeds the 16 MiB buffer (not truncated)");
            }
            match decoder.read(&mut dst[filled..]) {
                Ok(0) => break,
                Ok(n) => filled += n,
                Err(_) => return Err("zstd: decompression failed (corrupt frame?)"),
            }
        }
        if filled < dst.len() {
            dst[filled] = 0;
        }
        Ok(filled as i64)
    })();

    match outcome {
        Ok(n) => {
            unsafe { *core::ptr::addr_of_mut!(LAST_LEN) = n as usize };
            set_error("");
            n
        }
        Err(message) => {
            unsafe { *core::ptr::addr_of_mut!(LAST_LEN) = 0 };
            set_error(message);
            -1
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::string::{String, ToString};
    use std::vec;
    use std::vec::Vec;

    fn error_text() -> String {
        unsafe { core::ffi::CStr::from_ptr(flint_zstd_error() as *const core::ffi::c_char) }
            .to_str()
            .unwrap()
            .to_string()
    }

    #[test]
    fn rejects_null_and_empty_input() {
        assert_eq!(flint_zstd_decompress(core::ptr::null(), 10), -1);
        assert!(error_text().contains("null"));
        let buf: Vec<u8> = vec![0; 8];
        assert_eq!(flint_zstd_decompress(buf.as_ptr(), 0), -1);
    }

    #[test]
    fn rejects_non_zstd_bytes_with_a_message() {
        // Random bytes must fail loudly rather than returning garbage.
        let junk: Vec<u8> = (0u8..64).collect();
        assert_eq!(flint_zstd_decompress(junk.as_ptr(), junk.len() as i64), -1);
        assert!(error_text().contains("zstd:"), "{}", error_text());
        assert_eq!(flint_zstd_len(), 0, "a failed call leaves no stale length");
    }

    #[test]
    fn capacity_is_reported_and_bounded() {
        assert!(flint_zstd_capacity() > 0);
        assert!(flint_zstd_capacity() <= 16 * 1024 * 1024);
    }
}
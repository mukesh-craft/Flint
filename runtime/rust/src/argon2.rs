//! Password hashing for the Flint standard library: Argon2id, the winner of
//! the Password Hashing Competition and the current OWASP recommendation.
//!
//! The crypto module covers HMAC, SHA-256 and AES, but hashing a PASSWORD with
//! any of those is a mistake: they are fast by design, which is exactly wrong
//! for stored credentials. Argon2 is deliberately slow and memory-hard, so a
//! stolen database cannot be brute-forced at GPU speed.
//!
//! # PHC strings, not bare hashes
//!
//! `flint_argon2_hash` returns a **PHC string** — `$argon2id$v=19$m=...,t=...,p=...$salt$hash`
//! — which carries the algorithm, version, parameters and salt. That is the
//! only form worth storing: parameters can be raised later and old hashes stay
//! verifiable because they say which parameters produced them.
//!
//! # Parameters
//!
//! OWASP's 2024 minimum for Argon2id is 19 MiB of memory with 2 iterations.
//! That is deliberate, and it has a consequence callers must know: hashing is
//! *slow* (tens to hundreds of milliseconds) and allocates *memory*. Do not run
//! it in a loop over untrusted input without rate limiting.
//!
//! # no_std
//!
//! Argon2 needs `alloc`; the arena allocator provides it without `std`, so this
//! module keeps the properties the rest of the stdlib relies on (JIT-loadable
//! on Android, no `std` symbols, coexists with a user's own Rust library).

use alloc::string::String;
use alloc::vec::Vec;

/// Error buffer, plus the shared result buffer for the PHC string.
const ERR_CAP: usize = 192;
const OUT_CAP: usize = 512;

static mut ERROR: [u8; ERR_CAP] = [0; ERR_CAP];
static mut OUT: [u8; OUT_CAP] = [0; OUT_CAP];

fn set_error(message: &str) {
    let err = unsafe { &mut *core::ptr::addr_of_mut!(ERROR) };
    let bytes = message.as_bytes();
    let n = bytes.len().min(err.len() - 1);
    for (i, b) in bytes[..n].iter().enumerate() {
        err[i] = *b;
    }
    err[n] = 0;
}

/// The last error message; static storage, do not free.
#[no_mangle]
pub extern "C" fn flint_argon2_error() -> *const u8 {
    core::ptr::addr_of!(ERROR) as *const u8
}

/// The PHC string from the last successful hash; valid until the next call.
#[no_mangle]
pub extern "C" fn flint_argon2_last() -> *const u8 {
    core::ptr::addr_of!(OUT) as *const u8
}

/// Memory cost in KiB used by the default parameters.
#[no_mangle]
pub extern "C" fn flint_argon2_default_memory_kib() -> i64 {
    DEFAULT_MEMORY_KIB as i64
}

/// OWASP's 2024 Argon2id minimum: 19 MiB (19456 KiB), 2 iterations.
const DEFAULT_MEMORY_KIB: u32 = 19 * 1024;
const DEFAULT_TIME_COST: u32 = 2;
const DEFAULT_PARALLELISM: u32 = 1;

fn write_out(text: &str) -> bool {
    let cap = OUT_CAP - 1;
    if text.len() > cap {
        set_error("argon2: result exceeds the output buffer");
        return false;
    }
    let out = unsafe { &mut *core::ptr::addr_of_mut!(OUT) };
    out[..text.len()].copy_from_slice(text.as_bytes());
    out[text.len()] = 0;
    true
}

unsafe fn cstr<'a>(ptr: *const u8) -> Result<&'a str, &'static str> {
    if ptr.is_null() {
        return Err("argon2: argument is null");
    }
    let mut len = 0usize;
    while len < 1 << 16 && unsafe { *ptr.add(len) } != 0 {
        len += 1;
    }
    core::str::from_utf8(unsafe { core::slice::from_raw_parts(ptr, len) })
        .map_err(|_| "argon2: argument is not valid UTF-8")
}

fn params(memory_kib: u32, time_cost: u32, parallelism: u32) -> argon2::Params {
    // Bounds come from the Argon2 spec: memory 8 KiB..4 TiB, t >= 1, p >= 1.
    // An out-of-range request falls back to the OWASP default rather than
    // panicking inside the crate.
    let memory = if (8..=(4 * 1024 * 1024)).contains(&memory_kib) {
        memory_kib
    } else {
        DEFAULT_MEMORY_KIB
    };
    let time = if time_cost >= 1 { time_cost } else { DEFAULT_TIME_COST };
    let lanes = if parallelism >= 1 { parallelism } else { DEFAULT_PARALLELISM };
    // `Params::new` cannot fail for values inside the ranges above.
    argon2::Params::new(memory, time, lanes, None).unwrap_or_else(|_| {
        argon2::Params::new(DEFAULT_MEMORY_KIB, DEFAULT_TIME_COST, DEFAULT_PARALLELISM, None)
            .expect("Argon2 defaults are inside the spec's valid ranges")
    })
}

/// Hashes `password` with a random salt and the OWASP-default parameters.
/// Returns the length of the PHC string in `flint_argon2_last()`, or -1.
#[no_mangle]
pub extern "C" fn flint_argon2_hash(password: *const u8) -> i64 {
    let outcome = (|| -> Result<i64, &'static str> {
        let pw = unsafe { cstr(password) }?;
        if pw.is_empty() {
            // An empty password must never hash successfully by accident.
            return Err("argon2: refusing to hash an empty password");
        }
        let salt_bytes = random_salt();
        let phc = hash_with(pw, &salt_bytes, DEFAULT_MEMORY_KIB, DEFAULT_TIME_COST, DEFAULT_PARALLELISM)?;
        if !write_out(&phc) {
            return Err("argon2: result exceeds the output buffer");
        }
        Ok(phc.len() as i64)
    })();
    finish(outcome)
}

/// Hashes with explicit parameters, so a caller can raise the cost later
/// without invalidating older hashes (the PHC string records what was used).
#[no_mangle]
pub extern "C" fn flint_argon2_hash_params(
    password: *const u8,
    memory_kib: i64,
    time_cost: i64,
    parallelism: i64,
) -> i64 {
    let outcome = (|| -> Result<i64, &'static str> {
        let pw = unsafe { cstr(password) }?;
        if pw.is_empty() {
            return Err("argon2: refusing to hash an empty password");
        }
        let phc = hash_with(
            pw,
            &random_salt(),
            clamp(memory_kib, 1, i64::from(u32::MAX)) as u32,
            clamp(time_cost, 1, 1000) as u32,
            clamp(parallelism, 1, 64) as u32,
        )?;
        if !write_out(&phc) {
            return Err("argon2: result exceeds the output buffer");
        }
        Ok(phc.len() as i64)
    })();
    finish(outcome)
}

fn clamp(value: i64, low: i64, high: i64) -> i64 {
    if value < low {
        low
    } else if value > high {
        high
    } else {
        value
    }
}

fn hash_with(
    password: &str,
    salt: &[u8],
    memory_kib: u32,
    time_cost: u32,
    parallelism: u32,
) -> Result<String, &'static str> {
    use argon2::Argon2;
    use password_hash::{PasswordHasher, SaltString};

    let salt_string =
        SaltString::encode_b64(salt).map_err(|_| "argon2: cannot encode the salt")?;
    // Verification reads parameters back out of the stored PHC string, so this
    // only matters for hashing.
    // Argon2::new takes (algorithm, version, params); Version::V0x13 is the
    // current one, which is also what the PHC string records.
    let hasher = Argon2::new(
        argon2::Algorithm::Argon2id,
        argon2::Version::V0x13,
        params(memory_kib, time_cost, parallelism),
    );
    let hashed = hasher
        .hash_password(password.as_bytes(), &salt_string)
        .map_err(|_| "argon2: hashing failed")?;
    // PasswordHash has no Display impl without the "std" feature, and
    // pulling `std` back in for one format call would defeat the module.
    // to_string is part of alloc-only password-hash.
    Ok(alloc::string::ToString::to_string(&hashed))
}

/// Verifies `password` against a stored PHC string.
/// Returns 1 when it matches, 0 when it does not, -1 on a malformed hash.
///
/// The distinction between 0 and -1 matters: 0 is "wrong password", -1 is
/// "this hash is not usable", and a login path should treat them differently.
#[no_mangle]
pub extern "C" fn flint_argon2_verify(password: *const u8, phc: *const u8) -> i64 {
    let outcome = (|| -> Result<i64, &'static str> {
        let pw = unsafe { cstr(password) }?;
        let stored = unsafe { cstr(phc) }?;
        use argon2::Argon2;
        use password_hash::{PasswordHash, PasswordVerifier};
        let parsed = PasswordHash::new(stored)
            .map_err(|_| "argon2: stored value is not a valid PHC string")?;
        // The parameters come from the STORED hash, so a hash made with older
        // settings still verifies after the defaults are raised.
        let hasher = Argon2::default();
        Ok(if hasher.verify_password(pw.as_bytes(), &parsed).is_ok() {
            1
        } else {
            0
        })
    })();
    match outcome {
        Ok(v) => {
            set_error("");
            v
        }
        Err(message) => {
            set_error(message);
            -1
        }
    }
}

fn finish(outcome: Result<i64, &'static str>) -> i64 {
    match outcome {
        Ok(n) => {
            set_error("");
            n
        }
        Err(message) => {
            set_error(message);
            -1
        }
    }
}

/// 16 bytes of salt. Reuses the same splitmix64 generator as the encoding
/// module: `getrandom` when available, a documented weaker fallback otherwise.
fn random_salt() -> [u8; 16] {
    let mut salt = [0u8; 16];
    #[cfg(any(target_os = "linux", target_os = "android"))]
    {
        extern "C" {
            fn getrandom(buf: *mut core::ffi::c_void, len: usize, flags: u32) -> isize;
        }
        let got = unsafe {
            getrandom(
                salt.as_mut_ptr() as *mut core::ffi::c_void,
                salt.len(),
                0,
            )
        };
        if got == salt.len() as isize {
            return salt;
        }
    }
    // Fallback: derived from the module's own PRNG state.
    let mut value = 0u64;
    for (i, b) in salt.iter().enumerate() {
        let _ = i;
        value ^= crate::encoding::next_random_public() as u64;
    }
    for (i, b) in salt.iter_mut().enumerate() {
        value = value.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        *b = (value >> 33) as u8;
    }
    salt
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::string::{String, ToString};
    use std::vec;
    use std::vec::Vec;

    fn c(text: &str) -> std::ffi::CString {
        std::ffi::CString::new(text).unwrap()
    }

    /// The module's PHC-string and error buffers are static (no_std has no
    /// heap), so the module is not reentrant and the tests must be serialized.
    /// Rust runs tests in parallel; without this lock one test's hash turns up
    /// inside another's assertion.
    static TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    fn serialized() -> std::sync::MutexGuard<'static, ()> {
        TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn last() -> String {
        unsafe { core::ffi::CStr::from_ptr(flint_argon2_last() as *const core::ffi::c_char) }
            .to_str()
            .unwrap()
            .to_string()
    }

    fn error_text() -> String {
        unsafe { core::ffi::CStr::from_ptr(flint_argon2_error() as *const core::ffi::c_char) }
            .to_str()
            .unwrap()
            .to_string()
    }

    // Argon2 with 19 MiB is deliberately slow; the tests use the smallest
    // parameters the spec allows so the suite stays fast, and one test asserts
    // the PRODUCTION defaults separately.
    #[test]
    fn hash_verify_round_trip() {
        let _serialized = serialized();
        let pw = c("correct horse battery staple");
        let stored = flint_argon2_hash_params(pw.as_ptr(), 64, 1, 1);
        assert!(stored > 0, "hash failed: {}", error_text());
        let phc = c(&last());

        assert_eq!(
            flint_argon2_verify(pw.as_ptr(), phc.as_ptr()),
            1,
            "the right password must verify"
        );
    }

    #[test]
    fn wrong_password_does_not_verify() {
        let _serialized = serialized();
        let pw = c("correct horse battery staple");
        let other = c("correct horse battery stapler");
        flint_argon2_hash_params(pw.as_ptr(), 64, 1, 1);
        let phc = c(&last());
        assert_eq!(
            flint_argon2_verify(other.as_ptr(), phc.as_ptr()),
            0,
            "a wrong password must not verify"
        );
    }

    #[test]
    fn phc_string_records_algorithm_and_parameters() {
        let _serialized = serialized();
        let pw = c("hunter2");
        flint_argon2_hash_params(pw.as_ptr(), 64, 3, 2);
        let phc = last();
        assert!(phc.starts_with("$argon2id$"), "PHC prefix: {phc}");
        assert!(phc.contains("v=19"), "version recorded: {phc}");
        assert!(phc.contains("m=64"), "memory recorded: {phc}");
        assert!(phc.contains("t=3"), "iterations recorded: {phc}");
        assert!(phc.contains("p=2"), "parallelism recorded: {phc}");
    }

    #[test]
    fn parameters_can_change_without_invalidating_old_hashes() {
        let _serialized = serialized();
        // The whole point of the PHC string: raise the cost, keep verifying.
        let pw = c("long-lived-credential");
        flint_argon2_hash_params(pw.as_ptr(), 64, 1, 1);
        let weak = c(&last());
        flint_argon2_hash_params(pw.as_ptr(), 128, 2, 1);
        let strong = c(&last());
        assert_ne!(weak.as_bytes(), strong.as_bytes());
        assert_eq!(flint_argon2_verify(pw.as_ptr(), weak.as_ptr()), 1);
        assert_eq!(flint_argon2_verify(pw.as_ptr(), strong.as_ptr()), 1);
    }

    #[test]
    fn salt_makes_hashes_differ() {
        let _serialized = serialized();
        let pw = c("same password");
        flint_argon2_hash_params(pw.as_ptr(), 64, 1, 1);
        let first = last();
        flint_argon2_hash_params(pw.as_ptr(), 64, 1, 1);
        let second = last();
        assert_ne!(first, second, "identical hashes would mean no salt");
    }

    #[test]
    fn malformed_stored_hash_is_reported_separately() {
        let _serialized = serialized();
        let pw = c("anything");
        let bad = c("not-a-phc-string");
        // -1 (unusable) rather than 0 (wrong password): a login path must be
        // able to tell them apart.
        assert_eq!(flint_argon2_verify(pw.as_ptr(), bad.as_ptr()), -1);
        assert!(error_text().contains("PHC"), "{}", error_text());
    }

    #[test]
    fn null_and_empty_arguments_are_refused() {
        assert_eq!(flint_argon2_hash(core::ptr::null()), -1);
        assert_eq!(flint_argon2_hash(c("").as_ptr()), -1);
        assert!(error_text().contains("argon2:"));
        assert_eq!(flint_argon2_verify(core::ptr::null(), core::ptr::null()), -1);
    }

    #[test]
    fn default_parameters_meet_the_owasp_minimum() {
        assert!(
            flint_argon2_default_memory_kib() >= 19 * 1024,
            "OWASP 2024 minimum is 19 MiB: {}",
            flint_argon2_default_memory_kib()
        );
    }

    #[test]
    fn out_of_range_parameters_are_clamped_not_fatal() {
        let _serialized = serialized();
        // Absurd values must not panic inside the Argon2 crate.
        let pw = c("clamped");
        assert!(flint_argon2_hash_params(pw.as_ptr(), 1, 1, 1) > 0);
        assert!(flint_argon2_hash_params(pw.as_ptr(), i64::from(u32::MAX), 1, 1) > 0);
    }

    #[allow(dead_code)]
    fn _unused(_: Vec<u8>) -> u8 {
        0
    }
}
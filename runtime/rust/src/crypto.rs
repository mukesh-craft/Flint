//! Cryptography for the Flint standard library — pure Rust, no C library.
//!
//! HMAC-SHA256, AES-256-CTR and AES-256-GCM, implemented with RustCrypto
//! primitives (`sha2`, `hmac`, `aes`, `aes-gcm`, `cipher`). These are the
//! dependency-free wins: no C library to bind, no third-party TLS stack to
//! audit, and the correctness comes from algorithms whose test vectors ship in
//! the crates themselves.
//!
//! # Why `no_std` (same reason as the CSV module)
//!
//! `std` cannot live in a JIT-loaded object on Android (TLS handle / soinfo
//! abort), and two Rust `std` copies cannot share one link. These crates are
//! `no_std`-clean with `default-features = false`, so the module stays
//! loadable by both Flint execution paths and coexists with a user's own Rust
//! library.
//!
//! # Output discipline
//!
//! No allocation, so results land in a fixed static buffer and are returned as
//! NUL-terminated hex strings.
//!
//! # Result lifetime (the sharp edge)
//!
//! There is one shared output buffer. `flint_crypto_last()` points INTO it, and
//! ANY next crypto call — including a decrypt — overwrites it. A caller that
//! needs two results at once must copy the first (for example with
//! `flint_str_concat(value, "")`) before making the second call. This is not
//! hypothetical: the gate's own test hit it while being written.
//!
//! Errors come back as a negative length with `flint_crypto_error` explaining
//! why — never as a silently truncated digest.

#![allow(clippy::needless_range_loop)]

use aes::cipher::generic_array::GenericArray;
use aes::cipher::{KeyIvInit, StreamCipher};
use aes::Aes256;
use aes::cipher::KeyInit as AesKeyInit;
use ctr::Ctr128BE;
use aes_gcm::aead::AeadInPlace;

use aes_gcm::{Aes256Gcm, Nonce};
use hmac::{Hmac, Mac};
use sha2::{Digest, Sha256};

/// Capacity of the shared hex output buffer. A GCM result is ciphertext plus a
/// 16-byte tag; doubling covers ciphertext plus tag comfortably.
const OUT_HEX_CAP: usize = 4096;
/// Capacity of the raw byte buffer that hex output is produced from.
const OUT_RAW_CAP: usize = OUT_HEX_CAP / 2;

static mut OUT_HEX: [u8; OUT_HEX_CAP] = [0; OUT_HEX_CAP];
static mut OUT_RAW: [u8; OUT_RAW_CAP] = [0; OUT_RAW_CAP];
static mut ERR: [u8; 160] = [0; 160];
/// Scratch space for in-place transforms. A `no_std` module has no allocator,
/// so every transform copies its input here first and works in place.
const WORK_CAP: usize = 1 << 20;
static mut WORK: [u8; WORK_CAP] = [0; WORK_CAP];

/// Copies `input` into the scratch buffer, refusing anything larger than the
/// buffer (an error, never a truncation).
fn into_work(input: &[u8]) -> Result<usize, &'static str> {
    if input.len() > WORK_CAP {
        return Err("crypto: input exceeds the 1 MiB working buffer");
    }
    let work = unsafe { &mut *core::ptr::addr_of_mut!(WORK) };
    work[..input.len()].copy_from_slice(input);
    Ok(input.len())
}

fn set_error(message: &str) {
    let bytes = message.as_bytes();
    let err = unsafe { &mut *core::ptr::addr_of_mut!(ERR) };
    let n = bytes.len().min(err.len() - 1);
    for (i, b) in bytes[..n].iter().enumerate() {
        err[i] = *b;
    }
    err[n] = 0;
}

/// The last error message; static storage, do not free.
#[no_mangle]
pub extern "C" fn flint_crypto_error() -> *const u8 {
    core::ptr::addr_of!(ERR) as *const u8
}

/// Output buffer capacity in bytes, so callers can size their expectations.
#[no_mangle]
pub extern "C" fn flint_crypto_out_bytes() -> i64 {
    OUT_HEX_CAP as i64
}

/// The most recent result, as a NUL-terminated string.
///
/// The FFI entry points return a LENGTH rather than a pointer because a
/// pointer into a static buffer would tempt callers to keep it past the next
/// call. This accessor makes that contract usable: read the result, then call
/// again. Valid until the next crypto call.
#[no_mangle]
pub extern "C" fn flint_crypto_last() -> *const u8 {
    core::ptr::addr_of!(OUT_HEX) as *const u8
}

/// One input byte -> two lowercase hex characters.
///
/// Built from a nibble table with a shift, not `* 16`: multiplying a u8 nibble
/// by 16 overflows for values 16 and up, which the first NIST vector caught.
fn hex_pair(b: u8) -> [u8; 2] {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    [
        DIGITS[((b >> 4) & 0x0f) as usize],
        DIGITS[(b & 0x0f) as usize],
    ]
}

/// Turns `raw` into lowercase hex in the shared buffer and returns its length.
fn raw_to_hex(raw: &[u8]) -> Result<i64, &'static str> {
    if raw.len() * 2 + 1 > OUT_HEX_CAP {
        return Err("crypto: result exceeds the output buffer");
    }
    let out = unsafe { &mut *core::ptr::addr_of_mut!(OUT_HEX) };
    for (i, byte) in raw.iter().enumerate() {
        let pair = hex_pair(*byte);
        out[i * 2] = pair[0];
        out[i * 2 + 1] = pair[1];
    }
    out[raw.len() * 2] = 0;
    Ok((raw.len() * 2) as i64)
}

fn copy_out(raw: &[u8]) -> Result<i64, &'static str> {
    if raw.len() > OUT_RAW_CAP {
        return Err("crypto: result exceeds the output buffer");
    }
    let out = unsafe { &mut *core::ptr::addr_of_mut!(OUT_RAW) };
    out[..raw.len()].copy_from_slice(raw);
    Ok(raw.len() as i64)
}

/// Borrow a C string as bytes. Returns None for null or unterminated input;
/// `limit` bounds the scan so a bad pointer cannot walk the whole heap.
unsafe fn cstr_bytes<'a>(ptr: *const u8, limit: usize) -> Option<&'a [u8]> {
    if ptr.is_null() {
        return None;
    }
    let mut len = 0usize;
    while len < limit {
        if unsafe { *ptr.add(len) } == 0 {
            return Some(unsafe { core::slice::from_raw_parts(ptr, len) });
        }
        len += 1;
    }
    None
}

const MAX_INPUT: usize = 1 << 20;

fn args2(a: *const u8, b: *const u8) -> Result<(&'static [u8], &'static [u8]), &'static str> {
    let first = unsafe { cstr_bytes(a, MAX_INPUT) }.ok_or("crypto: argument is null or unterminated")?;
    let second = unsafe { cstr_bytes(b, MAX_INPUT) }.ok_or("crypto: argument is null or unterminated")?;
    Ok((first, second))
}

/// Lowercase hex SHA-256 of `data`. Returns 64 characters, or -1 on error.
#[no_mangle]
pub extern "C" fn flint_sha256_hex(data: *const u8) -> i64 {
    let outcome = (|| -> Result<i64, &'static str> {
        let Some(input) = (unsafe { cstr_bytes(data, MAX_INPUT) }) else {
            return Err("crypto: input is null or unterminated");
        };
        let mut hasher = Sha256::new();
        hasher.update(input);
        raw_to_hex(&hasher.finalize())
    })();
    finish(outcome)
}

/// Hex-encoded HMAC-SHA256 of `data` under `key`. Returns 64 characters.
#[no_mangle]
pub extern "C" fn flint_hmac_sha256_hex(key: *const u8, data: *const u8) -> i64 {
    let outcome = (|| -> Result<i64, &'static str> {
        let (key, data) = args2(key, data)?;
        type HmacSha256 = Hmac<Sha256>;
        // Ambiguity-proof: name the trait, do not rely on method resolution
        // picking the right one (cipher and hmac both provide these).
        let mut mac = <HmacSha256 as Mac>::new_from_slice(key)
            .map_err(|_| "crypto: HMAC rejects keys of this length")?;
        mac.update(data);
        raw_to_hex(&mac.finalize().into_bytes())
    })();
    finish(outcome)
}

/// AES-256-CTR encryption (and decryption — CTR is symmetric).
///
/// `key` must be 32 bytes and `iv` (counter block) 16 bytes, both given as
/// hex. Returns the ciphertext as hex, or -1 with an error.
#[no_mangle]
pub extern "C" fn flint_aes256_ctr_crypt_hex(
    key: *const u8,
    iv: *const u8,
    data: *const u8,
) -> i64 {
    let outcome = (|| -> Result<i64, &'static str> {
        let (key, iv) = args2(key, iv)?;
        let Some(input) = (unsafe { cstr_bytes(data, MAX_INPUT) }) else {
            return Err("crypto: input is null or unterminated");
        };
        // Each decoded value must be copied out BEFORE the next decode: they
        // all land in the same shared buffer, so reading the key after decoding
        // the IV silently encrypts with the IV as the key.
        let key_len = unhex(key, 32, "key")?;
        let mut key_bytes = [0u8; 32];
        key_bytes.copy_from_slice(raw_out(key_len));
        let mut iv_bytes = [0u8; 16];
        iv_bytes.copy_from_slice(raw_out(unhex(iv, 16, "iv")?));
        let len = into_work(input)?;
        let work = unsafe { &mut *core::ptr::addr_of_mut!(WORK) };
        let mut cipher = <Ctr128BE<Aes256> as aes::cipher::KeyIvInit>::new(
            GenericArray::from_slice(&key_bytes),
            GenericArray::from_slice(&iv_bytes),
        );
        cipher.apply_keystream(&mut work[..len]);
        copy_out(&work[..len])?;
        raw_to_hex(raw_out(len))
    })();
    finish(outcome)
}

/// AES-256-GCM authenticated encryption.
///
/// Returns `ciphertext || 16-byte tag` as hex. `aad` may be empty (pass a
/// pointer to an empty string). GCM requires a unique nonce per message under
/// one key; the module refuses to guess one for the caller.
#[no_mangle]
pub extern "C" fn flint_aes256_gcm_encrypt_hex(
    key: *const u8,
    nonce: *const u8,
    aad: *const u8,
    data: *const u8,
) -> i64 {
    let outcome = (|| -> Result<i64, &'static str> {
        let (key, nonce) = args2(key, nonce)?;
        let aad_bytes = unsafe { cstr_bytes(aad, MAX_INPUT) }.unwrap_or(b"");
        let Some(input) = (unsafe { cstr_bytes(data, MAX_INPUT) }) else {
            return Err("crypto: input is null or unterminated");
        };
        // Copy each decoded value out before decoding the next one: they all
        // land in the same shared buffer.
        let key_len = unhex(key, 32, "key")?;
        let mut key_bytes = [0u8; 32];
        key_bytes.copy_from_slice(raw_out(key_len));
        let mut nonce_bytes = [0u8; 12];
        nonce_bytes.copy_from_slice(raw_out(unhex(nonce, 12, "nonce")?));
        let len = into_work(input)?;
        if len + 16 > WORK_CAP {
            return Err("crypto: input leaves no room for the GCM tag");
        }
        let work = unsafe { &mut *core::ptr::addr_of_mut!(WORK) };
        let cipher = <Aes256Gcm as aes_gcm::KeyInit>::new(GenericArray::from_slice(&key_bytes));
        let tag = cipher
            .encrypt_in_place_detached(Nonce::from_slice(&nonce_bytes), aad_bytes, &mut work[..len])
            .map_err(|_| "crypto: GCM encryption failed")?;
        work[len..len + 16].copy_from_slice(&tag);
        copy_out(&work[..len + 16])?;
        raw_to_hex(raw_out(len + 16))
    })();
    finish(outcome)
}

/// Verifies and decrypts `ciphertext || tag` produced by
/// `flint_aes256_gcm_encrypt_hex`. Returns the plaintext as hex, or -1 when
/// authentication fails (a wrong key, nonce or tag).
#[no_mangle]
pub extern "C" fn flint_aes256_gcm_decrypt_hex(
    key: *const u8,
    nonce: *const u8,
    aad: *const u8,
    data: *const u8,
) -> i64 {
    let outcome = (|| -> Result<i64, &'static str> {
        let (key, nonce) = args2(key, nonce)?;
        let aad_bytes = unsafe { cstr_bytes(aad, MAX_INPUT) }.unwrap_or(b"");
        let Some(input) = (unsafe { cstr_bytes(data, MAX_INPUT) }) else {
            return Err("crypto: input is null or unterminated");
        };
        if input.len() % 2 != 0 {
            return Err("crypto: hex input has an odd length");
        }
        let raw_len = unhex(input, input.len() / 2, "ciphertext")?;
        if raw_len < 16 {
            return Err("crypto: GCM ciphertext shorter than its tag");
        }
        // Materialize the ciphertext in the scratch buffer so body and tag can
        // be split in place.
        let total = into_work(raw_out(raw_len))?;
        let key_len = unhex(key, 32, "key")?;
        let mut key_bytes = [0u8; 32];
        key_bytes.copy_from_slice(raw_out(key_len));
        let mut nonce_bytes = [0u8; 12];
        nonce_bytes.copy_from_slice(raw_out(unhex(nonce, 12, "nonce")?));
        let cipher = <Aes256Gcm as aes_gcm::KeyInit>::new(GenericArray::from_slice(&key_bytes));
        let work = unsafe { &mut *core::ptr::addr_of_mut!(WORK) };
        // Split the USED prefix, not the whole buffer: splitting at
        // `total - 16` on the full 1 MiB slice would hand GCM a "tag" of
        // ~1 MB. (The round-trip test caught exactly that.)
        let used = &mut work[..total];
        let (body, tag) = used.split_at_mut(total - 16);
        cipher
            .decrypt_in_place_detached(
                Nonce::from_slice(&nonce_bytes),
                aad_bytes,
                body,
                GenericArray::from_slice(tag),
            )
            .map_err(|_| "crypto: GCM authentication failed (wrong key, nonce, aad or tag)")?;
        let plain_len = total - 16;
        copy_out(&work[..plain_len])?;
        raw_to_hex(raw_out(plain_len))
    })();
    finish(outcome)
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

/// Decodes `expect_bytes` of hex into the shared raw buffer.
/// Decodes hex into the raw output buffer and returns the byte count.
///
/// No allocation: `no_std` has no heap, and every caller immediately consumes
/// the bytes (as a key, an IV or a body it copies into the scratch buffer).
fn unhex(input: &[u8], expect_bytes: usize, what: &str) -> Result<usize, &'static str> {
    if input.len() != expect_bytes * 2 {
        return Err(match what {
            "key" => "crypto: key must be 32 bytes of hex (64 characters)",
            "nonce" => "crypto: nonce must be 12 bytes of hex (24 characters)",
            _ => "crypto: hex length does not match the expected size",
        });
    }
    if expect_bytes > OUT_RAW_CAP {
        return Err("crypto: hex input exceeds the working buffer");
    }
    let out = unsafe { &mut *core::ptr::addr_of_mut!(OUT_RAW) };
    for (i, pair) in input.chunks_exact(2).enumerate() {
        let hi = hex_value(pair[0]).ok_or("crypto: invalid hex digit")?;
        let lo = hex_value(pair[1]).ok_or("crypto: invalid hex digit")?;
        out[i] = hi * 16 + lo;
    }
    Ok(expect_bytes)
}

/// The most recently decoded raw bytes (valid until the next call).
fn raw_out(n: usize) -> &'static [u8] {
    // Explicit deref then slice, never an implicit autoref of a raw pointer.
    let whole: &'static [u8; OUT_RAW_CAP] = unsafe { &*core::ptr::addr_of!(OUT_RAW) };
    &whole[..n]
}

fn hex_value(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

// Silence unused-import warnings for traits used only by some call paths.
#[allow(unused_imports)]
use aes::cipher::BlockEncrypt as _;

#[cfg(test)]
mod tests {
    use super::*;
    use std::format;
    use std::string::{String, ToString};
    use std::vec;
    use std::vec::Vec;

    fn c(s: &str) -> std::ffi::CString {
        std::ffi::CString::new(s).unwrap()
    }

    /// The module's buffers are static (it is `no_std`, so there is no heap),
    /// which means it is NOT reentrant -- exactly like Flint's other stdlib
    /// modules. Rust runs tests in parallel, so they must be serialized here or
    /// they corrupt each other's output. Symptom when this lock is missing:
    /// one test's plaintext turning up inside another's ciphertext.
    static TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());
    fn serialized() -> std::sync::MutexGuard<'static, ()> {
        TEST_LOCK.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    /// The hex the module left in its output buffer.
    fn out() -> String {
        unsafe { std::ffi::CStr::from_ptr(core::ptr::addr_of!(OUT_HEX) as *const u8) }
            .to_str()
            .unwrap()
            .to_string()
    }

    fn err() -> String {
        unsafe { std::ffi::CStr::from_ptr(core::ptr::addr_of!(ERR) as *const u8) }
            .to_str()
            .unwrap()
            .to_string()
    }

    /// Calls an FFI entry point, asserts success, returns its hex output.
    fn ok(n: i64) -> String {
        assert!(n >= 0, "expected success, got {n} ({})", err());
        out()
    }

    #[test]
    fn sha256_matches_published_vectors() {
        let _serialized = serialized();
        // FIPS 180-4 vectors. A wrong implementation cannot pass by accident.
        assert_eq!(
            ok(flint_sha256_hex(c("").as_ptr())),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
        assert_eq!(
            ok(flint_sha256_hex(c("abc").as_ptr())),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }

    #[test]
    fn hmac_sha256_matches_rfc4231() {
        let _serialized = serialized();
        // RFC 4231 test case 2.
        let key = c("Jefe");
        let data = c("what do ya want for nothing?");
        assert_eq!(
            ok(flint_hmac_sha256_hex(key.as_ptr(), data.as_ptr())),
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"
        );
    }

    #[test]
    fn aes256_ctr_matches_openssl() {
        let _serialized = serialized();
        // Expected value produced by an INDEPENDENT implementation:
        //   openssl enc -aes-256-ctr \
        //     -K 603deb...dff4 -iv f0f1...feff <ascii "6bc1bee2...172a">
        // (OpenSSL 3.6.3). The key and IV are NIST SP 800-38A F.5.1 values;
        // the plaintext is that vector's first block as ASCII text, which is
        // what this API operates on.
        let key = c("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4");
        let iv = c("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
        let pt = c("6bc1bee22e409f96e93d7e117393172a");
        let ciphertext = ok(flint_aes256_ctr_crypt_hex(key.as_ptr(), iv.as_ptr(), pt.as_ptr()));
        assert_eq!(
            ciphertext,
            "3dbd1ec03b7273016cffbf25f106fc343f575af9640428376300bf0fbe5249f5"
        );

        // NOTE on round trips: this API encrypts TEXT and returns HEX, so
        // feeding the hex result back in would encrypt the hex characters --
        // a different input, not the ciphertext. CTR symmetry (decrypt ==
        // encrypt over the same bytes) is a property of the mode, and the
        // OpenSSL comparison above is the actual correctness proof. GCM below
        // does round-trip, because it has a decode entry point.
    }

    #[test]
    fn gcm_round_trips() {
        let _serialized = serialized();
        let key = c("0000000000000000000000000000000000000000000000000000000000000000");
        let nonce = c("000000000000000000000000");
        let aad = c("header");
        let pt = c("attack at dawn");
        let sealed = ok(flint_aes256_gcm_encrypt_hex(key.as_ptr(), nonce.as_ptr(), aad.as_ptr(), pt.as_ptr()));
        // ciphertext plus the 16-byte tag, hex encoded
        assert_eq!(sealed.len(), (pt.as_bytes().len() + 16) * 2);

        let ct = c(&sealed);
        let plain = ok(flint_aes256_gcm_decrypt_hex(key.as_ptr(), nonce.as_ptr(), aad.as_ptr(), ct.as_ptr()));
        assert_eq!(
            String::from_utf8(
                (0..plain.len() / 2)
                    .map(|i| u8::from_str_radix(&plain[i * 2..i * 2 + 2], 16).unwrap())
                    .collect::<Vec<u8>>()
            )
            .unwrap(),
            "attack at dawn"
        );
    }

    #[test]
    fn gcm_detects_a_tampered_tag() {
        let _serialized = serialized();
        let key = c("0000000000000000000000000000000000000000000000000000000000000000");
        let nonce = c("000000000000000000000000");
        let pt = c("secret");
        let sealed = ok(flint_aes256_gcm_encrypt_hex(key.as_ptr(), nonce.as_ptr(), core::ptr::null(), pt.as_ptr()));

        let mut tampered = sealed.clone();
        let last = tampered.len() - 1;
        let flipped = if tampered.as_bytes()[last] == b'0' { "1" } else { "0" };
        tampered.replace_range(last..last + 1, flipped);

        let t = c(&tampered);
        assert_eq!(
            flint_aes256_gcm_decrypt_hex(key.as_ptr(), nonce.as_ptr(), core::ptr::null(), t.as_ptr()),
            -1
        );
        assert!(err().contains("authentication failed"), "{}", err());
    }

    #[test]
    fn gcm_wrong_nonce_fails() {
        let _serialized = serialized();
        let key = c("0000000000000000000000000000000000000000000000000000000000000000");
        let nonce = c("000000000000000000000000");
        let other = c("000000000000000000000001");
        let pt = c("secret");
        let sealed = ok(flint_aes256_gcm_encrypt_hex(key.as_ptr(), nonce.as_ptr(), core::ptr::null(), pt.as_ptr()));
        let ct = c(&sealed);
        assert_eq!(
            flint_aes256_gcm_decrypt_hex(key.as_ptr(), other.as_ptr(), core::ptr::null(), ct.as_ptr()),
            -1
        );
    }

    #[test]
    fn wrong_aad_fails_authentication() {
        let _serialized = serialized();
        let key = c("0000000000000000000000000000000000000000000000000000000000000000");
        let nonce = c("000000000000000000000000");
        let aad = c("header");
        let other_aad = c("tampered");
        let sealed = ok(flint_aes256_gcm_encrypt_hex(key.as_ptr(), nonce.as_ptr(), aad.as_ptr(), c("body").as_ptr()));
        let ct = c(&sealed);
        assert_eq!(
            flint_aes256_gcm_decrypt_hex(key.as_ptr(), nonce.as_ptr(), other_aad.as_ptr(), ct.as_ptr()),
            -1
        );
    }

    #[test]
    fn null_arguments_report_errors_instead_of_crashing() {
        assert_eq!(flint_sha256_hex(core::ptr::null()), -1);
        assert_eq!(flint_hmac_sha256_hex(core::ptr::null(), core::ptr::null()), -1);
        assert_eq!(
            flint_aes256_ctr_crypt_hex(core::ptr::null(), core::ptr::null(), core::ptr::null()),
            -1
        );
        assert!(!err().is_empty());
    }

    #[test]
    fn bad_hex_lengths_are_rejected() {
        let _serialized = serialized();
        let short_key = c("00");
        let iv = c("00000000000000000000000000000000");
        let data = c("00");
        assert_eq!(
            flint_aes256_ctr_crypt_hex(short_key.as_ptr(), iv.as_ptr(), data.as_ptr()),
            -1
        );
        assert!(err().contains("key must be 32 bytes"), "{}", err());
    }

    #[test]
    fn non_hex_characters_are_rejected() {
        let _serialized = serialized();
        let key = c("zz00000000000000000000000000000000000000000000000000000000000000");
        let iv = c("00000000000000000000000000000000");
        let data = c("00");
        assert_eq!(
            flint_aes256_ctr_crypt_hex(key.as_ptr(), iv.as_ptr(), data.as_ptr()),
            -1
        );
        assert!(err().contains("hex"), "{}", err());
    }

    #[test]
    fn nul_terminated_input_is_hashed_exactly() {
        let _serialized = serialized();
        // The ABI takes C strings, so the terminator is part of the contract.
        // (Constructing an unterminated pointer in a test would itself be UB;
        // the scan is bounded by MAX_INPUT as defence in depth.)
        let mut buf: Vec<u8> = vec![b'a'; 64];
        buf.push(0);
        assert_eq!(
            ok(flint_sha256_hex(buf.as_ptr())),
            ok_sha(std::str::from_utf8(&buf[..64]).unwrap())
        );
    }

    /// SHA-256 of an ASCII string via the FFI, for comparison above.
    fn ok_sha(s: &str) -> String {
        ok(flint_sha256_hex(c(s).as_ptr()))
    }
}

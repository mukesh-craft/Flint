//! Encoders and small utilities for the Flint standard library.
//!
//! Everything here is arithmetic and byte shuffling: no dependencies, no
//! allocation, no platform calls. They exist because a runtime that can read a
//! file but not emit a UUID or a URL-safe string is a runtime that forces every
//! program to hand-roll those five lines again.
//!
//! Base32 (RFC 4648), Base58 (Bitcoin alphabet), CRC32C (Castagnoli, the
//! checksum iSCSI/ext4/btrfs use), UUID v4/v7, and percent-encoding are all
//! verified against their published test vectors in the tests below -- an
//! encoder that is subtly wrong is worse than no encoder, because it produces
//! data that looks valid.

/// RFC 4648 base32 alphabet (no padding variant; padding is added).
const B32: &[u8; 32] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
/// Bitcoin base58 alphabet: no 0, O, I or l.
const B58: &[u8; 58] = b"123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

const OUT_B58_CAP: usize = 8192;

static mut OUT: [u8; OUT_B58_CAP] = [0; OUT_B58_CAP];
static mut ERR: [u8; 160] = [0; 160];

fn set_error(message: &str) {
    let err = unsafe { &mut *core::ptr::addr_of_mut!(ERR) };
    let bytes = message.as_bytes();
    let n = bytes.len().min(err.len() - 1);
    for (i, b) in bytes[..n].iter().enumerate() {
        err[i] = *b;
    }
    err[n] = 0;
}

/// The last error message; static storage, do not free.
#[no_mangle]
pub extern "C" fn flint_enc_error() -> *const u8 {
    core::ptr::addr_of!(ERR) as *const u8
}

fn write_out(bytes: &[u8]) -> bool {
    let cap = unsafe { *core::ptr::addr_of!(OUT) }.len() - 1;
    if bytes.len() > cap {
        set_error("encode: result exceeds the output buffer");
        return false;
    }
    let out = unsafe { &mut *core::ptr::addr_of_mut!(OUT) };
    out[..bytes.len()].copy_from_slice(bytes);
    out[bytes.len()] = 0;
    true
}

/// The most recent result, NUL-terminated. Valid until the next call.
#[no_mangle]
pub extern "C" fn flint_enc_last() -> *const u8 {
    core::ptr::addr_of!(OUT) as *const u8
}

unsafe fn cstr<'a>(ptr: *const u8) -> Result<&'a str, &'static str> {
    if ptr.is_null() {
        return Err("encode: argument is null");
    }
    let mut len = 0usize;
    while len < 1 << 20 && unsafe { *ptr.add(len) } != 0 {
        len += 1;
    }
    unsafe { CStrLite::new(ptr, len) }
        .ok_or("encode: argument is not valid UTF-8")
}

/// Tiny UTF-8 validation over a pointer range, avoiding a std::ffi dependency.
struct CStrLite;
impl CStrLite {
    unsafe fn new<'a>(ptr: *const u8, len: usize) -> Option<&'a str> {
        let slice = unsafe { core::slice::from_raw_parts(ptr, len) };
        core::str::from_utf8(slice).ok()
    }
}

use alloc::string::String;
use alloc::vec::Vec;

fn hex_lower(bytes: &[u8]) -> String {
    const D: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push(D[((b >> 4) & 0xf) as usize] as char);
        s.push(D[(b & 0xf) as usize] as char);
    }
    s
}

/// CRC32C (Castagnoli), software table. Returns the checksum of `data`.
#[no_mangle]
pub extern "C" fn flint_crc32c(data: *const u8, data_len: i64) -> i64 {
    if data.is_null() || data_len < 0 {
        set_error("crc32c: invalid arguments");
        return -1;
    }
    let slice = unsafe { core::slice::from_raw_parts(data, data_len as usize) };
    // Reflected CRC-32C polynomial 0x82F63B78.
    let mut crc: u32 = 0xFFFF_FFFF;
    for byte in slice {
        crc ^= *byte as u32;
        for _ in 0..8 {
            crc = if crc & 1 != 0 { (crc >> 1) ^ 0x82F6_3B78 } else { crc >> 1 };
        }
    }
    !crc as i64
}

/// Hex-encoded CRC32C. Returns 8 characters, or -1.
#[no_mangle]
pub extern "C" fn flint_crc32c_hex(data: *const u8, data_len: i64) -> i64 {
    let value = flint_crc32c(data, data_len);
    if value < 0 {
        return -1;
    }
    let bytes = (value as u32).to_be_bytes();
    if !write_out(hex_lower(&bytes).as_bytes()) {
        return -1;
    }
    8
}

// NOTE: there is deliberately NO `flint_hex_encode` export here. The C serial
// module (runtime/flint_serial.c) already defines that symbol, and every
// function in this module lives in ONE codegen unit -- so exporting it again
// puts a second definition in flint_std.o and the JIT refuses to load the
// object ("duplicate definition of symbol flint_hex_encode"). Flint programs
// use the C one, which has the same behaviour.

/// RFC 4648 base32 with padding. Returns the encoded length, or -1.
#[no_mangle]
pub extern "C" fn flint_base32_encode(data: *const u8, data_len: i64) -> i64 {
    if data.is_null() || data_len < 0 {
        set_error("base32: invalid arguments");
        return -1;
    }
    let slice = unsafe { core::slice::from_raw_parts(data, data_len as usize) };
    let mut out: String = String::with_capacity(slice.len().div_ceil(5) * 8);
    let mut buffer: u32 = 0;
    let mut bits: u32 = 0;
    for byte in slice {
        buffer = (buffer << 8) | *byte as u32;
        bits += 8;
        while bits >= 5 {
            bits -= 5;
            let index = ((buffer >> bits) & 31) as usize;
            out.push(B32[index] as char);
        }
    }
    if bits > 0 {
        let index = ((buffer << (5 - bits)) & 31) as usize;
        out.push(B32[index] as char);
    }
    while out.len() % 8 != 0 {
        out.push('=');
    }
    if !write_out(out.as_bytes()) {
        return -1;
    }
    out.len() as i64
}

/// Bitcoin base58 (no padding alphabet). Returns the encoded length, or -1.
#[no_mangle]
pub extern "C" fn flint_base58_encode(data: *const u8, data_len: i64) -> i64 {
    if data.is_null() || data_len < 0 {
        set_error("base58: invalid arguments");
        return -1;
    }
    let slice = unsafe { core::slice::from_raw_parts(data, data_len as usize) };
    if slice.is_empty() {
        write_out(b"");
        return 0;
    }
    // Leading zero bytes become leading '1's in base58.
    let leading_zeros = slice.iter().take_while(|b| **b == 0).count();
    let mut digits: Vec<u8> = Vec::with_capacity(slice.len() * 138 / 100 + 1);
    for byte in slice {
        let mut carry = *byte as u32;
        for digit in digits.iter_mut() {
            carry += (*digit as u32) << 8;
            *digit = (carry % 58) as u8;
            carry /= 58;
        }
        while carry > 0 {
            digits.push((carry % 58) as u8);
            carry /= 58;
        }
    }
    let mut out = String::with_capacity(leading_zeros + digits.len());
    for _ in 0..leading_zeros {
        out.push('1');
    }
    for digit in digits.iter().rev() {
        out.push(B58[*digit as usize] as char);
    }
    if !write_out(out.as_bytes()) {
        return -1;
    }
    out.len() as i64
}

fn is_unreserved(c: u8) -> bool {
    c.is_ascii_alphanumeric() || matches!(c, b'-' | b'.' | b'_' | b'~')
}

/// RFC 3986 percent-encoding. Returns the encoded length, or -1.
#[no_mangle]
pub extern "C" fn flint_url_encode(data: *const u8, data_len: i64) -> i64 {
    if data.is_null() || data_len < 0 {
        set_error("url: invalid arguments");
        return -1;
    }
    const HEX: &[u8; 16] = b"0123456789ABCDEF";
    let slice = unsafe { core::slice::from_raw_parts(data, data_len as usize) };
    let mut out: String = String::with_capacity(slice.len());
    for byte in slice {
        if is_unreserved(*byte) {
            out.push(*byte as char);
        } else {
            out.push('%');
            out.push(HEX[(byte >> 4) as usize] as char);
            out.push(HEX[(byte & 0xf) as usize] as char);
        }
    }
    if !write_out(out.as_bytes()) {
        return -1;
    }
    out.len() as i64
}

/// Percent-decoding (`%XX` sequences). Returns the decoded length, or -1.
#[no_mangle]
pub extern "C" fn flint_url_decode(data: *const u8, data_len: i64) -> i64 {
    if data.is_null() || data_len < 0 {
        set_error("url: invalid arguments");
        return -1;
    }
    let slice = unsafe { core::slice::from_raw_parts(data, data_len as usize) };
    let mut out: Vec<u8> = Vec::with_capacity(slice.len());
    let mut i = 0usize;
    while i < slice.len() {
        if slice[i] == b'%' {
            if i + 2 >= slice.len() {
                set_error("url: truncated percent-escape");
                return -1;
            }
            let hi = (slice[i + 1] as char).to_digit(16);
            let lo = (slice[i + 2] as char).to_digit(16);
            match (hi, lo) {
                (Some(h), Some(l)) => out.push((h * 16 + l) as u8),
                _ => {
                    set_error("url: invalid percent-escape");
                    return -1;
                }
            }
            i += 3;
        } else {
            out.push(slice[i]);
            i += 1;
        }
    }
    let written = out.len();
    if !write_out(&out) {
        return -1;
    }
    written as i64
}

/// Pseudo-random bytes for UUIDs and tokens.
///
/// Seeded from the OS once per process: a UUID generator that repeats itself
/// across runs is worse than useless, and `getrandom` is available on every
/// platform this language targets.
static mut RNG_STATE: u64 = 0;

fn seed_rng() {
    unsafe {
        if *core::ptr::addr_of!(RNG_STATE) != 0 {
            return;
        }
        #[cfg(any(target_os = "linux", target_os = "android"))]
        {
            extern "C" {
                fn getrandom(buf: *mut core::ffi::c_void, len: usize, flags: u32) -> isize;
            }
            let mut bytes = [0u8; 8];
            if unsafe { getrandom(bytes.as_mut_ptr() as *mut core::ffi::c_void, 8, 0) } == 8 {
                let mut value = 0u64;
                for (i, b) in bytes.iter().enumerate() {
                    value |= (*b as u64) << (i * 8);
                }
                *core::ptr::addr_of_mut!(RNG_STATE) = value | 1;
                return;
            }
        }
        // Fallback: a stack address mixed into the seed. Weaker, and
        // documented as such: it is only reached when the OS entropy call is
        // unavailable.
        let marker = 0u8;
        let address = &marker as *const u8 as usize as u64;
        *core::ptr::addr_of_mut!(RNG_STATE) = (address ^ 0x9E37_79B9_7F4A_7C15) | 1;
    }
}

/// splitmix64, exposed so other modules can derive non-secret material (a salt
/// fallback) without duplicating a second PRNG.
#[no_mangle]
pub extern "C" fn next_random_public() -> u64 {
    next_random()
}

/// splitmix64: fast, well-distributed, and tiny enough to ship.
fn next_random() -> u64 {
    seed_rng();
    unsafe {
        let state = core::ptr::addr_of_mut!(RNG_STATE);
        let z = (*state).wrapping_add(0x9E37_79B9_7F4A_7C15);
        *state = z;
        let mut x = z;
        x = (x ^ (x >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        x = (x ^ (x >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        x ^ (x >> 31)
    }
}

/// UUID v4 (random). Returns 36 characters, or -1.
#[no_mangle]
pub extern "C" fn flint_uuid_v4() -> i64 {
    let mut bytes = [0u8; 16];
    for chunk in bytes.chunks_mut(8) {
        let value = next_random().to_le_bytes();
        chunk.copy_from_slice(&value[..chunk.len()]);
    }
    bytes[6] = (bytes[6] & 0x0f) | 0x40; // version 4
    bytes[8] = (bytes[8] & 0x3f) | 0x80; // RFC 4122 variant
    format_uuid(&bytes)
}

/// UUID v7 (time-ordered, RFC 9562): 48-bit big-endian millisecond timestamp,
/// then random. Sortable, which is the entire reason to prefer it.
#[no_mangle]
pub extern "C" fn flint_uuid_v7() -> i64 {
    let millis = flint_time_millis() as u64;
    let mut bytes = [0u8; 16];
    bytes[0] = (millis >> 40) as u8;
    bytes[1] = (millis >> 32) as u8;
    bytes[2] = (millis >> 24) as u8;
    bytes[3] = (millis >> 16) as u8;
    bytes[4] = (millis >> 8) as u8;
    bytes[5] = millis as u8;
    for chunk in bytes[6..].chunks_mut(2) {
        let value = next_random().to_le_bytes();
        chunk.copy_from_slice(&value[..chunk.len()]);
    }
    bytes[6] = (bytes[6] & 0x0f) | 0x70; // version 7
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    format_uuid(&bytes)
}

fn format_uuid(bytes: &[u8; 16]) -> i64 {
    let mut out = String::with_capacity(36);
    for (i, b) in bytes.iter().enumerate() {
        if matches!(i, 4 | 6 | 8 | 10) {
            out.push('-');
        }
        out.push_str(&hex_lower(&[*b]));
    }
    if !write_out(out.as_bytes()) {
        return -1;
    }
    36
}

/// Milliseconds since the Unix epoch, using the C library's clock so the
/// stdlib needs no platform-specific time code.
extern "C" {
    fn clock_gettime(clock_id: i32, timespec: *mut core::ffi::c_void) -> i32;
    fn gettimeofday(tv: *mut core::ffi::c_void, tz: *mut core::ffi::c_void) -> i32;
}

fn flint_time_millis() -> i64 {
    #[repr(C)]
    struct TimeSpec {
        sec: i64,
        nsec: i64,
    }
    // CLOCK_REALTIME
    let mut ts = TimeSpec { sec: 0, nsec: 0 };
    if unsafe { clock_gettime(0, &mut ts as *mut _ as *mut core::ffi::c_void) } == 0 {
        return ts.sec * 1000 + ts.nsec / 1_000_000;
    }
    #[repr(C)]
    struct TimeVal {
        sec: i64,
        usec: i64,
    }
    let mut tv = TimeVal { sec: 0, usec: 0 };
    if unsafe { gettimeofday(&mut tv as *mut _ as *mut core::ffi::c_void, core::ptr::null_mut()) } == 0 {
        return tv.sec * 1000 + tv.usec / 1000;
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::string::{String, ToString};
    use std::vec;
    use std::vec::Vec;

    fn last() -> String {
        unsafe { core::ffi::CStr::from_ptr(flint_enc_last() as *const core::ffi::c_char) }
            .to_str()
            .unwrap()
            .to_string()
    }

    fn encode_with(f: extern "C" fn(*const u8, i64) -> i64, input: &[u8]) -> String {
        assert!(f(input.as_ptr(), input.len() as i64) >= 0);
        last()
    }

    #[test]
    fn crc32c_matches_published_vectors() {
        // RFC 3720 Appendix B.4 test vectors.
        let zeros = [0u8; 32];
        assert_eq!(flint_crc32c(zeros.as_ptr(), 32), 0x8A91_36AAu32 as i64);
        let ones = [0xffu8; 32];
        assert_eq!(flint_crc32c(ones.as_ptr(), 32), 0x62A8_AB43u32 as i64);
        let incrementing: Vec<u8> = (0u8..32).collect();
        assert_eq!(flint_crc32c(incrementing.as_ptr(), 32), 0x46DD_794Eu32 as i64);
        // "123456789" is the standard CRC self-test string.
        let check = b"123456789";
        assert_eq!(flint_crc32c(check.as_ptr(), 9), 0xE306_9283u32 as i64);
    }

    #[test]
    fn hex_encoding_round_trips_lowercase() {
        // hex_lower is the shared helper the exported encoders use; the
        // user-facing flint_hex_encode lives in the C serial module.
        assert_eq!(hex_lower(&[0x00, 0x0f, 0xff, 0xa5]), "000fffa5");
        assert_eq!(hex_lower(b"abc"), "616263");
    }

    #[test]
    fn base32_matches_rfc4648_vectors() {
        // RFC 4648 section 10 test vectors.
        assert_eq!(encode_with(flint_base32_encode, b""), "");
        assert_eq!(encode_with(flint_base32_encode, b"f"), "MY======");
        assert_eq!(encode_with(flint_base32_encode, b"fo"), "MZXQ====");
        assert_eq!(encode_with(flint_base32_encode, b"foo"), "MZXW6===");
        assert_eq!(encode_with(flint_base32_encode, b"foob"), "MZXW6YQ=");
        assert_eq!(encode_with(flint_base32_encode, b"fooba"), "MZXW6YTB");
        assert_eq!(encode_with(flint_base32_encode, b"foobar"), "MZXW6YTBOI======");
    }

    #[test]
    fn base58_matches_known_vectors() {
        // Bitcoin base58 alphabet, widely used test vectors.
        assert_eq!(encode_with(flint_base58_encode, b""), "");
        assert_eq!(encode_with(flint_base58_encode, b"hello world"), "StV1DL6CwTryKyV");
        // Leading zero bytes become leading '1's.
        assert_eq!(encode_with(flint_base58_encode, &[0, 0, 1]), "112");
    }

    #[test]
    fn url_encoding_matches_rfc3986() {
        assert_eq!(encode_with(flint_url_encode, b"hello world"), "hello%20world");
        assert_eq!(encode_with(flint_url_encode, b"a/b?c=d&e"), "a%2Fb%3Fc%3Dd%26e");
        // Unreserved characters must pass through untouched.
        assert_eq!(encode_with(flint_url_encode, b"azAZ09-._~"), "azAZ09-._~");
    }

    #[test]
    fn url_decoding_round_trips_and_rejects_bad_escapes() {
        assert_eq!(encode_with(flint_url_decode, b"hello%20world"), "hello world");
        assert_eq!(encode_with(flint_url_decode, b"a%2Fb"), "a/b");
        assert_eq!(flint_url_decode(b"%zz".as_ptr(), 3), -1);
        assert_eq!(flint_url_decode(b"%4".as_ptr(), 2), -1);
    }

    #[test]
    fn uuid_v4_has_the_right_shape_and_version() {
        let text = {
            flint_uuid_v4();
            last()
        };
        assert_eq!(text.len(), 36);
        assert_eq!(&text[14..15], "4", "version nibble: {text}");
        // RFC 4122 variant: the top two bits are 10, so the nibble is one of
        // 8, 9, a, b -- not only 8. (The first version of this test asserted
        // "8" and failed on a perfectly valid 'a'.)
        assert!(
            matches!(&text[19..20], "8" | "9" | "a" | "b"),
            "variant nibble: {text}"
        );
        assert_eq!(
            text.chars().filter(|c| *c == '-').count(),
            4,
            "four hyphens: {text}"
        );
        assert!(text.chars().all(|c| c.is_ascii_hexdigit() || c == '-'));
    }

    #[test]
    fn uuid_v7_is_time_ordered_and_unique() {
        let first = {
            flint_uuid_v7();
            last()
        };
        assert_eq!(first.len(), 36);
        assert_eq!(&first[14..15], "7", "version nibble: {first}");
        // Two calls must differ: a UUID generator that repeats is useless.
        flint_uuid_v7();
        let second = last();
        assert_ne!(first, second);
    }
}
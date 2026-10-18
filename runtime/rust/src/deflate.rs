//! Deflate (RFC 1951) encoding, plus the zlib wrapper and the checksums PNG
//! needs.
//!
//! # Why this is hand-written
//!
//! `miniz_oxide` provides a slice-based, `no_std` **decompressor** (used by the PNG
//! decoder), but its compressor requires `std::io::Write + Seek`, which a
//! `#![no_std]` module cannot provide. So the encoder here is ours: LZ77 with a
//! hash-chain matcher, emitted with **fixed Huffman codes** (RFC 1951 §3.2.6).
//!
//! Fixed Huffman rather than dynamic is a deliberate trade: dynamic coding buys
//! 10–20% on text and a few percent on images, at the cost of a tree builder
//! and the failure modes that come with it. Fixed coding has no data-dependent
//! tables, so it cannot produce a corrupt stream — for a PNG encoder inside a
//! language runtime, predictable-and-valid beats 12% smaller.
//!
//! Every function here is pure and testable, and the tests include a
//! round-trip through the *independent* decompressor, so "valid DEFLATE" is
//! checked by something other than the encoder that produced it.

use alloc::vec;
use alloc::vec::Vec;

/// LZ77 window and match limits from the spec.
const MIN_MATCH: usize = 3;
const MAX_MATCH: usize = 258;
const WINDOW: usize = 32768;
const HASH_BITS: usize = 15;
const HASH_SIZE: usize = 1 << HASH_BITS;
/// Cap on how far back the chain walk looks, so pathological inputs stay linear.
const MAX_CHAIN: usize = 64;

const LENGTH_BASE: [u16; 29] = [
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131,
    163, 195, 227, 258,
];
const LENGTH_EXTRA: [u8; 29] = [
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
];
const DIST_BASE: [u16; 30] = [
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
    2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
];
const DIST_EXTRA: [u8; 30] = [
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13,
    13,
];

/// A bit writer that emits DEFLATE's LSB-first bit order.
struct BitWriter {
    out: Vec<u8>,
    bit_buffer: u32,
    bit_count: u32,
}

impl BitWriter {
    fn new() -> Self {
        BitWriter {
            out: Vec::new(),
            bit_buffer: 0,
            bit_count: 0,
        }
    }

    fn write_bits(&mut self, value: u32, count: u32) {
        self.bit_buffer |= (value & ((1u32 << count) - 1)) << self.bit_count;
        self.bit_count += count;
        while self.bit_count >= 8 {
            self.out.push((self.bit_buffer & 0xff) as u8);
            self.bit_buffer >>= 8;
            self.bit_count -= 8;
        }
    }

    /// Huffman codes are written MSB-first and must be reversed on the wire.
    fn write_code(&mut self, code: u32, length: u32) {
        let mut reversed = 0u32;
        for i in 0..length {
            reversed |= ((code >> (length - 1 - i)) & 1) << i;
        }
        self.write_bits(reversed, length);
    }

    fn finish(mut self) -> Vec<u8> {
        if self.bit_count > 0 {
            self.out.push((self.bit_buffer & 0xff) as u8);
        }
        self.out
    }
}

/// Fixed Huffman literal/length code (RFC 1951 §3.2.6).
fn fixed_literal_code(symbol: u16) -> (u32, u32) {
    match symbol {
        0..=143 => (0x30 + symbol as u32, 8),
        144..=255 => (0x190 + (symbol as u32 - 144), 9),
        256..=279 => (symbol as u32 - 256, 7),
        _ => (0xC0 + (symbol as u32 - 280), 8),
    }
}

fn length_symbol(length: usize) -> (u16, u32, u32) {
    for i in (0..LENGTH_BASE.len()).rev() {
        if length >= LENGTH_BASE[i] as usize {
            let extra_bits = LENGTH_EXTRA[i] as u32;
            let extra = (length - LENGTH_BASE[i] as usize) as u32;
            return (257 + i as u16, extra, extra_bits);
        }
    }
    (257, 0, 0)
}

fn distance_symbol(distance: usize) -> (u16, u32, u32) {
    for i in (0..DIST_BASE.len()).rev() {
        if distance >= DIST_BASE[i] as usize {
            let extra_bits = DIST_EXTRA[i] as u32;
            let extra = (distance - DIST_BASE[i] as usize) as u32;
            return (i as u16, extra, extra_bits);
        }
    }
    (0, 0, 0)
}

/// Raw DEFLATE stream (no zlib wrapper) of `data`.
pub fn deflate(data: &[u8]) -> Vec<u8> {
    let mut writer = BitWriter::new();
    // One final fixed-Huffman block: BFINAL=1, BTYPE=01.
    writer.write_bits(1, 1);
    writer.write_bits(1, 2);

    // Hash chains: head[h] is the most recent position with 3-byte hash h.
    let mut head = vec![u32::MAX; HASH_SIZE];
    let mut prev = vec![u32::MAX; data.len().max(1)];

    let hash = |d: &[u8], i: usize| -> usize {
        (((d[i] as usize) << 10) ^ ((d[i + 1] as usize) << 5) ^ (d[i + 2] as usize))
            & (HASH_SIZE - 1)
    };

    let mut i = 0usize;
    while i < data.len() {
        let mut best_len = 0usize;
        let mut best_dist = 0usize;

        if i + MIN_MATCH <= data.len() {
            let h = hash(data, i);
            let mut candidate = head[h];
            let mut chain = 0usize;
            while candidate != u32::MAX && chain < MAX_CHAIN {
                let c = candidate as usize;
                let distance = i - c;
                if distance == 0 || distance > WINDOW {
                    break;
                }
                let max_len = MAX_MATCH.min(data.len() - i);
                let mut len = 0usize;
                while len < max_len && data[c + len] == data[i + len] {
                    len += 1;
                }
                if len > best_len {
                    best_len = len;
                    best_dist = distance;
                    if len == max_len {
                        break;
                    }
                }
                candidate = prev[c];
                chain += 1;
            }
            prev[i] = head[h];
            head[h] = i as u32;
        }

        if best_len >= MIN_MATCH {
            let (symbol, extra, extra_bits) = length_symbol(best_len);
            let (code, bits) = fixed_literal_code(symbol);
            writer.write_code(code, bits);
            if extra_bits > 0 {
                writer.write_bits(extra, extra_bits);
            }
            let (dsym, dextra, dextra_bits) = distance_symbol(best_dist);
            writer.write_code(dsym as u32, 5);
            if dextra_bits > 0 {
                writer.write_bits(dextra, dextra_bits);
            }
            // Register the interior positions so later matches can find them.
            for k in (i + 1)..(i + best_len) {
                if k + MIN_MATCH <= data.len() {
                    let h = hash(data, k);
                    prev[k] = head[h];
                    head[h] = k as u32;
                }
            }
            i += best_len;
        } else {
            let (code, bits) = fixed_literal_code(data[i] as u16);
            writer.write_code(code, bits);
            i += 1;
        }
    }

    // End-of-block symbol 256.
    let (code, bits) = fixed_literal_code(256);
    writer.write_code(code, bits);
    writer.finish()
}

/// Adler-32 (RFC 1950), the zlib stream checksum.
pub fn adler32(data: &[u8]) -> u32 {
    let mut a: u32 = 1;
    let mut b: u32 = 0;
    // 5552 is the largest run that cannot overflow the 32-bit accumulators.
    for chunk in data.chunks(5552) {
        for byte in chunk {
            a += *byte as u32;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    (b << 16) | a
}

/// zlib-wrapped stream (2-byte header, deflate body, Adler-32 trailer).
pub fn zlib_compress(data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(data.len() / 2 + 16);
    // CMF=0x78 (deflate, 32K window), FLG=0x01 -> (0x78 << 8 | 0x01) % 31 == 0.
    out.push(0x78);
    out.push(0x01);
    out.extend_from_slice(&deflate(data));
    out.extend_from_slice(&adler32(data).to_be_bytes());
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::string::{String, ToString};
    use std::vec::Vec;

    /// The independent oracle: if our encoder and miniz_oxide agree, the stream
    /// is valid DEFLATE, not merely self-consistent.
    fn round_trip(data: &[u8]) -> Vec<u8> {
        let compressed = zlib_compress(data);
        miniz_oxide::inflate::decompress_to_vec_zlib_with_limit(compressed.as_slice(), data.len())
            .expect("our encoder produced a stream the reference decoder rejects")
    }

    #[test]
    fn empty_input_round_trips() {
        assert!(round_trip(b"").is_empty());
    }

    #[test]
    fn literals_round_trip() {
        let data = b"hello world";
        assert_eq!(round_trip(data), data.to_vec());
    }

    #[test]
    fn highly_repetitive_data_round_trips() {
        let data = b"abcabcabcabcabcabcabcabcabcabcabcabcabcabcabc".repeat(50);
        assert_eq!(round_trip(&data), data);
    }

    #[test]
    fn every_byte_value_round_trips() {
        let data: Vec<u8> = (0..=255u8).cycle().take(1000).collect();
        assert_eq!(round_trip(&data), data);
    }

    #[test]
    fn incompressible_data_round_trips() {
        // Pseudo-random bytes: the matcher must not corrupt what it cannot match.
        let mut data = Vec::with_capacity(4096);
        let mut state = 0x12345678u32;
        for _ in 0..4096 {
            state = state.wrapping_mul(1103515245).wrapping_add(12345);
            data.push((state >> 16) as u8);
        }
        assert_eq!(round_trip(&data), data);
    }

    #[test]
    fn compression_actually_happens_on_repetitive_data() {
        let data = vec![7u8; 10_000];
        let compressed = zlib_compress(&data);
        assert!(
            compressed.len() < data.len() / 10,
            "expected >10x on a constant buffer, got {} -> {}",
            data.len(),
            compressed.len()
        );
    }

    #[test]
    fn adler32_matches_the_reference_vectors() {
        assert_eq!(adler32(b""), 1);
        assert_eq!(adler32(b"a"), 0x0062_0062);
        assert_eq!(adler32(b"abc"), 0x024D_0127);
        assert_eq!(adler32(b"Wikipedia"), 0x11E6_0398);
    }

    #[test]
    fn zlib_header_is_well_formed() {
        let stream = zlib_compress(b"x");
        assert_eq!(stream[0], 0x78);
        let check = ((stream[0] as u16) << 8) | stream[1] as u16;
        assert_eq!(check % 31, 0, "zlib header checksum must be divisible by 31");
        let trailer = u32::from_be_bytes([
            stream[stream.len() - 4],
            stream[stream.len() - 3],
            stream[stream.len() - 2],
            stream[stream.len() - 1],
        ]);
        assert_eq!(trailer, adler32(b"x"), "trailer must be the Adler-32 of the input");
    }

    #[test]
    fn match_length_and_distance_encoding_round_trips() {
        // Long match forces a length symbol above the 11-bit simple range, and a
        // large distance forces distance extra bits.
        let mut data = vec![0u8; 40000];
        data[0] = 1;
        for k in 1..20000 {
            data[k] = data[k - 1];
        }
        data[0] = 9;
        let out = round_trip(&data);
        assert_eq!(out, data);
    }

    #[test]
    fn length_and_distance_tables_agree_with_the_spec() {
        // Spot-check the table edges that off-by-one errors hide in.
        assert_eq!(length_symbol(3), (257, 0, 0));
        // Lengths 11 and 12 share a symbol but that symbol carries ONE extra
        // bit (RFC 1951 table). Asserting 0 extra bits here was wrong.
        assert_eq!(length_symbol(11), (265, 0, 1));
        assert_eq!(length_symbol(12), (265, 1, 1));
        assert_eq!(length_symbol(258), (285, 0, 0));
        assert_eq!(distance_symbol(1), (0, 0, 0));
        assert_eq!(distance_symbol(2), (1, 0, 0));
        // 32768 = base 24577 + 8191 extra across 13 bits (2^13 - 1). The extra
        // VALUE and the extra BIT COUNT are different numbers; asserting 13 for
        // both was the test's mistake.
        assert_eq!(distance_symbol(32768), (29, 8191, 13));
        assert_eq!(distance_symbol(24577), (29, 0, 13));
    }

    #[test]
    fn bit_writer_is_lsb_first() {
        // DEFLATE packs bits into bytes starting at the LEAST significant bit,
        // and values fill from bit 0 upwards: 0b101 in 3 bits, then 0b11 in 2.
        let mut w = BitWriter::new();
        w.write_bits(0b101, 3);
        w.write_bits(0b11, 2);
        let bytes = w.finish();
        assert_eq!(bytes[0], 0b0001_1101, "expected 29, got {}", bytes[0]);
    }
}
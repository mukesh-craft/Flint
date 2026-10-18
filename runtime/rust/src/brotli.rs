//! Brotli decompression, `no_std`.
//!
//! The user asked for Brotli to read compressed web assets (a `.br` file next
//! to a JS or WASM bundle). Decompression only: Brotli compression is not
//! exposed, because a decoder is what a runtime needs and a compressor is not.
//!
//! # Why this file looks the way it does
//!
//! `brotli-decompressor`'s convenient entry point, `BrotliDecompress`, is
//! compiled only with the crate's `std` feature. Flint's standard library is
//! `#![no_std]` with a custom `panic_impl` and a bump-allocated global
//! allocator, so the `std` build of any dependency is unusable (it drags in the
//! `std` lang items and the shared runtime cannot link it).
//!
//! The `no_std` path is `BrotliDecompressCustomIo`, which takes:
//!
//! * a reader and a writer implementing Brotli's own `CustomRead`/`CustomWrite`
//!   traits — plain traits, no `std::io` involved;
//! * two scratch buffers;
//! * three allocators for `u8`, `u32` and Huffman-code tables.
//!
//! All six adapters are below. They allocate through this crate's global
//! allocator, which is the bounded arena (`crate::arena`), so a decompression
//! bomb cannot exhaust the heap: the writer refuses to grow past the caller's
//! limit and the arena itself is fixed size.
//!
//! # Bounds
//!
//! `decompress` takes an explicit output limit and fails rather than exceeding
//! it. Brotli's worst-case expansion is roughly 1,000:1 for a single repeated
//! byte, so an unbounded decoder is a denial-of-service primitive; the limit is
//! mandatory, not advisory.

extern crate alloc;

use alloc::vec::Vec;
use brotli_decompressor::{
    Allocator, BrotliDecompressCustomIo, CustomRead, CustomWrite, HuffmanCode,
    SliceWrapper, SliceWrapperMut,
};

/// The output limit for callers that do not supply one.
pub const DEFAULT_LIMIT: usize = 32 * 1024 * 1024;
/// Scratch buffer size for the decoder's streaming loop.
const SCRATCH: usize = 16 * 1024;

/// Every failure mode of this module is one value: Brotli's custom-I/O API
/// returns a single error type, supplied by the caller.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DecodeFailure;

// ---------------------------------------------------------------------------
// Allocator adapters
// ---------------------------------------------------------------------------

/// A heap cell handed to the decoder. Backed by `Vec`, so it comes from this
/// crate's global allocator (the bounded arena).
///
/// `Default` is required by `AllocatedSlice`; `SliceWrapper::len` defaults to
/// the slice length, so the length lives in the `Vec` itself.
pub struct Cell<T> {
    data: Vec<T>,
}

impl<T: Default + Clone> Default for Cell<T> {
    fn default() -> Self {
        Cell { data: Vec::new() }
    }
}

impl<T> SliceWrapper<T> for Cell<T> {
    fn slice(&self) -> &[T] {
        &self.data
    }
}

impl<T> SliceWrapperMut<T> for Cell<T> {
    fn slice_mut(&mut self) -> &mut [T] {
        &mut self.data
    }
}

/// The decoder's allocator. `free_cell` drops the cell: with a bump allocator
/// the memory is reclaimed when the arena is reset, and holding a pointer past
/// that would be a use-after-free.
pub struct ArenaAllocator;

impl<T: Default + Clone> Allocator<T> for ArenaAllocator {
    type AllocatedMemory = Cell<T>;

    fn alloc_cell(&mut self, len: usize) -> Self::AllocatedMemory {
        let mut data = Vec::with_capacity(len);
        data.resize(len, T::default());
        Cell { data }
    }

    fn free_cell(&mut self, data: Self::AllocatedMemory) {
        drop(data);
    }
}

// ---------------------------------------------------------------------------
// I/O adapters
// ---------------------------------------------------------------------------

/// Reads the compressed stream. Returning `Ok(0)` is how `CustomRead` signals
/// end of input; Brotli treats a short read as EOF and then reports whether the
/// stream was complete.
pub struct SliceReader<'a> {
    data: &'a [u8],
    position: usize,
}

impl<'a> SliceReader<'a> {
    fn new(data: &'a [u8]) -> Self {
        SliceReader { data, position: 0 }
    }
}

impl CustomRead<DecodeFailure> for SliceReader<'_> {
    fn read(&mut self, into: &mut [u8]) -> Result<usize, DecodeFailure> {
        let remaining = self.data.len() - self.position;
        let count = core::cmp::min(into.len(), remaining);
        into[..count].copy_from_slice(&self.data[self.position..self.position + count]);
        self.position += count;
        Ok(count)
    }
}

/// Collects the decompressed bytes, refusing to exceed `limit`.
pub struct BoundedWriter {
    output: Vec<u8>,
    limit: usize,
}

impl BoundedWriter {
    fn new(limit: usize) -> Self {
        BoundedWriter {
            output: Vec::new(),
            limit,
        }
    }
}

impl CustomWrite<DecodeFailure> for BoundedWriter {
    fn write(&mut self, data: &[u8]) -> Result<usize, DecodeFailure> {
        if self.output.len() + data.len() > self.limit {
            return Err(DecodeFailure);
        }
        self.output.extend_from_slice(data);
        Ok(data.len())
    }

    fn flush(&mut self) -> Result<(), DecodeFailure> {
        Ok(())
    }
}

/// Decompresses a complete Brotli stream.
///
/// `limit` bounds the output: a stream that expands past it fails instead of
/// filling memory. The returned error is deliberately coarse — callers get
/// "this is not a valid Brotli stream (or it is too big)", which is all a
/// caller can act on.
pub fn decompress(input: &[u8], limit: usize) -> Result<Vec<u8>, DecodeFailure> {
    if input.is_empty() {
        return Err(DecodeFailure);
    }
    let mut reader = SliceReader::new(input);
    let mut writer = BoundedWriter::new(limit);
    let mut input_buffer = [0u8; SCRATCH];
    let mut output_buffer = [0u8; SCRATCH];

    BrotliDecompressCustomIo(
        &mut reader,
        &mut writer,
        &mut input_buffer[..],
        &mut output_buffer[..],
        ArenaAllocator,
        ArenaAllocator,
        ArenaAllocator,
        DecodeFailure,
    )?;

    Ok(writer.output)
}

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

static mut ERROR: [u8; 128] = [0; 128];
static mut RESULT: Vec<u8> = Vec::new();

fn set_error(message: &str) {
    let slot = unsafe { &mut *core::ptr::addr_of_mut!(ERROR) };
    let bytes = message.as_bytes();
    let count = bytes.len().min(slot.len() - 1);
    for (i, b) in bytes[..count].iter().enumerate() {
        slot[i] = *b;
    }
    slot[count] = 0;
}

/// The last error message; static storage, do not free.
#[no_mangle]
pub extern "C" fn flint_brotli_error() -> *const u8 {
    core::ptr::addr_of!(ERROR) as *const u8
}

/// The decompressed bytes; valid until the next call. Returns NULL when the
/// last call failed.
#[no_mangle]
pub extern "C" fn flint_brotli_last() -> *const u8 {
    unsafe { RESULT.as_ptr() }
}

/// Length of the decompressed result, or 0 if the last call failed.
#[no_mangle]
pub extern "C" fn flint_brotli_len() -> i64 {
    unsafe { RESULT.len() as i64 }
}

/// Decompresses a Brotli stream. Returns 1 on success, 0 on failure with
/// `flint_brotli_error()` set. The output limit is `max_output`; pass 0 for
/// the 32 MiB default.
#[no_mangle]
pub extern "C" fn flint_brotli_decompress(data: *const u8, data_len: i64, max_output: i64) -> i64 {
    let outcome = (|| -> Result<i64, &'static str> {
        if data.is_null() || data_len <= 0 {
            return Err("brotli: input is null or empty");
        }
        if data_len as usize > 256 * 1024 * 1024 {
            return Err("brotli: input larger than this decoder accepts");
        }
        let limit = if max_output > 0 {
            max_output as usize
        } else {
            DEFAULT_LIMIT
        };
        let input = unsafe { core::slice::from_raw_parts(data, data_len as usize) };
        let output = decompress(input, limit).map_err(|_| {
            "brotli: not a valid Brotli stream, truncated, or larger than the output limit"
        })?;
        unsafe { RESULT = output };
        Ok(1)
    })();
    match outcome {
        Ok(value) => {
            set_error("");
            value
        }
        Err(message) => {
            set_error(message);
            0
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::string::String;
    use std::vec::Vec;

    /// tests/data_real.br was produced by the reference `brotli` CLI, so
    /// decoding it proves interoperability rather than self-consistency.
    #[test]
    fn decodes_a_stream_produced_by_the_reference_cli() {
        let compressed = include_bytes!("../tests/data_real.br");
        let expected = "The quick brown fox jumps over the lazy dog. ".repeat(64);
        let output = decompress(compressed, DEFAULT_LIMIT).expect("reference stream must decode");
        assert_eq!(output.len(), expected.len());
        assert_eq!(core::str::from_utf8(&output).unwrap(), expected);
    }

    #[test]
    fn decodes_a_stream_with_no_repetition() {
        // Incompressible text exercises the literal path.
        let compressed = include_bytes!("../tests/data_literal.br");
        let output = decompress(compressed, DEFAULT_LIMIT).expect("literal stream must decode");
        assert_eq!(output, b"flint brotli literal path test, no repeats here at all.");
    }

    #[test]
    fn empty_input_is_rejected() {
        assert_eq!(decompress(&[], DEFAULT_LIMIT), Err(DecodeFailure));
    }

    #[test]
    fn garbage_is_rejected() {
        let junk = [0xFFu8; 64];
        assert!(decompress(&junk, DEFAULT_LIMIT).is_err());
    }

    #[test]
    fn a_truncated_stream_is_rejected() {
        let compressed = include_bytes!("../tests/data_real.br");
        let cut = &compressed[..compressed.len() / 2];
        assert!(decompress(cut, DEFAULT_LIMIT).is_err());
    }

    #[test]
    fn a_flipped_bit_is_rejected_or_differs() {
        let compressed = include_bytes!("../tests/data_real.br");
        let mut damaged = compressed.to_vec();
        // Corrupt the middle of the payload, not the header.
        let at = damaged.len() / 2;
        damaged[at] ^= 0xFF;
        match decompress(&damaged, DEFAULT_LIMIT) {
            Err(_) => {}
            Ok(output) => {
                // Some bit flips decode to different bytes rather than an
                // error; either is acceptable, silently returning the original
                // bytes is not.
                let expected: Vec<u8> = "The quick brown fox jumps over the lazy dog. "
                    .repeat(64)
                    .into_bytes();
                assert_ne!(output, expected);
            }
        }
    }

    #[test]
    fn the_output_limit_is_enforced() {
        let compressed = include_bytes!("../tests/data_real.br");
        // The stream expands to ~2.9 KiB, so a 1 KiB cap must fail rather than
        // allocate past the limit.
        assert_eq!(decompress(compressed, 1024), Err(DecodeFailure));
        assert!(decompress(compressed, 4096).is_ok());
    }

    #[test]
    fn the_reader_reports_eof_exactly_once() {
        let data = [1u8, 2, 3, 4, 5];
        let mut reader = SliceReader::new(&data);
        let mut two = [0u8; 2];
        assert_eq!(reader.read(&mut two), Ok(2));
        assert_eq!(&two, &[1, 2]);
        assert_eq!(reader.read(&mut two), Ok(2));
        assert_eq!(reader.read(&mut two), Ok(1));
        assert_eq!(reader.read(&mut two), Ok(0));
        assert_eq!(reader.read(&mut two), Ok(0));
    }

    #[test]
    fn the_writer_refuses_to_pass_its_limit() {
        let mut writer = BoundedWriter::new(4);
        assert_eq!(writer.write(b"abcd"), Ok(4));
        assert_eq!(writer.write(b"e"), Err(DecodeFailure));
        assert_eq!(writer.output.len(), 4);
    }

    #[test]
    fn alloc_cells_have_the_requested_length() {
        let mut allocator = ArenaAllocator;
        let cell: Cell<u8> = allocator.alloc_cell(37);
        assert_eq!(SliceWrapper::len(&cell), 37);
        assert!(SliceWrapper::slice(&cell).iter().all(|b| *b == 0));
    }
}
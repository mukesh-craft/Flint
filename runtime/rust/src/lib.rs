//! CSV parsing for the Flint standard library — `no_std` on purpose.
//!
//! # Why no_std
//!
//! Two facts about this project forced this design, both found by experiment
//! rather than assumption:
//!
//! 1. **Rust's `std` cannot be linked into Flint's JIT.** `std` resolves
//!    thread-local handles through libc; inside JIT memory there is no DSO, and
//!    Android's linker aborts the process with
//!    `increment_dso_handle_reference_counter: Couldn't find soinfo by
//!    dso_handle`.
//! 2. **Two Rust static libraries cannot share one link.** A user linking their
//!    own `.a` alongside Flint's Rust modules gets duplicate `std` symbols
//!    (`rust_eh_personality`, `std::panicking::EMPTY_PANIC`). With `no_std`,
//!    Flint's modules bring no `std` at all, so user Rust libraries and Flint's
//!    modules coexist in the same binary.
//!
//! The cost is a fixed arena instead of the heap. For a parser that is a fair
//! trade — allocation-free parsing is also faster — and the limit is reported
//! as a loud error instead of an abort when a file is too big.
//!
//! Memory safety is otherwise unchanged: parsing runs on slices, indexing is
//! bounds-checked, and every pointer argument is validated before use.

#![no_std]

#[cfg(not(test))]
use core::panic::PanicInfo;
use core::ptr;

// Sized for the shipped module; scaled down under test. `Doc` is ~1.6 MB with
// these limits, which no test thread wants on its stack, and tests run in
// parallel so a shared static would race.
#[cfg(not(test))]
pub const ARENA_BYTES: usize = 1 << 20;
#[cfg(test)]
pub const ARENA_BYTES: usize = 8 * 1024;

/// Maximum rows (each row needs one row-end marker).
#[cfg(not(test))]
const MAX_ROWS: usize = 8192;
#[cfg(test)]
const MAX_ROWS: usize = 256;

/// Maximum fields across the whole document (each needs an offset+length).
#[cfg(not(test))]
const MAX_FIELDS: usize = 65536;
#[cfg(test)]
const MAX_FIELDS: usize = 4096;

// The library needs its own panic handler (no_std), but the test build links
// std, which already provides one. Two `panic_impl` lang items do not compile.
#[cfg(not(test))]
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    // `no_std` has nowhere to unwind to. A panic here means a bug in this
    // module, and the FFI entry points are written so user input cannot reach
    // one. Calling libc's `abort()` is deliberate: an earlier version used a
    // raw `svc #0` syscall, and Android delivers that as SIGSEGV with no
    // message at all, which turned an ordinary assertion failure into an
    // unattributable segfault. libc is always linked here (Flint's AOT path
    // links it), and abort() at least names itself.
    // SAFETY: abort() never returns and has no preconditions.
    unsafe { abort() }
}

// libc's abort, declared directly: `std` is not linked (see the module docs),
// and `core::process::abort` does not exist.
extern "C" {
    fn abort() -> !;
}

struct Doc {
    /// Field bytes, NUL-terminated, appended here.
    arena: [u8; ARENA_BYTES],
    arena_len: usize,
    /// (offset, len) per field, in document order.
    fields: [(u32, u32); MAX_FIELDS],
    field_count: usize,
    /// Index into `fields` just past the end of each row.
    row_ends: [u32; MAX_ROWS],
    row_count: usize,
    /// Parsing failure, NUL-terminated in `error`.
    error: [u8; 128],
    error_len: usize,
}

static mut DOC: Doc = Doc {
    arena: [0; ARENA_BYTES],
    arena_len: 0,
    fields: [(0, 0); MAX_FIELDS],
    field_count: 0,
    row_ends: [0; MAX_ROWS],
    row_count: 0,
    error: [0; 128],
    error_len: 0,
};

impl Doc {
    const fn new() -> Self {
        Doc {
            arena: [0; ARENA_BYTES],
            arena_len: 0,
            fields: [(0, 0); MAX_FIELDS],
            field_count: 0,
            row_ends: [0; MAX_ROWS],
            row_count: 0,
            error: [0; 128],
            error_len: 0,
        }
    }

    fn reset(&mut self) {
        self.arena_len = 0;
        self.field_count = 0;
        self.row_count = 0;
        self.error_len = 0;
    }

    fn set_error(&mut self, message: &str) {
        self.error_len = message.len().min(self.error.len() - 1);
        for (i, byte) in message.as_bytes()[..self.error_len].iter().enumerate() {
            self.error[i] = *byte;
        }
        self.error[self.error_len] = 0;
    }

    /// Copies `bytes` into the arena and records it as a field. The terminator
    /// is written here so callers can treat every field as a C string.
    fn push_field(&mut self, bytes: &[u8]) -> Result<(), &'static str> {
        if self.field_count >= MAX_FIELDS {
            return Err("csv: too many fields");
        }
        if self.arena_len + bytes.len() + 1 > ARENA_BYTES {
            return Err("csv: document exceeds the 1 MiB parse arena");
        }
        let start = self.arena_len;
        self.arena[start..start + bytes.len()].copy_from_slice(bytes);
        self.arena[start + bytes.len()] = 0;
        self.arena_len = start + bytes.len() + 1;
        self.fields[self.field_count] = (start as u32, bytes.len() as u32);
        self.field_count += 1;
        Ok(())
    }

    fn close_row(&mut self) -> Result<(), &'static str> {
        if self.row_count >= MAX_ROWS {
            return Err("csv: too many rows");
        }
        self.row_ends[self.row_count] = self.field_count as u32;
        self.row_count += 1;
        Ok(())
    }
}

/// The parser. Pure over a byte slice: no allocation, no global state, and every
/// read from `input` is a bounds-checked index.
// The `flush_field` macro ends with `pending_len = 0`; on the final iteration
// of the loop that store is dead. Harmless, and cheaper to allow than to
// restructure the macro around a return value.
#[allow(unused_assignments)]
fn parse(doc: &mut Doc, input: &[u8], delim: u8) -> Result<(), &'static str> {
    let mut i = 0usize;
    let mut in_quotes = false;
    let mut field_was_quoted = false;
    let mut row_has_fields = false;
    // Current field may contain escapes, so it is parsed from two sources:
    // `input[field_start..i]` plus any doubled-quote collapses.
    let mut pending: [u8; 4096] = [0; 4096];
    let mut pending_len = 0usize;

    macro_rules! flush_field {
        () => {{
            doc.push_field(&pending[..pending_len])?;
            pending_len = 0;
        }};
    }

    while i < input.len() {
        let c = input[i];
        if in_quotes {
            if c == b'"' {
                if i + 1 < input.len() && input[i + 1] == b'"' {
                    // "" inside a quoted field is one literal quote.
                    if pending_len >= pending.len() {
                        return Err("csv: quoted field exceeds 4096 bytes");
                    }
                    pending[pending_len] = b'"';
                    pending_len += 1;
                    i += 2;
                    continue;
                }
                in_quotes = false;
                i += 1;
                continue;
            }
            if pending_len >= pending.len() {
                return Err("csv: field exceeds 4096 bytes");
            }
            pending[pending_len] = c;
            pending_len += 1;
            i += 1;
            continue;
        }

        match c {
            b'"' if pending_len == 0 && !field_was_quoted => {
                in_quotes = true;
                field_was_quoted = true;
                i += 1;
            }
            b'"' => return Err("csv: unexpected quote inside an unquoted field"),
            b'\r' | b'\n' => {
                if c == b'\r' && i + 1 < input.len() && input[i + 1] == b'\n' {
                    i += 1;
                }
                i += 1;
                // A line that produced no fields at all is a blank line, and
                // blank lines are skipped by this dialect. Tracked explicitly:
                // keying off "the last field is empty" also swallows the
                // legitimate ",," row of empty fields.
                if !row_has_fields {
                    continue;
                }
                flush_field!();
                row_has_fields = false;
                doc.close_row()?;
            }
            b'#' if pending_len == 0 && !field_was_quoted => {
                // Comment: skip the rest of the line INCLUDING its newline.
                // Leaving the newline behind would close an empty row and add
                // a phantom record -- which is exactly what the first version
                // of this parser did.
                while i < input.len() && input[i] != b'\n' {
                    i += 1;
                }
                if i < input.len() {
                    i += 1;
                }
            }
            other if other == delim => {
                flush_field!();
                row_has_fields = true;
                field_was_quoted = false;
                i += 1;
            }
            other => {
                if pending_len >= pending.len() {
                    return Err("csv: field exceeds 4096 bytes");
                }
                pending[pending_len] = other;
                pending_len += 1;
                i += 1;
            }
        }
    }

    if in_quotes {
        return Err("csv: unterminated quoted field at end of input");
    }
    if row_has_fields || pending_len > 0 {
        flush_field!();
        doc.close_row()?;
    }
    Ok(())
}

// --- C ABI ------------------------------------------------------------------

/// Reads a NUL-terminated argument. Returns None for null or unterminated
/// input, which callers turn into an error message rather than a crash.
unsafe fn cstr_bytes<'a>(ptr: *const u8) -> Option<&'a [u8]> {
    if ptr.is_null() {
        return None;
    }
    let mut len = 0usize;
    // Bounded scan: a non-terminated C string must not walk off the heap.
    while len < ARENA_BYTES {
        if unsafe { *ptr.add(len) } == 0 {
            return Some(unsafe { core::slice::from_raw_parts(ptr, len) });
        }
        len += 1;
    }
    None
}

/// Parse `text` with the single-byte `delim`. Returns the row count, or -1 on
/// failure — see `flint_csv_error`.
#[no_mangle]
pub extern "C" fn flint_csv_parse(text: *const u8, delim: *const u8) -> i64 {
    let doc = unsafe { &mut *ptr::addr_of_mut!(DOC) };
    doc.reset();

    let outcome = (|| -> Result<(), &'static str> {
        let Some(body) = (unsafe { cstr_bytes(text) }) else {
            return Err("csv: input is null or not NUL-terminated");
        };
        let Some(d) = (unsafe { cstr_bytes(delim) }) else {
            return Err("csv: delimiter is null or not NUL-terminated");
        };
        if d.len() != 1 {
            return Err("csv: delimiter must be exactly one character");
        }
        parse(doc, body, d[0])
    })();

    match outcome {
        Ok(()) => doc.row_count as i64,
        Err(message) => {
            doc.row_count = 0;
            doc.field_count = 0;
            doc.set_error(message);
            -1
        }
    }
}

#[no_mangle]
pub extern "C" fn flint_csv_rows() -> i64 {
    unsafe { (*ptr::addr_of!(DOC)).row_count as i64 }
}

/// Column count of `row`, or -1 when out of range.
#[no_mangle]
pub extern "C" fn flint_csv_cols(row: i64) -> i64 {
    let doc = unsafe { &*ptr::addr_of!(DOC) };
    if row < 0 || row as usize >= doc.row_count {
        return -1;
    }
    let end = doc.row_ends[row as usize] as usize;
    let start = if row == 0 { 0 } else { doc.row_ends[row as usize - 1] as usize };
    (end - start) as i64
}

/// Field `col` of `row` as a pointer into the arena. Valid until the next
/// parse. Null on any out-of-range index.
///
/// No allocation happens here, so there is nothing to free: the returned
/// pointer belongs to the module's arena and `flint_csv_free` is a no-op kept
/// for symmetry with the rest of the Flint string API.
#[no_mangle]
pub extern "C" fn flint_csv_get(row: i64, col: i64) -> *const u8 {
    let doc = unsafe { &*ptr::addr_of!(DOC) };
    if row < 0 || col < 0 || row as usize >= doc.row_count {
        return ptr::null();
    }
    let end = doc.row_ends[row as usize] as usize;
    let start = if row == 0 { 0 } else { doc.row_ends[row as usize - 1] as usize };
    let idx = start + col as usize;
    if idx >= end || idx >= doc.field_count {
        return ptr::null();
    }
    let (offset, _len) = doc.fields[idx];
    // Safety: `offset` was written by push_field, which bounds-checked the copy.
    unsafe { doc.arena.as_ptr().add(offset as usize) }
}

/// Releases a string returned by `flint_csv_get`. Kept so Flint code can follow
/// the same free-then-reuse shape as every other string builtin; the arena owns
/// the bytes, so this does nothing.
#[no_mangle]
pub extern "C" fn flint_csv_free(_text: *const u8) {}

/// The last parse error, or "" after a successful parse. Static storage: do not
/// free it.
#[no_mangle]
pub extern "C" fn flint_csv_error() -> *const u8 {
    let doc = unsafe { &*ptr::addr_of!(DOC) };
    doc.error.as_ptr()
}

/// Arena capacity in bytes, so a program can report the limit before parsing.
#[no_mangle]
pub extern "C" fn flint_csv_arena_bytes() -> i64 {
    ARENA_BYTES as i64
}

// The library is no_std; tests may use std (they never ship).
#[cfg(test)]
extern crate std;

#[cfg(test)]
mod tests {
    use super::*;
    /// Heap-allocate the document. `Doc` is ~41 KiB even in the scaled-down
    /// test build, and stack-constructing it overflows this platform's test
    /// harness threads (SIGSEGV before the test body even starts). The shipped
    /// module never builds one on the stack: its `DOC` is a static.
    fn test_doc() -> std::boxed::Box<Doc> {
        let layout = std::alloc::Layout::new::<Doc>();
        // SAFETY: layout matches Doc exactly; alloc_zeroed gives a valid,
        // zeroed Doc (every field's zero value is the intended initial state).
        let raw = unsafe { std::alloc::alloc_zeroed(layout) as *mut Doc };
        assert!(!raw.is_null(), "out of memory");
        unsafe { std::boxed::Box::from_raw(raw) }
    }
    use std::format;
    use std::string::{String, ToString};
    use std::vec;
    use std::vec::Vec;

    fn run(text: &str, delim: u8) -> Result<(usize, usize), &'static str> {
        let mut doc = test_doc();
        parse(&mut doc, text.as_bytes(), delim)?;
        Ok((doc.row_count, doc.field_count))
    }

    fn rows(text: &str, delim: u8) -> Vec<Vec<String>> {
        let mut doc = test_doc();
        parse(&mut doc, text.as_bytes(), delim).expect("parse");
        let mut out = Vec::new();
        for r in 0..doc.row_count {
            let end = doc.row_ends[r] as usize;
            let start = if r == 0 { 0 } else { doc.row_ends[r - 1] as usize };
            let mut fields = Vec::new();
            for idx in start..end {
                let (off, len) = doc.fields[idx];
                fields.push(String::from_utf8_lossy(&doc.arena[off as usize..off as usize + len as usize]).to_string());
            }
            out.push(fields);
        }
        out
    }

    #[test]
    fn simple_rows() {
        assert_eq!(rows("a,b,c\n1,2,3\n", b','), vec![vec!["a", "b", "c"], vec!["1", "2", "3"]]);
    }

    #[test]
    fn quoted_field_keeps_delimiter_and_newline() {
        let r = rows("\"x,y\",\"line\nbreak\"\n", b',');
        assert_eq!(r[0][0], "x,y");
        assert_eq!(r[0][1], "line\nbreak");
    }

    #[test]
    fn doubled_quote_is_one_quote() {
        let r = rows("\"say \"\"hi\"\"\",x\n", b',');
        assert_eq!(r[0][0], "say \"hi\"");
        assert_eq!(r[0][1], "x");
    }

    #[test]
    fn crlf_and_missing_trailing_newline() {
        assert_eq!(rows("a,b\r\n1,2", b','), vec![vec!["a", "b"], vec!["1", "2"]]);
    }

    #[test]
    fn empty_fields_preserved_and_blank_lines_skipped() {
        let r = rows("a,,c\n\n,\n", b',');
        assert_eq!(r[0], vec!["a", "", "c"]);
        assert_eq!(r[1], vec!["", ""]);
    }

    #[test]
    fn comment_lines_skipped() {
        let r = rows("# header\na,b\n1,2\n", b',');
        assert_eq!(r.len(), 2);
        assert_eq!(r[0], vec!["a", "b"]);
    }

    #[test]
    fn semicolon_delimiter() {
        assert_eq!(rows("a;b\n", b';'), vec![vec!["a", "b"]]);
    }

    #[test]
    fn unterminated_quote_errors() {
        assert!(run("\"never closed\n", b',').is_err());
    }

    #[test]
    fn inner_quote_errors() {
        assert!(run("ab\"cd,e\n", b',').is_err());
    }

    #[test]
    fn doc_can_be_constructed_alone() {
        let d = test_doc();
        assert_eq!(d.row_count, 0);
    }

    #[test]
    fn empty_input_yields_no_rows() {
        assert_eq!(run("", b','), Ok((0, 0)));
    }

    #[test]
    fn oversized_field_is_reported_not_truncated() {
        let big = "x".repeat(5000);
        let err = run(&format!("{big},y\n"), b',').unwrap_err();
        assert!(err.contains("4096"), "{err}");
    }

    #[test]
    fn out_of_range_accessors_return_sentinels() {
        let mut doc = test_doc();
        parse(&mut doc, b"a\n", b',').unwrap();
        assert_eq!(doc.row_count, 1);
        assert_eq!(end_of(&doc, 0) - 0, 1);
        assert!(doc.row_ends.len() >= MAX_ROWS);
    }

    fn end_of(doc: &Doc, row: usize) -> usize {
        doc.row_ends[row] as usize
    }

    #[test]
    fn arena_overflow_is_reported() {
        // Many short fields, so the document outgrows the arena before any
        // single field hits the 4096-byte per-field cap.
        let mut input: Vec<u8> = Vec::new();
        while input.len() < ARENA_BYTES + 512 {
            input.extend_from_slice(b"ab,");
        }
        let mut doc = test_doc();
        let err = parse(&mut doc, &input, b',').unwrap_err();
        assert!(err.contains("arena"), "{err}");
    }

    #[test]
    fn long_single_field_hits_the_field_cap_first() {
        // Documented limit ordering: the per-field cap (4096) reports before
        // the arena cap, so a pathological field is diagnosed precisely.
        let mut doc = Doc::new();
        let long = vec![b'a'; 5000];
        let err = parse(&mut doc, &long, b',').unwrap_err();
        assert!(err.contains("4096"), "{err}");
    }
}
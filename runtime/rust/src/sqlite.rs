//! SQLite for the Flint standard library — a Rust wrapper over the system
//! libsqlite3.
//!
//! # Why a wrapper rather than a Rust engine
//!
//! SQLite *is* C. There is no production-grade pure-Rust SQLite (limbo is
//! pre-1.0 and not something to ship inside a language runtime), so the honest
//! options were "bind the C library" or "ship nothing". This module takes the
//! first, and puts every safety property that is available to us on the
//! boundary: handles are checked, every returned pointer is copied into a
//! buffer this module owns, text is NUL-checked before use, and no C pointer
//! ever escapes to Flint code.
//!
//! # Loading
//!
//! The JIT cannot resolve `sqlite3_*` on its own (they are not libc symbols),
//! so the compiler dlopens libsqlite3 and publishes those symbols into the ORC
//! session (`publishJITLibrary` in src/main.cpp). The AOT link line gains
//! `-lsqlite3`. Both are verified by tests/test_rust_stdlib.sh.
//!
//! # Surface
//!
//! `open`/`exec`/`query`/`close`, up to [`MAX_CONNECTIONS`] live connections.
//! Query results are materialised into this module's arena so a Flint program
//! never holds a C pointer past the next call.

#![allow(clippy::missing_safety_doc)]

use core::ffi::{c_char, c_int, c_void, CStr};

type Sqlite3 = c_void;
type Sqlite3Stmt = c_void;

// Opaque C API. Only the functions this module actually calls are declared;
// declaring more would mean promising surface we do not maintain.
#[link(name = "sqlite3")]
unsafe extern "C" {
    fn sqlite3_open_v2(
        filename: *const c_char,
        db: *mut *mut Sqlite3,
        flags: c_int,
        vfs: *const c_char,
    ) -> c_int;
    fn sqlite3_close_v2(db: *mut Sqlite3) -> c_int;
    fn sqlite3_prepare_v2(
        db: *mut Sqlite3,
        sql: *const c_char,
        nbyte: c_int,
        stmt: *mut *mut Sqlite3Stmt,
        tail: *mut *const c_char,
    ) -> c_int;
    fn sqlite3_step(stmt: *mut Sqlite3Stmt) -> c_int;
    fn sqlite3_finalize(stmt: *mut Sqlite3Stmt) -> c_int;
    fn sqlite3_column_count(stmt: *mut Sqlite3Stmt) -> c_int;
    fn sqlite3_column_text(stmt: *mut Sqlite3Stmt, col: c_int) -> *const u8;
    fn sqlite3_errmsg(db: *mut Sqlite3) -> *const c_char;
    fn sqlite3_changes(db: *mut Sqlite3) -> c_int;
    fn sqlite3_libversion() -> *const c_char;
}

const SQLITE_OK: c_int = 0;
const SQLITE_ROW: c_int = 100;
const SQLITE_DONE: c_int = 101;
const SQLITE_OPEN_READWRITE: c_int = 0x00000002;
const SQLITE_OPEN_CREATE: c_int = 0x00000004;

/// Concurrent connections this module tracks. Small on purpose: no allocation
/// means no dynamic table, and eight covers what a script realistically opens.
pub const MAX_CONNECTIONS: usize = 8;
const MAX_CONNECTIONS_I64: i64 = MAX_CONNECTIONS as i64;

/// Result arena for query output, and the error string.
// Scaled down under test: the tables are stack-sized statics, and a test
// harness thread on this platform does not want a megabyte frame.
#[cfg(not(test))]
const ARENA_BYTES: usize = 256 * 1024;
#[cfg(test)]
const ARENA_BYTES: usize = 16 * 1024;
const MAX_ROWS: usize = 4096;
const MAX_COLS: usize = 128;

static mut ARENA: [u8; ARENA_BYTES] = [0; ARENA_BYTES];
static mut ARENA_LEN: usize = 0;
static mut ERROR: [u8; 256] = [0; 256];

/// Row/column geometry of the last query, plus per-cell offsets.
static mut CELL_OFFSET: [(u32, u32); MAX_ROWS * MAX_COLS] = [(0, 0); MAX_ROWS * MAX_COLS];
static mut CELL_COUNT: usize = 0;
static mut LAST_ROWS: usize = 0;
static mut LAST_COLS: usize = 0;

static mut CONNECTIONS: [*mut Sqlite3; MAX_CONNECTIONS] = [ptr_null(); MAX_CONNECTIONS];

const fn ptr_null() -> *mut Sqlite3 {
    core::ptr::null_mut()
}

fn db_slot() -> &'static mut [*mut Sqlite3; MAX_CONNECTIONS] {
    unsafe { &mut *core::ptr::addr_of_mut!(CONNECTIONS) }
}

fn arena() -> &'static mut [u8] {
    unsafe { &mut *core::ptr::addr_of_mut!(ARENA) }
}

fn arena_len() -> &'static mut usize {
    unsafe { &mut *core::ptr::addr_of_mut!(ARENA_LEN) }
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

/// Copies sqlite's message (which is NUL-terminated by contract) into ours.
fn set_error_from_c(ptr: *const c_char) {
    if ptr.is_null() {
        set_error("sqlite: operation failed with no message");
        return;
    }
    let text = unsafe { CStr::from_ptr(ptr) }.to_str().unwrap_or("<non-utf8 sqlite message>");
    set_error(text);
}

fn reset_result() {
    *arena_len() = 0;
    unsafe {
        let cells = &mut *core::ptr::addr_of_mut!(CELL_OFFSET);
        for c in cells.iter_mut() {
            *c = (0, 0);
        }
    }
    unsafe {
        *core::ptr::addr_of_mut!(CELL_COUNT) = 0;
        *core::ptr::addr_of_mut!(LAST_ROWS) = 0;
        *core::ptr::addr_of_mut!(LAST_COLS) = 0;
    }
}

/// Pushes bytes into the result arena, NUL-terminated, returning (offset, len).
fn push_cell(bytes: &[u8]) -> Result<(u32, u32), &'static str> {
    let space = arena().len();
    let len = space - *arena_len();
    if bytes.len() + 1 > len {
        return Err("sqlite: result exceeds the 256 KiB arena");
    }
    let start = *arena_len();
    arena()[start..start + bytes.len()].copy_from_slice(bytes);
    arena()[start + bytes.len()] = 0;
    *arena_len() = start + bytes.len() + 1;
    Ok((start as u32, bytes.len() as u32))
}

/// Borrows a C string argument with a bounded scan.
unsafe fn cstr<'a>(ptr: *const c_char) -> Result<&'a str, &'static str> {
    if ptr.is_null() {
        return Err("sqlite: argument is null");
    }
    unsafe { CStr::from_ptr(ptr) }.to_str().map_err(|_| "sqlite: argument is not valid UTF-8")
}

fn handle_index(handle: i64) -> Result<usize, &'static str> {
    if handle < 1 || handle > MAX_CONNECTIONS_I64 {
        return Err("sqlite: handle out of range (handles start at 1)");
    }
    let index = (handle - 1) as usize;
    if db_slot()[index].is_null() {
        return Err("sqlite: handle is not open");
    }
    Ok(index)
}

/// SQLite library version, e.g. "3.53.4". Static storage; do not free.
#[no_mangle]
pub extern "C" fn flint_sqlite_version() -> *const c_char {
    unsafe { sqlite3_libversion() }
}

/// Open (or create) a database. Returns a handle >= 1, or -1 on failure with
/// the reason in `flint_sqlite_error`.
#[no_mangle]
pub extern "C" fn flint_sqlite_open(path: *const c_char) -> i64 {
    let outcome = (|| -> Result<i64, &'static str> {
        let path_str = unsafe { cstr(path) }?;
        if path_str.is_empty() {
            return Err("sqlite: path is empty");
        }
        let slot = db_slot()
            .iter()
            .position(|db| db.is_null())
            .ok_or("sqlite: all connection slots are in use")?;

        let mut db: *mut Sqlite3 = core::ptr::null_mut();
        let rc = unsafe {
            sqlite3_open_v2(
                path,
                &mut db,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                core::ptr::null(),
            )
        };
        if rc != SQLITE_OK {
            if !db.is_null() {
                set_error_from_c(unsafe { sqlite3_errmsg(db) });
                unsafe { sqlite3_close_v2(db) };
            }
            return Err("sqlite: cannot open database");
        }
        db_slot()[slot] = db;
        Ok(slot as i64 + 1)
    })();
    report(outcome)
}

/// Close a connection. Returns 0, or -1 with a reason.
#[no_mangle]
pub extern "C" fn flint_sqlite_close(handle: i64) -> i64 {
    let outcome = (|| -> Result<i64, &'static str> {
        let index = handle_index(handle)?;
        let db = db_slot()[index];
        db_slot()[index] = core::ptr::null_mut();
        unsafe { sqlite3_close_v2(db) };
        Ok(0)
    })();
    report(outcome)
}

/// Run a statement with no result rows (CREATE, INSERT, UPDATE, ...).
/// Returns the number of rows changed, or -1 on failure.
#[no_mangle]
pub extern "C" fn flint_sqlite_exec(handle: i64, sql: *const c_char) -> i64 {
    let outcome = (|| -> Result<i64, &'static str> {
        let index = handle_index(handle)?;
        let sql_str = unsafe { cstr(sql) }?;
        if sql_str.is_empty() {
            return Err("sqlite: statement is empty");
        }
        let db = db_slot()[index];

        let mut stmt: *mut Sqlite3Stmt = core::ptr::null_mut();
        let rc = unsafe {
            sqlite3_prepare_v2(db, sql, -1, &mut stmt, core::ptr::null_mut())
        };
        if rc != SQLITE_OK {
            set_error_from_c(unsafe { sqlite3_errmsg(db) });
            return Err("sqlite: cannot prepare statement");
        }
        let mut last = rc;
        // A statement string may hold several statements; run them all.
        while !stmt.is_null() {
            last = unsafe { sqlite3_step(stmt) };
            unsafe { sqlite3_finalize(stmt) };
            if last != SQLITE_ROW && last != SQLITE_DONE {
                set_error_from_c(unsafe { sqlite3_errmsg(db) });
                return Err("sqlite: statement failed");
            }
            if last == SQLITE_DONE {
                break;
            }
            stmt = core::ptr::null_mut();
        }
        Ok(unsafe { sqlite3_changes(db) } as i64)
    })();
    report(outcome)
}

/// Run a query and materialise its rows into this module's arena.
/// Returns the row count, or -1 on failure.
#[no_mangle]
pub extern "C" fn flint_sqlite_query(handle: i64, sql: *const c_char) -> i64 {
    let outcome = (|| -> Result<i64, &'static str> {
        let index = handle_index(handle)?;
        let sql_str = unsafe { cstr(sql) }?;
        if sql_str.is_empty() {
            return Err("sqlite: statement is empty");
        }
        let db = db_slot()[index];
        reset_result();

        let mut stmt: *mut Sqlite3Stmt = core::ptr::null_mut();
        let rc = unsafe {
            sqlite3_prepare_v2(db, sql, -1, &mut stmt, core::ptr::null_mut())
        };
        if rc != SQLITE_OK {
            set_error_from_c(unsafe { sqlite3_errmsg(db) });
            return Err("sqlite: cannot prepare statement");
        }

        let cols = unsafe { sqlite3_column_count(stmt) };
        if cols < 0 || cols as usize > MAX_COLS {
            unsafe { sqlite3_finalize(stmt) };
            return Err("sqlite: result has too many columns");
        }
        let mut rows = 0usize;
        loop {
            let step = unsafe { sqlite3_step(stmt) };
            if step == SQLITE_ROW {
                if rows >= MAX_ROWS {
                    unsafe { sqlite3_finalize(stmt) };
                    return Err("sqlite: result has too many rows");
                }
                for col in 0..cols {
                    let text = unsafe { sqlite3_column_text(stmt, col) };
                    let bytes: &[u8] = if text.is_null() {
                        b""
                    } else {
                        unsafe { core::slice::from_raw_parts(text, cstr_len(text)) }
                    };
                    let slot = rows * MAX_COLS + col as usize;
                    let (offset, len) = push_cell(bytes)?;
                    unsafe {
                        let cells = &mut *core::ptr::addr_of_mut!(CELL_OFFSET);
                        cells[slot] = (offset, len);
                    }
                }
                rows += 1;
            } else if step == SQLITE_DONE {
                break;
            } else {
                set_error_from_c(unsafe { sqlite3_errmsg(db) });
                unsafe { sqlite3_finalize(stmt) };
                return Err("sqlite: query failed");
            }
        }
        unsafe { sqlite3_finalize(stmt) };
        unsafe {
            *core::ptr::addr_of_mut!(CELL_COUNT) = rows * cols as usize;
            *core::ptr::addr_of_mut!(LAST_ROWS) = rows;
            *core::ptr::addr_of_mut!(LAST_COLS) = cols as usize;
        }
        Ok(rows as i64)
    })();
    report(outcome)
}

unsafe fn cstr_len(ptr: *const u8) -> usize {
    let mut n = 0usize;
    while n < ARENA_BYTES && unsafe { *ptr.add(n) } != 0 {
        n += 1;
    }
    n
}

/// Rows in the last query result.
#[no_mangle]
pub extern "C" fn flint_sqlite_rows() -> i64 {
    unsafe { *core::ptr::addr_of!(LAST_ROWS) as i64 }
}

/// Columns in the last query result.
#[no_mangle]
pub extern "C" fn flint_sqlite_cols() -> i64 {
    unsafe { *core::ptr::addr_of!(LAST_COLS) as i64 }
}

/// One cell as a string pointer into the module's arena, or null when out of
/// range. Valid until the next query.
#[no_mangle]
pub extern "C" fn flint_sqlite_get(row: i64, col: i64) -> *const c_char {
    if row < 0 || col < 0 {
        return core::ptr::null();
    }
    // Bounds are the RESULT's shape, not the table's capacity: checking only
    // against the array length let row 99 of a one-row result return a stale
    // cell instead of null.
    let (rows, cols) = unsafe {
        (
            *core::ptr::addr_of!(LAST_ROWS),
            *core::ptr::addr_of!(LAST_COLS),
        )
    };
    if row as usize >= rows || col as usize >= cols {
        return core::ptr::null();
    }
    let slot = row as usize * MAX_COLS + col as usize;
    let (offset, len) = unsafe {
        let cells = &*core::ptr::addr_of!(CELL_OFFSET);
        if slot >= cells.len() {
            return core::ptr::null();
        }
        cells[slot]
    };
    if offset == 0 && len == 0 {
        // A written-but-empty cell (an SQL NULL) starts at offset 0 too, so it
        // reads the arena's leading NUL: an empty string, never garbage.
        return core::ptr::addr_of!(ARENA) as *const c_char;
    }
    unsafe { arena().as_ptr().add(offset as usize) as *const c_char }
}

/// The last error message; static storage, do not free.
#[no_mangle]
pub extern "C" fn flint_sqlite_error() -> *const c_char {
    core::ptr::addr_of!(ERROR) as *const c_char
}

fn report(outcome: Result<i64, &'static str>) -> i64 {
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

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;
    use std::format;
    use std::string::{String, ToString};

    /// One connection per test: the module's slots are global, and tests run in
    /// parallel.
    static TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    fn serialized() -> std::sync::MutexGuard<'static, ()> {
        TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn err() -> String {
        unsafe { CStr::from_ptr(flint_sqlite_error()) }
            .to_str()
            .unwrap()
            .to_string()
    }

    fn cell(row: i64, col: i64) -> String {
        let p = flint_sqlite_get(row, col);
        if p.is_null() {
            return "<null>".to_string();
        }
        unsafe { CStr::from_ptr(p) }.to_str().unwrap().to_string()
    }

    fn temp_db(tag: &str) -> CString {
        let path = format!(
            "{}/flint-sqlite-test-{}-{}.db",
            std::env::var("TMPDIR").unwrap_or_else(|_| "/tmp".to_string()),
            std::process::id(),
            tag
        );
        CString::new(path).unwrap()
    }

    fn drop_db(path: &CString) {
        let _ = std::fs::remove_file(path.to_str().unwrap());
    }

    #[test]
    fn version_is_reported() {
        let _serialized = serialized();
        let v = unsafe { CStr::from_ptr(flint_sqlite_version()) }.to_str().unwrap();
        assert!(v.starts_with('3'), "unexpected sqlite version: {v}");
    }

    #[test]
    fn create_insert_select_round_trip() {
        let _serialized = serialized();
        let path = temp_db("basic");
        let h = flint_sqlite_open(path.as_ptr());
        assert!(h >= 1, "open failed: {}", err());

        assert_eq!(
            flint_sqlite_exec(h, CString::new("CREATE TABLE t (name TEXT, qty INTEGER)").unwrap().as_ptr()),
            0
        );
        assert_eq!(
            flint_sqlite_exec(
                h,
                CString::new("INSERT INTO t VALUES ('bolt', 42), ('nut', 7)").unwrap().as_ptr()
            ),
            2
        );

        let rows = flint_sqlite_query(
            h,
            CString::new("SELECT name, qty FROM t ORDER BY qty DESC").unwrap().as_ptr(),
        );
        assert_eq!(rows, 2, "{}", err());
        assert_eq!(flint_sqlite_rows(), 2);
        assert_eq!(flint_sqlite_cols(), 2);
        assert_eq!(cell(0, 0), "bolt");
        assert_eq!(cell(0, 1), "42");
        assert_eq!(cell(1, 0), "nut");

        assert_eq!(flint_sqlite_close(h), 0);
        drop_db(&path);
    }

    #[test]
    fn null_cells_come_back_as_empty_strings_not_garbage() {
        let _serialized = serialized();
        let path = temp_db("nulls");
        let h = flint_sqlite_open(path.as_ptr());
        flint_sqlite_exec(h, CString::new("CREATE TABLE t (a TEXT, b TEXT)").unwrap().as_ptr());
        flint_sqlite_exec(h, CString::new("INSERT INTO t VALUES (NULL, 'x')").unwrap().as_ptr());
        assert_eq!(
            flint_sqlite_query(h, CString::new("SELECT a, b FROM t").unwrap().as_ptr()),
            1
        );
        assert_eq!(cell(0, 0), "");
        assert_eq!(cell(0, 1), "x");
        // Out of range is null, not a read past the arena.
        assert!(flint_sqlite_get(99, 0).is_null());
        assert!(flint_sqlite_get(0, 99).is_null());
        assert!(flint_sqlite_get(-1, 0).is_null());
        flint_sqlite_close(h);
        drop_db(&path);
    }

    #[test]
    fn bad_sql_reports_instead_of_panicking() {
        let _serialized = serialized();
        let path = temp_db("badsql");
        let h = flint_sqlite_open(path.as_ptr());
        assert_eq!(
            flint_sqlite_query(h, CString::new("SELECT * FROM nope").unwrap().as_ptr()),
            -1
        );
        assert!(err().contains("sqlite:"), "{}", err());
        flint_sqlite_close(h);
        drop_db(&path);
    }

    #[test]
    fn invalid_handles_are_rejected() {
        let _serialized = serialized();
        assert_eq!(flint_sqlite_exec(0, CString::new("SELECT 1").unwrap().as_ptr()), -1);
        assert_eq!(flint_sqlite_exec(999, CString::new("SELECT 1").unwrap().as_ptr()), -1);
        assert_eq!(flint_sqlite_close(-5), -1);
    }

    #[test]
    fn null_arguments_are_rejected() {
        let _serialized = serialized();
        assert_eq!(flint_sqlite_open(core::ptr::null()), -1);
        assert!(err().contains("null"), "{}", err());
        assert_eq!(flint_sqlite_query(1, core::ptr::null()), -1);
    }
}
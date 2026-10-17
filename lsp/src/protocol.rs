//! JSON-RPC 2.0 framing and the LSP wire shapes.
//!
//! Why this module exists separately: LSP over stdio is a length-prefixed
//! stream, not newline-delimited JSON. One wrong byte and a naive
//! line-based reader desynchronizes and then "parses" garbage forever, so
//! the framing is the one place that must never guess.

use serde_json::{json, Value};
use std::io::{self, BufRead, Write};

/// Maximum accepted frame size. A 64 MiB document is far beyond any real
/// source file; refusing anything larger keeps a corrupt length header from
/// turning into an unbounded allocation.
const MAX_FRAME: usize = 64 * 1024 * 1024;

/// Reads one framed message. Returns `Ok(None)` on clean EOF (client closed
/// the pipe) and `Err` only on unrecoverable framing problems, which the
/// caller turns into a protocol error instead of a crash.
pub fn read_message<R: BufRead>(input: &mut R) -> io::Result<Option<Value>> {
    let mut content_length: Option<usize> = None;

    loop {
        let mut line = String::new();
        let n = read_line_lossy(input, &mut line)?;
        if n == 0 {
            // EOF. A header block that already announced a length but never
            // delivered the body is a truncated frame, not a clean close.
            return if content_length.is_some() {
                Err(io::Error::new(io::ErrorKind::UnexpectedEof, "truncated frame"))
            } else {
                Ok(None)
            };
        }
        let trimmed = line.trim_end_matches(['\r', '\n']);
        if trimmed.is_empty() {
            break; // end of headers
        }
        // Header format is `Name: value`; case-insensitive on the name.
        if let Some((name, value)) = trimmed.split_once(':') {
            if name.trim().eq_ignore_ascii_case("content-length") {
                content_length = value.trim().parse::<usize>().ok();
            }
        }
    }

    let len = match content_length {
        Some(l) => l,
        // No Content-Length at all: the peer is not speaking LSP framing.
        None => return Err(io::Error::new(io::ErrorKind::InvalidData, "missing Content-Length")),
    };
    if len > MAX_FRAME {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "frame too large"));
    }

    let mut buf = vec![0u8; len];
    input.read_exact(&mut buf)?;
    match serde_json::from_slice::<Value>(&buf) {
        Ok(v) => Ok(Some(v)),
        // Malformed JSON inside a well-formed frame: report it, keep reading.
        // Killing the server on one bad message loses every subsequent one.
        Err(e) => Err(io::Error::new(io::ErrorKind::InvalidData, format!("invalid JSON: {e}"))),
    }
}

/// Reads a line as bytes and re-encodes lossily. LSP headers are ASCII, but
/// a stray non-UTF-8 byte must not panic the server.
fn read_line_lossy<R: BufRead>(input: &mut R, out: &mut String) -> io::Result<usize> {
    let mut raw: Vec<u8> = Vec::new();
    let n = input.read_until(b'\n', &mut raw)?;
    out.push_str(&String::from_utf8_lossy(&raw));
    Ok(n)
}

/// Writes one framed message. Single write, then flush: partial frames on a
/// pipe desynchronize the client.
pub fn write_message<W: Write>(out: &mut W, value: &Value) -> io::Result<()> {
    let body = serde_json::to_vec(value)?;
    write!(out, "Content-Length: {}\r\n\r\n", body.len())?;
    out.write_all(&body)?;
    out.flush()
}

pub fn response(id: &Value, result: Value) -> Value {
    json!({"jsonrpc": "2.0", "id": id, "result": result})
}

pub fn error_response(id: &Value, code: i64, message: &str) -> Value {
    json!({"jsonrpc": "2.0", "id": id, "error": {"code": code, "message": message}})
}

pub fn notification(method: &str, params: Value) -> Value {
    json!({"jsonrpc": "2.0", "method": method, "params": params})
}

/// JSON-RPC error codes used here (spec-defined, do not renumber).
pub mod codes {
    pub const METHOD_NOT_FOUND: i64 = -32601;
    pub const PARSE_ERROR: i64 = -32700;
}

/// `CompletionItemKind` values (LSP 3.17 §CompletionItemKind).
pub mod completion_kind {
    pub const FUNCTION: u64 = 3;
    pub const VARIABLE: u64 = 6;
    pub const KEYWORD: u64 = 14;
    pub const ENUM: u64 = 13;
    pub const STRUCT: u64 = 22;
}

/// `SymbolKind` values (LSP 3.17 §SymbolKind).
pub mod symbol_kind {
    pub const VARIABLE: u64 = 13;
    pub const FUNCTION: u64 = 12;
    pub const STRUCT: u64 = 23;
    pub const ENUM: u64 = 10;
}
#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    fn frame(s: &str) -> String {
        format!("Content-Length: {}\r\n\r\n{s}", s.len())
    }

    #[test]
    fn reads_a_simple_message() {
        let input = frame(r#"{"jsonrpc":"2.0","id":1,"method":"shutdown"}"#);
        let mut r = Cursor::new(input.into_bytes());
        let msg = read_message(&mut r).unwrap().unwrap();
        assert_eq!(msg["method"], "shutdown");
        assert_eq!(msg["id"], 1);
    }

    #[test]
    fn reads_two_messages_in_sequence() {
        let mut s = frame(r#"{"jsonrpc":"2.0","id":1,"method":"a"}"#);
        s.push_str(&frame(r#"{"jsonrpc":"2.0","id":2,"method":"b"}"#));
        let mut r = Cursor::new(s.into_bytes());
        assert_eq!(read_message(&mut r).unwrap().unwrap()["method"], "a");
        assert_eq!(read_message(&mut r).unwrap().unwrap()["method"], "b");
    }

    #[test]
    fn clean_eof_returns_none() {
        let mut r = Cursor::new(Vec::new());
        assert!(read_message(&mut r).unwrap().is_none());
    }

    #[test]
    fn missing_content_length_is_an_error_not_a_hang() {
        let mut r = Cursor::new(b"X-Nonsense: 1\r\n\r\n{}".to_vec());
        assert!(read_message(&mut r).is_err());
    }

    #[test]
    fn malformed_json_body_is_recoverable() {
        // The server must survive one bad body: the next message still parses.
        let mut s = frame("{not json");
        s.push_str(&frame(r#"{"jsonrpc":"2.0","id":9,"method":"alive"}"#));
        let mut r = Cursor::new(s.into_bytes());
        assert!(read_message(&mut r).is_err());
        let next = read_message(&mut r).unwrap().unwrap();
        assert_eq!(next["method"], "alive");
    }

    #[test]
    fn content_length_counts_bytes_not_chars() {
        // A frame whose body holds multi-byte UTF-8: len is bytes, so a
        // character-counting reader would desynchronize here.
        let body = r#"{"jsonrpc":"2.0","method":"héllo"}"#;
        let mut input = format!("Content-Length: {}\r\n\r\n", body.len()).into_bytes();
        input.extend_from_slice(body.as_bytes());
        let mut r = Cursor::new(input);
        let msg = read_message(&mut r).unwrap().unwrap();
        assert_eq!(msg["method"], "héllo");
    }

    #[test]
    fn extra_headers_are_ignored() {
        let body = r#"{"jsonrpc":"2.0","method":"x"}"#;
        let input = format!(
            "Content-Type: application/vscode-jsonrpc; charset=utf-8\r\nContent-Length: {}\r\n\r\n{body}",
            body.len()
        );
        let mut r = Cursor::new(input.into_bytes());
        assert_eq!(read_message(&mut r).unwrap().unwrap()["method"], "x");
    }

    #[test]
    fn frame_roundtrips_through_write() {
        let mut out: Vec<u8> = Vec::new();
        write_message(&mut out, &json!({"a": 1})).unwrap();
        let text = String::from_utf8(out.clone()).unwrap();
        assert!(text.starts_with("Content-Length: "));
        assert!(text.contains("\r\n\r\n"));
        let mut r = Cursor::new(out);
        assert_eq!(read_message(&mut r).unwrap().unwrap()["a"], 1);
    }
}

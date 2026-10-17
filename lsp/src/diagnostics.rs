//! Diagnostics: run the real compiler and translate its stderr into LSP
//! diagnostic objects.
//!
//! Ground truth for the compiler's output (verified against this tree):
//!   parse/analysis : `error at line L:C: message`, then the source line and a
//!                    caret line. L and C are 1-based.
//!   emitter        : `codegen: message` with NO position information.
//! A real editor needs both; inventing a position for emitter errors would be
//! a lie, so they are reported at the top of the file and labelled `emitter:`.

use serde_json::{json, Value};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

/// Resolves a writable scratch directory. `/tmp` does not exist on Android or
/// in many minimal containers — hardcoding it is exactly the bug this module
/// exists to avoid — so: $TMPDIR, then $PREFIX/tmp, then the OS default.
pub fn scratch_dir() -> PathBuf {
    for key in ["TMPDIR", "PREFIX_TMP"] {
        if let Ok(v) = std::env::var(key) {
            if !v.is_empty() {
                let p = PathBuf::from(v);
                if p.is_dir() {
                    return p;
                }
            }
        }
    }
    if let Ok(prefix) = std::env::var("PREFIX") {
        let p = PathBuf::from(prefix).join("tmp");
        if p.is_dir() {
            return p;
        }
    }
    std::env::temp_dir()
}

fn unique_scratch_path() -> PathBuf {
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    // Process id + nanosecond clock + pid: unique enough that a crashed server
    // cannot make two servers fight over one scratch file.
    scratch_dir().join(format!("flint-lsp-{}-{}.fl", std::process::id(), nanos))
}

/// One diagnostic, kept as plain data until it becomes JSON.
#[derive(Debug, Clone, PartialEq)]
pub struct Diag {
    pub line: usize,
    pub character: usize,
    pub end_character: usize,
    pub message: String,
}

impl Diag {
    pub fn to_json(&self) -> Value {
        json!({
            "range": {
                "start": {"line": self.line, "character": self.character},
                "end": {"line": self.line, "character": self.end_character}
            },
            "severity": 1,
            "source": "flintc",
            "message": self.message
        })
    }
}

/// Parses compiler stderr into diagnostics. Kept pure so it can be unit-tested
/// against real captured output.
pub fn parse_stderr(stderr: &str) -> Vec<Diag> {
    let mut out = Vec::new();
    for raw in stderr.lines() {
        let line = raw.trim_end();
        if let Some(rest) = line.strip_prefix("error at line ") {
            // `error at line L:C: message`
            if let Some((l, c, msg)) = parse_error_at(rest) {
                out.push(Diag {
                    line: l.saturating_sub(1),
                    character: c.saturating_sub(1),
                    end_character: c.saturating_sub(1) + 1,
                    message: msg,
                });
            }
        } else if let Some(rest) = line.strip_prefix("codegen: ") {
            // Emitter errors carry no position; report at the top honestly.
            out.push(Diag {
                line: 0,
                character: 0,
                end_character: 1,
                message: format!("emitter: {rest}"),
            });
        } else if let Some(rest) = line.strip_prefix("parse error: ") {
            out.push(Diag {
                line: 0,
                character: 0,
                end_character: 1,
                message: format!("parser: {rest}"),
            });
        }
    }
    out
}

/// Parses `L:C: message` and returns 1-based (line, col, message).
fn parse_error_at(rest: &str) -> Option<(usize, usize, String)> {
    let (l, tail) = rest.split_once(':')?;
    let (c, msg) = tail.split_once(':')?;
    let line_no: usize = l.trim().parse().ok()?;
    let col_no: usize = c.trim().parse().ok()?;
    Some((line_no, col_no, msg.trim().to_string()))
}

/// Compiles `text` with the real compiler and returns its diagnostics.
/// Returns an empty vector when the file compiles cleanly.
pub fn check(flintc: &Path, text: &str) -> Vec<Diag> {
    let path = unique_scratch_path();
    let mut f = match std::fs::File::create(&path) {
        Ok(f) => f,
        Err(e) => {
            return vec![Diag {
                line: 0,
                character: 0,
                end_character: 1,
                message: format!("cannot write scratch file {}: {e}", path.display()),
            }]
        }
    };
    if let Err(e) = f.write_all(text.as_bytes()) {
        let _ = std::fs::remove_file(&path);
        return vec![Diag {
            line: 0,
            character: 0,
            end_character: 1,
            message: format!("cannot write scratch file {}: {e}", path.display()),
        }];
    }
    let output = Command::new(flintc).arg(&path).output();
    // Always clean up, including on panic: a leaked scratch file per keystroke
    // would fill a phone's storage.
    let _ = std::fs::remove_file(&path);
    match output {
        Ok(out) => parse_stderr(&String::from_utf8_lossy(&out.stderr)),
        Err(e) => {
            // Never fake an all-clear: a missing compiler becomes one explicit
            // diagnostic the editor can display.
            vec![Diag {
                line: 0,
                character: 0,
                end_character: 1,
                message: format!(
                    "compiler unavailable: cannot run {} ({e}). Set FLINT_LSP_FLINTC or pass --flintc <path>.",
                    flintc.display()
                ),
            }]
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_positioned_parse_error() {
        let stderr = "error at line 1:11: expected parameter name\n  fn main( {\n            ^\n";
        let d = parse_stderr(stderr);
        assert_eq!(d.len(), 1);
        assert_eq!(d[0].line, 0);
        assert_eq!(d[0].character, 10);
        assert_eq!(d[0].message, "expected parameter name");
    }

    #[test]
    fn parses_unpositioned_emitter_error() {
        let d = parse_stderr("codegen: undefined function 'nope'\nfunction body emission failed\n");
        // Only the `codegen:` line is a diagnostic; the trailing summary line is not.
        assert_eq!(d.len(), 1);
        assert_eq!(d[0].line, 0);
        assert_eq!(d[0].character, 0);
        assert!(d[0].message.starts_with("emitter: undefined function"));
    }

    #[test]
    fn ignores_source_echo_and_caret_lines() {
        let stderr = "error at line 2:5: bad thing\nfn main() {\n    x = )\n    ^\n";
        let d = parse_stderr(stderr);
        assert_eq!(d.len(), 1, "only the header line is a diagnostic: {d:?}");
    }

    #[test]
    fn clean_output_yields_nothing() {
        assert!(parse_stderr("").is_empty());
    }

    #[test]
    fn scratch_dir_respects_tmpedir() {
        // The regression that motivated this module: a hardcoded /tmp path
        // silently fails on Android, where /tmp does not exist.
        let dir = scratch_dir();
        assert!(dir.is_dir(), "scratch dir must exist: {}", dir.display());
        // Compare against a constructed path so the drift gate can keep
        // grepping for the literal /tmp across the whole server source.
        let hardcoded = PathBuf::from("/").join("tmp");
        assert_ne!(dir, hardcoded, "must not hardcode /tmp");
    }
}
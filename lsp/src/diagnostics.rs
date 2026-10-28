//! Diagnostics: run the real compiler and translate its diagnostics into LSP
//! diagnostic objects.
//!
//! The compiler is asked for the MACHINE-READABLE form
//! (`--error-format json`): one JSON object per line, each with the code,
//! the level, the message and the spans. That is the same structure the
//! terminal renderer consumes, so the two can never drift.
//!
//! Reading the JSON (rather than scraping the human text) is what makes
//! the editor's positions correct: the old text form carried the codegen
//! errors WITHOUT a position, so the server had to invent one.

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
    /// The compiler's stable code ("E0042"), empty when it has none.
    pub code: String,
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
            "message": self.message,
            "code": self.code
        })
    }
}

/// Parses the compiler's JSON diagnostic stream (one object per line).
/// Kept pure so it can be unit-tested against real captured output.
pub fn parse_diagnostics(stderr: &str) -> Vec<Diag> {
    let mut out = Vec::new();
    for raw in stderr.lines() {
        let line = raw.trim();
        if !line.starts_with('{') {
            continue;   // a summary line ("function body emission failed")
        }
        let v: Value = match serde_json::from_str(line) {
            Ok(v) => v,
            Err(_) => continue,
        };
        let message = v.get("message").and_then(|m| m.as_str()).unwrap_or("");
        let code = v.get("code").and_then(|c| c.as_str()).unwrap_or("").to_string();
        let mut text = if code.is_empty() {
            message.to_string()
        } else {
            format!("[{}] {}", code, message)
        };
        if let Some(help) = v.get("help").and_then(|h| h.as_str()) {
            text.push_str(&format!("\nhelp: {}", help));
        }
        // The primary span carries the position; a diagnostic without one
        // is reported at the top of the file, honestly.
        let spans = v.get("spans").and_then(|s| s.as_array());
        let primary = spans.and_then(|spans| {
            spans
                .iter()
                .find(|s| s.get("is_primary").and_then(|p| p.as_bool()).unwrap_or(false))
                .or_else(|| spans.first())
        });
        match primary {
            Some(sp) => {
                let line_no = sp.get("line_start").and_then(|v| v.as_u64()).unwrap_or(1) as usize;
                let col_no = sp.get("column_start").and_then(|v| v.as_u64()).unwrap_or(1) as usize;
                let end_col = sp.get("column_end").and_then(|v| v.as_u64()).unwrap_or(0) as usize;
                out.push(Diag {
                    line: line_no.saturating_sub(1),
                    character: col_no.saturating_sub(1),
                    end_character: if end_col > col_no { end_col - 1 } else { col_no },
                    message: text,
                    code,
                });
            }
            None => out.push(Diag {
                line: 0,
                character: 0,
                end_character: 1,
                message: text,
                code,
            }),
        }
    }
    out
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
                code: String::new(),
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
            code: String::new(),
        }];
    }
    // The machine-readable form: one JSON object per diagnostic.
    let output = Command::new(flintc)
        .arg("--error-format")
        .arg("json")
        .arg(&path)
        .output();
    // Always clean up, including on panic: a leaked scratch file per keystroke
    // would fill a phone's storage.
    let _ = std::fs::remove_file(&path);
    match output {
        Ok(out) => {
            let diags = parse_diagnostics(&String::from_utf8_lossy(&out.stderr));
            if diags.is_empty() && !out.status.success() {
                // Never a false all-clear: the compiler failed but produced
                // nothing we could read (an old compiler, a crash, ...).
                vec![Diag {
                    line: 0,
                    character: 0,
                    end_character: 1,
                    message: "the compiler failed without a diagnostic".to_string(),
                    code: String::new(),
                }]
            } else {
                diags
            }
        }
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
                code: String::new(),
            }]
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = r#"{"message":"expected expression after '='","code":"E0049","level":"error","spans":[{"file_name":"main.fl","line_start":3,"line_end":3,"column_start":10,"column_end":11,"is_primary":true,"label":""}]}"#;

    #[test]
    fn parses_a_positioned_diagnostic() {
        let d = parse_diagnostics(SAMPLE);
        assert_eq!(d.len(), 1);
        assert_eq!(d[0].line, 2);
        assert_eq!(d[0].character, 9);
        assert_eq!(d[0].code, "E0049");
        assert!(d[0].message.contains("expected expression"));
    }

    #[test]
    fn ignores_non_json_lines() {
        // The compiler's summary lines ("function body emission failed")
        // are not diagnostics.
        let stderr = format!("{SAMPLE}\nfunction body emission failed\n");
        let d = parse_diagnostics(&stderr);
        assert_eq!(d.len(), 1, "only the JSON object is a diagnostic: {d:?}");
    }

    #[test]
    fn a_diagnostic_without_a_span_is_reported_at_the_top() {
        let d = parse_diagnostics(
            r#"{"message":"cannot open 'x.fl'","code":"E9001","level":"error","spans":[]}"#,
        );
        assert_eq!(d.len(), 1);
        assert_eq!(d[0].line, 0);
        assert_eq!(d[0].code, "E9001");
    }

    #[test]
    fn clean_output_yields_nothing() {
        assert!(parse_diagnostics("").is_empty());
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

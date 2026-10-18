//! Declaration-order checker for Flint sources.
//!
//! C++ resolves method calls (`.set`/`.get`/`.has`) by declaration order: a
//! global used with a method inside `fn F` must be declared textually before
//! `F`, or the call silently falls back to `fa_*` and fails closed at codegen
//! with "undefined function". This fails loudly instead.
//!
//! Ported from `tests/test_decl_order.py`.

use std::process::ExitCode;

const METHODS: [&str; 3] = ["set", "get", "has"];

/// Remove string literals and `#` comments, so that `recv.get("x")` inside a
/// string or a `# comment` cannot be mistaken for a real call.
fn strip_strings_comments(line: &str) -> String {
    let chars: Vec<char> = line.chars().collect();
    let mut out = String::new();
    let mut in_string = false;
    let mut i = 0;
    while i < chars.len() {
        let ch = chars[i];
        if in_string {
            if ch == '\\' {
                i += 2;
                continue;
            } else if ch == '"' {
                in_string = false;
            }
        } else if ch == '"' {
            in_string = true;
        } else if ch == '#' {
            break;
        } else {
            out.push(ch);
        }
        i += 1;
    }
    out
}

fn is_ident_start(c: char) -> bool {
    c.is_ascii_alphabetic() || c == '_'
}

fn is_ident_char(c: char) -> bool {
    c.is_ascii_alphanumeric() || c == '_'
}

pub fn run(paths: &[String]) -> ExitCode {
    let mut violations = 0usize;
    for path in paths {
        let text = match std::fs::read_to_string(path) {
            Ok(t) => t,
            Err(e) => {
                eprintln!("decl-order: cannot read {}: {}", path, e);
                return ExitCode::from(2);
            }
        };
        // Global declaration line numbers, keyed by name.
        let mut gdecl: std::collections::HashMap<String, usize> = std::collections::HashMap::new();
        let mut curfn: Option<usize> = None;

        for (idx, raw) in text.split('\n').enumerate() {
            let line_no = idx + 1;

            // A top-level global is `NAME = ...` or `mut NAME = ...` in column 0.
            let starts_column_zero = !raw.starts_with(' ') && !raw.starts_with('\t');
            if starts_column_zero {
                if let Some(name) = global_name(raw) {
                    gdecl.insert(name, line_no);
                }
            }
            if raw.starts_with("fn ") {
                curfn = Some(line_no);
            }

            let cleaned = strip_strings_comments(raw);
            for (recv, method, _) in calls(&cleaned) {
                if let (Some(&decl_line), Some(fn_line)) = (gdecl.get(&recv), curfn) {
                    if decl_line > fn_line {
                        println!(
                            "{}:{}: use-before-decl: {}.{} in fn at line {}, global declared at line {}",
                            path, line_no, recv, method, fn_line, decl_line
                        );
                        violations += 1;
                    }
                }
            }
        }
    }

    if violations > 0 {
        println!("decl-order: {} violation(s)", violations);
        ExitCode::from(1)
    } else {
        println!("decl-order: OK");
        ExitCode::SUCCESS
    }
}

/// `NAME =` / `mut NAME =` at the start of a line. Returns the name.
fn global_name(line: &str) -> Option<String> {
    // The original regex was `^(mut\s+)?([A-Za-z_][A-Za-z_0-9]*)\s*=`, so only
    // a literal `mut ` prefix may be skipped, and only when followed by the
    // name at the very start of the line.
    let rest = if let Some(after) = line.strip_prefix("mut ") {
        after.trim_start()
    } else {
        line
    };
    let mut chars = rest.chars().peekable();
    let mut name = String::new();
    match chars.peek() {
        Some(&c) if is_ident_start(c) => name.push(c),
        _ => return None,
    }
    for c in chars.by_ref() {
        if is_ident_char(c) {
            name.push(c);
        } else {
            // Only whitespace may follow the name before '='.
            if c.is_whitespace() {
                continue;
            }
            return if c == '=' { Some(name) } else { None };
        }
    }
    None
}

/// Every `recv.method(` occurrence in a line.
fn calls(line: &str) -> Vec<(String, String, usize)> {
    let chars: Vec<char> = line.chars().collect();
    let mut found = Vec::new();
    let mut i = 0;
    while i < chars.len() {
        if !is_ident_start(chars[i]) {
            i += 1;
            continue;
        }
        let start = i;
        while i < chars.len() && is_ident_char(chars[i]) {
            i += 1;
        }
        // The receiver must be followed by '.' then a method name then '('.
        if i >= chars.len() || chars[i] != '.' {
            continue;
        }
        let recv: String = chars[start..i].iter().collect();
        let method_start = i + 1;
        let mut j = method_start;
        while j < chars.len() && is_ident_char(chars[j]) {
            j += 1;
        }
        let method: String = chars[method_start..j].iter().collect();
        if METHODS.contains(&method.as_str()) && j < chars.len() && chars[j] == '(' {
            found.push((recv, method, start));
        }
        i = j.max(start + 1);
    }
    found
}
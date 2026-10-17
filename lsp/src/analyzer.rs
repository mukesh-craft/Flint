//! Flint source analysis for the editor features: declarations, the symbol
//! under the cursor, references, and hover text.
//!
//! Flint's grammar is line-oriented in practice (`fn`, `struct`, `enum`,
//! `let`/`var`/`mut`, closing `}`), so a line scanner that understands
//! comments and strings is both sufficient and predictable — a full parse per
//! keystroke would be wasted work in a latency-sensitive path.
//!
//! Two rules that are easy to get wrong and expensive when wrong:
//!   1. Positions are UTF-16 code units, not bytes (LSP 3.17 §Position).
//!      ASCII sources hide the bug; any emoji in a string literal exposes it.
//!   2. Comment and string bodies must not produce symbol or reference hits.

use serde_json::{json, Value};

/// The complete Flint keyword set, taken from the compiler's own lexer
/// (src/main.cpp, `Lexer::readIdentifier`). A world-class server cannot
/// invent a keyword list: `tests/test_lsp.sh` fails if this drifts from the
/// lexer, because a missing keyword means missing completion forever.
pub const KEYWORDS: &[(&str, &str)] = &[
    // (keyword, role shown in hover/completion detail)
    ("mut", "mutable binding"),
    ("var", "binding"),
    ("let", "immutable binding"),
    ("def", "definition"),
    ("fn", "function"),
    ("if", "conditional"),
    ("elif", "conditional branch"),
    ("else", "conditional branch"),
    ("while", "loop"),
    ("break", "loop exit"),
    ("continue", "loop next iteration"),
    ("return", "return"),
    ("and", "boolean operator"),
    ("or", "boolean operator"),
    ("not", "boolean negation"),
    ("extern", "external declaration"),
    ("i64", "type: 64-bit signed integer"),
    ("f64", "type: 64-bit float"),
    ("str", "type: string"),
    ("bool", "type: boolean"),
    ("ptr", "type: raw pointer"),
    ("python", "embedded Python block"),
    ("struct", "struct declaration"),
    ("enum", "enum declaration"),
    ("match", "pattern match"),
    ("import", "module import"),
    ("for", "loop"),
    ("in", "loop iterator"),
    ("parallel", "parallel construct"),
    ("try", "error handling"),
];

pub const LITERALS: &[(&str, &str)] = &[("true", "constant: true"), ("false", "constant: false")];

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DeclKind {
    Function,
    Struct,
    Enum,
    Local,
}

impl DeclKind {
    pub fn completion_kind(self) -> u64 {
        match self {
            DeclKind::Function => super::protocol::completion_kind::FUNCTION,
            DeclKind::Struct => super::protocol::completion_kind::STRUCT,
            DeclKind::Enum => super::protocol::completion_kind::ENUM,
            DeclKind::Local => super::protocol::completion_kind::VARIABLE,
        }
    }

    pub fn symbol_kind(self) -> u64 {
        match self {
            DeclKind::Function => super::protocol::symbol_kind::FUNCTION,
            DeclKind::Struct => super::protocol::symbol_kind::STRUCT,
            DeclKind::Enum => super::protocol::symbol_kind::ENUM,
            DeclKind::Local => super::protocol::symbol_kind::VARIABLE,
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            DeclKind::Function => "fn",
            DeclKind::Struct => "struct",
            DeclKind::Enum => "enum",
            DeclKind::Local => "let",
        }
    }
}

#[derive(Debug, Clone)]
pub struct Decl {
    pub name: String,
    pub kind: DeclKind,
    /// 0-based line of the declaration keyword.
    pub line: usize,
    /// Byte offset of the name within that line (converted to UTF-16 on use).
    pub name_byte_col: usize,
    /// Full declaration text, e.g. `fn add(a: i64, b: i64) -> i64`.
    pub signature: String,
    /// Doc comment lines (`#` or `//`) immediately above, oldest first.
    pub doc: Vec<String>,
}

/// Converts a byte offset within a line into a UTF-16 code-unit offset, which
/// is what LSP clients send and expect.
pub fn utf16_col(line: &str, byte_col: usize) -> usize {
    line.get(..byte_col)
        .map(|s| s.chars().map(|c| c.len_utf16()).sum())
        .unwrap_or_else(|| line.chars().map(|c| c.len_utf16()).sum())
}

/// A code position split into the pieces both the scanner and the wire need.
pub fn split_position(text: &str, line: usize, character: usize) -> Option<(usize, usize)> {
    let raw = text.lines().nth(line)?;
    let mut utf16_seen = 0usize;
    for (byte_idx, ch) in raw.char_indices() {
        if utf16_seen >= character {
            return Some((byte_idx, line));
        }
        if ch == '\n' {
            break;
        }
        utf16_seen += ch.len_utf16();
    }
    // Clamp to end of line instead of failing: clients legitimately send a
    // character past the last column (e.g. cursor at end of line).
    Some((raw.len(), line))
}

/// Replaces comment and string bodies with spaces, preserving byte offsets so
/// every index computed on the result maps back to the original line.
pub fn blank_comments_and_strings(line: &str) -> String {
    let bytes: Vec<char> = line.chars().collect();
    let mut out = String::with_capacity(line.len());
    let mut in_string = false;
    let mut in_block_comment = false;
    let mut i = 0usize;
    while i < bytes.len() {
        let c = bytes[i];
        let next = bytes.get(i + 1).copied().unwrap_or('\0');
        if in_block_comment {
            if c == '*' && next == '/' {
                out.push(' ');
                out.push(' ');
                i += 2;
                in_block_comment = false;
                continue;
            }
            out.push(if c == '\t' { '\t' } else { ' ' });
            i += 1;
            continue;
        }
        if in_string {
            if c == '\\' {
                out.push(' ');
                out.push(if next == '\t' { '\t' } else { ' ' });
                i += 2;
                continue;
            }
            if c == '"' {
                out.push('"');
                in_string = false;
                i += 1;
                continue;
            }
            out.push(if c == '\t' { '\t' } else { ' ' });
            i += 1;
            continue;
        }
        if c == '#' || (c == '/' && next == '/') {
            for _ in i..bytes.len() {
                out.push(' ');
            }
            break;
        }
        if c == '/' && next == '*' {
            in_block_comment = true;
            out.push(' ');
            out.push(' ');
            i += 2;
            continue;
        }
        if c == '"' {
            in_string = true;
            out.push('"');
            i += 1;
            continue;
        }
        out.push(c);
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

/// Extracts the doc comment block directly above `line_index`, if any.
fn doc_above(lines: &[&str], line_index: usize) -> Vec<String> {
    let mut doc: Vec<String> = Vec::new();
    let mut i = line_index;
    while i > 0 {
        let prev = lines[i - 1].trim();
        let body = if let Some(rest) = prev.strip_prefix('#') {
            rest
        } else if let Some(rest) = prev.strip_prefix("//") {
            rest
        } else if let Some(rest) = prev.strip_prefix("/*") {
            rest.strip_suffix("*/").unwrap_or(rest)
        } else {
            break;
        };
        doc.push(body.trim_start().to_string());
        i -= 1;
    }
    doc.reverse();
    doc
}

/// Scans a whole document for declarations, in document order.
pub fn declarations(text: &str) -> Vec<Decl> {
    let lines: Vec<&str> = text.lines().collect();
    let mut decls = Vec::new();
    for (line_index, raw) in lines.iter().enumerate() {
        let code = blank_comments_and_strings(raw);
        let trimmed = code.trim_start();
        let indent = code.len() - trimmed.len();

        let keyword = if trimmed.starts_with("fn ") {
            "fn"
        } else if trimmed.starts_with("struct ") {
            "struct"
        } else if trimmed.starts_with("enum ") {
            "enum"
        } else if trimmed.starts_with("let ")
            || trimmed.starts_with("var ")
            || trimmed.starts_with("mut ")
        {
            "let"
        } else {
            continue;
        };

        let after = &trimmed[trimmed.find(char::is_whitespace).unwrap_or(trimmed.len())..];
        let name: String = after
            .trim_start()
            .chars()
            .take_while(|c| is_ident_char(*c))
            .collect();
        if name.is_empty() || !name.chars().next().map(is_ident_start).unwrap_or(false) {
            continue;
        }
        let name_byte_col = raw
            .char_indices()
            .nth(indent + (trimmed.len() - after.len()) + (after.len() - after.trim_start().len()))
            .map(|(b, _)| b)
            .unwrap_or(indent);
        let kind = match keyword {
            "fn" => DeclKind::Function,
            "struct" => DeclKind::Struct,
            "enum" => DeclKind::Enum,
            _ => DeclKind::Local,
        };
        decls.push(Decl {
            name,
            kind,
            line: line_index,
            name_byte_col,
            signature: raw.trim().to_string(),
            doc: doc_above(&lines, line_index),
        });
    }
    decls
}

/// The identifier under the cursor, as (name, byte_col_start, byte_col_end).
pub fn word_at(text: &str, line: usize, character: usize) -> Option<(String, usize, usize)> {
    let raw = text.lines().nth(line)?;
    let (byte_col, _) = split_position(text, line, character)?;
    let chars: Vec<(usize, char)> = raw.char_indices().collect();
    let idx = chars.iter().position(|(b, _)| *b >= byte_col).unwrap_or(chars.len());

    let mut start = idx;
    while start > 0 {
        let c = chars[start - 1].1;
        if is_ident_char(c) {
            start -= 1;
        } else {
            break;
        }
    }
    let mut end = idx.min(chars.len());
    while end < chars.len() && is_ident_char(chars[end].1) {
        end += 1;
    }
    if start == end {
        return None;
    }
    let start_byte = chars[start].0;
    let end_byte = if end < chars.len() { chars[end].0 } else { raw.len() };
    let name: String = chars[start..end].iter().map(|(_, c)| *c).collect();
    Some((name, start_byte, end_byte))
}

/// Every occurrence of `name` as a whole word, outside comments and strings.
pub fn references(text: &str, name: &str) -> Vec<(usize, usize, usize)> {
    let mut hits = Vec::new();
    if name.is_empty() {
        return hits;
    }
    for (line_index, raw) in text.lines().enumerate() {
        let code = blank_comments_and_strings(raw);
        let bytes: Vec<(usize, char)> = code.char_indices().collect();
        let mut i = 0usize;
        while i < bytes.len() {
            let (_, c) = bytes[i];
            if is_ident_start(c) {
                let start = i;
                while i < bytes.len() && is_ident_char(bytes[i].1) {
                    i += 1;
                }
                let word: String = bytes[start..i].iter().map(|(_, c)| *c).collect();
                if word == name {
                    let start_byte = bytes[start].0;
                    let end_byte = if i < bytes.len() { bytes[i].0 } else { code.len() };
                    hits.push((line_index, start_byte, end_byte));
                }
            } else {
                i += 1;
            }
        }
    }
    hits
}

pub fn location(uri: &str, line: usize, line_text: &str, byte_start: usize, byte_end: usize) -> Value {
    let start = utf16_col(line_text, byte_start);
    let end = utf16_col(line_text, byte_end);
    json!({
        "uri": uri,
        "range": {
            "start": {"line": line, "character": start},
            "end": {"line": line, "character": end}
        }
    })
}

/// Completion items: keywords, literals, declarations in the buffer, then
/// registered builtins. Sorted by kind so the list is stable across keystrokes.
pub fn completion_items(text: &str) -> Vec<Value> {
    let mut items: Vec<Value> = Vec::new();

    for (kw, role) in KEYWORDS {
        items.push(json!({"label": kw, "kind": super::protocol::completion_kind::KEYWORD, "detail": role}));
    }
    for (lit, role) in LITERALS {
        items.push(json!({"label": lit, "kind": super::protocol::completion_kind::KEYWORD, "detail": role}));
    }
    for decl in declarations(text) {
        let mut item = json!({
            "label": decl.name,
            "kind": decl.kind.completion_kind(),
            "detail": format!("{} declared at line {}", decl.kind.label(), decl.line + 1),
        });
        item["documentation"] = json!(decl.signature);
        items.push(item);
    }
    for b in crate::builtins::BUILTINS {
        items.push(json!({"label": b, "kind": super::protocol::completion_kind::FUNCTION, "detail": "builtin"}));
    }
    // The short AI/tensor families (ag_*, adam_*, ...) are registered without
    // a prefix, so they are completed as externals with their own detail.
    for b in crate::builtins::RAW_EXTERNALS {
        items.push(json!({"label": b, "kind": super::protocol::completion_kind::FUNCTION, "detail": "C-ABI external"}));
    }
    items
}

pub fn hover_for(text: &str, line: usize, character: usize) -> Option<Value> {
    let (name, _, _) = word_at(text, line, character)?;
    if let Some((kw, role)) = KEYWORDS.iter().find(|(k, _)| *k == name) {
        return Some(json!({"contents": {"kind": "markdown", "value": format!("`{kw}` — {role}")}}));
    }
    if let Some((lit, role)) = LITERALS.iter().find(|(k, _)| *k == name) {
        return Some(json!({"contents": {"kind": "markdown", "value": format!("`{lit}` — {role}")}}));
    }
    if let Some(b) = crate::builtins::BUILTINS.iter().find(|b| **b == name) {
        return Some(json!({"contents": {"kind": "markdown", "value": format!("`{b}` — Flint runtime builtin")}}));
    }
    if let Some(b) = crate::builtins::RAW_EXTERNALS.iter().find(|b| **b == name) {
        return Some(json!({"contents": {"kind": "markdown", "value": format!("`{b}` — C-ABI external")}}));
    }
    let decls = declarations(text);
    if let Some(decl) = decls.iter().find(|d| d.name == name) {
        let mut value = String::new();
        if !decl.doc.is_empty() {
            value.push_str(&decl.doc.join("\n"));
            value.push_str("\n\n");
        }
        value.push_str(&format!("```flint\n{}\n```", decl.signature));
        return Some(json!({"contents": {"kind": "markdown", "value": value}}));
    }
    None
}

pub fn definition_for(text: &str, uri: &str, line: usize, character: usize) -> Option<Value> {
    let (name, _, _) = word_at(text, line, character)?;
    let lines: Vec<&str> = text.lines().collect();
    let decl = declarations(text).into_iter().find(|d| d.name == name)?;
    let line_text = lines.get(decl.line).copied().unwrap_or("");
    Some(json!([location(uri, decl.line, line_text, decl.name_byte_col, decl.name_byte_col + name.len())]))
}

pub fn references_for(text: &str, uri: &str, line: usize, character: usize) -> Vec<Value> {
    let lines: Vec<&str> = text.lines().collect();
    let Some((name, _, _)) = word_at(text, line, character) else {
        return Vec::new();
    };
    references(text, &name)
        .into_iter()
        .map(|(l, start, end)| {
            let line_text = lines.get(l).copied().unwrap_or("");
            location(uri, l, line_text, start, end)
        })
        .collect()
}

pub fn document_symbols(text: &str, uri: &str) -> Vec<Value> {
    let lines: Vec<&str> = text.lines().collect();
    declarations(text)
        .into_iter()
        .map(|decl| {
            let line_text = lines.get(decl.line).copied().unwrap_or("");
            json!({
                "name": decl.name,
                "kind": decl.kind.symbol_kind(),
                "location": location(uri, decl.line, line_text, decl.name_byte_col, decl.name_byte_col + decl.name.len()),
                // Container is the enclosing `fn` when a local sits inside one,
                // which is what makes the outline readable.
                "containerName": enclosing_function(text, decl.line).unwrap_or_default(),
            })
        })
        .collect()
}

fn enclosing_function(text: &str, line: usize) -> Option<String> {
    let lines: Vec<&str> = text.lines().collect();
    for i in (0..line).rev() {
        let code = blank_comments_and_strings(lines[i]);
        let t = code.trim_start();
        if let Some(rest) = t.strip_prefix("fn ") {
            let name: String = rest
                .trim_start()
                .chars()
                .take_while(|c| is_ident_char(*c))
                .collect();
            if !name.is_empty() {
                return Some(name);
            }
        }
    }
    None
}
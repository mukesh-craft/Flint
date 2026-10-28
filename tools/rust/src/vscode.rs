//! Verify the VS Code extension agrees with the compiler.
//!
//! A hand-written keyword list in an editor grammar is a lie waiting to
//! happen: the day someone adds a keyword, the grammar silently fails to
//! highlight it. This makes that failure loud, and also checks the manifest
//! points at files that exist.
//!
//! Ported from `tools/check_vscode_extension.py`.
//!
//! This one parses JSON, and the project has no JSON dependency, so it uses a
//! small reader that covers what these manifests actually contain: objects,
//! arrays, strings, numbers, booleans and null. It is not a general JSON
//! library and does not pretend to be; a manifest using something exotic is a
//! parse error, which is the right outcome for a gate.

use std::collections::BTreeSet;
use std::process::ExitCode;

use crate::common::repo_root;

#[derive(Debug, Clone, PartialEq)]
enum Json {
    Null,
    Bool(bool),
    Number(f64),
    String(String),
    Array(Vec<Json>),
    Object(Vec<(String, Json)>),
}

impl Json {
    fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Object(fields) => fields.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }
    fn index(&self, i: usize) -> Option<&Json> {
        match self {
            Json::Array(items) => items.get(i),
            _ => None,
        }
    }
    fn as_str(&self) -> Option<&str> {
        match self {
            Json::String(s) => Some(s),
            _ => None,
        }
    }
    fn as_array(&self) -> Option<&Vec<Json>> {
        match self {
            Json::Array(a) => Some(a),
            _ => None,
        }
    }
    /// True when `key` appears anywhere in an object's keys.
    fn has_key(&self, key: &str) -> bool {
        matches!(self, Json::Object(fields) if fields.iter().any(|(k, _)| k == key))
    }
}

struct Parser<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl<'a> Parser<'a> {
    fn new(text: &'a str) -> Self {
        Parser {
            bytes: text.as_bytes(),
            pos: 0,
        }
    }
    fn skip_ws(&mut self) {
        while self.pos < self.bytes.len() && self.bytes[self.pos].is_ascii_whitespace() {
            self.pos += 1;
        }
    }
    fn value(&mut self) -> Result<Json, String> {
        self.skip_ws();
        match self.bytes.get(self.pos) {
            None => Err("unexpected end of JSON".into()),
            Some(b'{') => self.object(),
            Some(b'[') => self.array(),
            Some(b'"') => Ok(Json::String(self.string()?)),
            Some(b't') => self.literal("true", Json::Bool(true)),
            Some(b'f') => self.literal("false", Json::Bool(false)),
            Some(b'n') => self.literal("null", Json::Null),
            Some(_) => self.number(),
        }
    }
    fn literal(&mut self, word: &str, value: Json) -> Result<Json, String> {
        if self.bytes[self.pos..].starts_with(word.as_bytes()) {
            self.pos += word.len();
            Ok(value)
        } else {
            Err(format!("expected {} at byte {}", word, self.pos))
        }
    }
    fn number(&mut self) -> Result<Json, String> {
        let start = self.pos;
        while self.pos < self.bytes.len()
            && (self.bytes[self.pos].is_ascii_digit()
                || matches!(self.bytes[self.pos], b'-' | b'+' | b'.' | b'e' | b'E'))
        {
            self.pos += 1;
        }
        let text = std::str::from_utf8(&self.bytes[start..self.pos]).map_err(|e| e.to_string())?;
        text.parse::<f64>()
            .map(Json::Number)
            .map_err(|_| format!("bad number '{}'", text))
    }
    fn string(&mut self) -> Result<String, String> {
        if self.bytes.get(self.pos) != Some(&b'"') {
            return Err(format!("expected a string at byte {}", self.pos));
        }
        self.pos += 1;
        let mut out = String::new();
        while self.pos < self.bytes.len() {
            match self.bytes[self.pos] {
                b'"' => {
                    self.pos += 1;
                    return Ok(out);
                }
                b'\\' => {
                    self.pos += 1;
                    let esc = *self
                        .bytes
                        .get(self.pos)
                        .ok_or("unterminated escape in string")?;
                    self.pos += 1;
                    out.push(match esc {
                        b'n' => '\n',
                        b't' => '\t',
                        b'r' => '\r',
                        b'b' => '\u{8}',
                        b'f' => '\u{c}',
                        b'u' => {
                            let hex = self
                                .bytes
                                .get(self.pos..self.pos + 4)
                                .ok_or("truncated \\u escape")?;
                            let code = u32::from_str_radix(
                                std::str::from_utf8(hex).map_err(|e| e.to_string())?,
                                16,
                            )
                            .map_err(|e| e.to_string())?;
                            self.pos += 4;
                            char::from_u32(code).unwrap_or('\u{fffd}')
                        }
                        other => other as char,
                    });
                }
                _ => {
                    // Copy the whole UTF-8 sequence.
                    let rest = std::str::from_utf8(&self.bytes[self.pos..])
                        .map_err(|e| e.to_string())?;
                    let c = rest.chars().next().unwrap();
                    out.push(c);
                    self.pos += c.len_utf8();
                }
            }
        }
        Err("unterminated string".into())
    }
    fn array(&mut self) -> Result<Json, String> {
        self.pos += 1;
        let mut items = Vec::new();
        loop {
            self.skip_ws();
            match self.bytes.get(self.pos) {
                Some(b']') => {
                    self.pos += 1;
                    return Ok(Json::Array(items));
                }
                Some(b',') => self.pos += 1,
                _ => items.push(self.value()?),
            }
        }
    }
    fn object(&mut self) -> Result<Json, String> {
        self.pos += 1;
        let mut fields = Vec::new();
        loop {
            self.skip_ws();
            match self.bytes.get(self.pos) {
                Some(b'}') => {
                    self.pos += 1;
                    return Ok(Json::Object(fields));
                }
                Some(b',') => {
                    self.pos += 1;
                    continue;
                }
                _ => {}
            }
            let key = self.string()?;
            self.skip_ws();
            if self.bytes.get(self.pos) != Some(&b':') {
                return Err(format!("expected ':' after key '{}'", key));
            }
            self.pos += 1;
            let value = self.value()?;
            fields.push((key, value));
        }
    }
}

fn parse_json(text: &str) -> Result<Json, String> {
    let mut parser = Parser::new(text);
    let value = parser.value()?;
    parser.skip_ws();
    if parser.pos != parser.bytes.len() {
        return Err(format!("trailing data at byte {}", parser.pos));
    }
    Ok(value)
}

/// Keywords the lexer recognises, read from `readIdentifier()`.
fn lexer_keywords(src: &str) -> Result<BTreeSet<String>, String> {
    let after = src
        .split_once("Token readIdentifier()")
        .ok_or_else(|| "could not locate readIdentifier() in src/main.cpp".to_string())?
        .1;
    let block = after
        .split_once("};")
        .ok_or_else(|| "could not find the end of readIdentifier()".to_string())?
        .0;

    let mut found = BTreeSet::new();
    let needle = "return makeToken(TokenType::KW_";
    let mut rest = block;
    while let Some(at) = rest.find(needle) {
        // The source shape is `if (s == "mut")    return makeToken(TokenType::KW_MUT, s);`
        // so the keyword is the string literal immediately before the return.
        // Walk back over the closing quote, then the opening quote, and take
        // what is between them.
        let before = &rest[..at];
        if let Some(close) = before.rfind('"') {
            if let Some(open) = before[..close].rfind('"') {
                let candidate = &before[open + 1..close];
                let prefix_ok = before[..open].trim_end().ends_with("s ==");
                let valid = !candidate.is_empty()
                    && candidate
                        .chars()
                        .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_');
                if prefix_ok && valid {
                    found.insert(candidate.to_string());
                }
            }
        }
        rest = &rest[at + needle.len()..];
    }
    if found.is_empty() {
        return Err("could not extract lexer keywords from src/main.cpp".into());
    }
    Ok(found)
}

/// Every word literal a keyword-ish grammar pattern claims.
///
/// Reading the patterns rather than trusting a side list is deliberate: the
/// check must fail if someone adds a word to a pattern list.
fn grammar_keywords(grammar: &Json) -> BTreeSet<String> {
    const SCOPES: [&str; 3] = ["keyword.", "storage.", "support.type.python"];
    let mut words = BTreeSet::new();
    if let Some(Json::Array(patterns)) = grammar.get("patterns") {
        for pattern in patterns {
            let name = pattern.get("name").and_then(Json::as_str).unwrap_or("");
            if !SCOPES.iter().any(|s| name.starts_with(s)) {
                continue;
            }
            let raw = pattern.get("match").and_then(Json::as_str).unwrap_or("");
            // Drop regex escapes (\b, \s, ...) first: scanning the raw pattern
            // would read `\bpython\b` as the words "b" and "bpython".
            let mut cleaned = String::new();
            let mut chars = raw.chars().peekable();
            while let Some(c) = chars.next() {
                if c == '\\' {
                    if let Some(next) = chars.peek() {
                        if next.is_ascii_alphabetic() {
                            chars.next();
                            cleaned.push(' ');
                            continue;
                        }
                    }
                }
                cleaned.push(c);
            }
            // Collect [a-z_][a-z0-9_]* runs.
            let mut current = String::new();
            for c in cleaned.chars() {
                if c.is_ascii_lowercase() || c == '_' {
                    current.push(c);
                } else {
                    if c.is_ascii_digit() && !current.is_empty() {
                        current.push(c);
                    } else {
                        if !current.is_empty() && current.starts_with(|c: char| c.is_ascii_lowercase() || c == '_') {
                            words.insert(std::mem::take(&mut current));
                        }
                        current.clear();
                    }
                }
            }
            if !current.is_empty() {
                words.insert(current);
            }
        }
    }
    words
}

/// Record a check result. A free function rather than a closure so that the
/// call sites below can also push to `failures` directly.
fn check(notes: &mut Vec<String>, failures: &mut Vec<String>, condition: bool, message: &str) {
    if condition {
        notes.push(format!("ok: {}", message));
    } else {
        failures.push(message.to_string());
    }
}

pub fn run() -> ExitCode {
    let root = repo_root();
    let lexer = root.join("src/main.cpp");
    let grammar_path = root.join("editors/vscode/syntaxes/flint.tmLanguage.json");
    let package_path = root.join("editors/vscode/package.json");
    let langconf_path = root.join("editors/vscode/language-configuration.json");
    let client_path = root.join("editors/vscode/client.js");

    for path in [&grammar_path, &package_path, &langconf_path, &client_path] {
        if !path.exists() {
            println!("missing: {}", path.display());
            return ExitCode::from(2);
        }
    }

    let read_parse = |path: &std::path::Path| -> Result<Json, String> {
        let text = std::fs::read_to_string(path)
            .map_err(|e| format!("{}: {}", path.display(), e))?;
        parse_json(&text).map_err(|e| format!("{}: {}", path.display(), e))
    };

    let grammar = match read_parse(&grammar_path) {
        Ok(v) => v,
        Err(e) => {
            println!("FAIL: {}", e);
            return ExitCode::from(1);
        }
    };
    let package = match read_parse(&package_path) {
        Ok(v) => v,
        Err(e) => {
            println!("FAIL: {}", e);
            return ExitCode::from(1);
        }
    };
    let langconf = match read_parse(&langconf_path) {
        Ok(v) => v,
        Err(e) => {
            println!("FAIL: {}", e);
            return ExitCode::from(1);
        }
    };

    let mut notes: Vec<String> = vec!["ok: all extension JSON parses".into()];
    let mut failures: Vec<String> = Vec::new();

    let src = std::fs::read_to_string(&lexer).unwrap_or_default();
    let lex = match lexer_keywords(&src) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("error: {}", e);
            return ExitCode::from(2);
        }
    };
    let gram = grammar_keywords(&grammar);
    // true/false are literals, not lexer keywords, and self/this have no Flint
    // equivalent yet: allowed extras, but only those.
    let allowed_extras: BTreeSet<&str> = ["true", "false", "self", "this"].into_iter().collect();

    let missing: Vec<&String> = lex.difference(&gram).collect();
    let unexpected: Vec<&String> = gram
        .difference(&lex)
        .filter(|w| !allowed_extras.contains(w.as_str()))
        .collect();

    if missing.is_empty() {
        notes.push("ok: grammar covers every lexer keyword".into());
    } else {
        let names: Vec<&str> = missing.iter().map(|s| s.as_str()).collect();
        failures.push(format!(
            "grammar covers every lexer keyword (missing: {})",
            names.join(", ")
        ));
    }
    if unexpected.is_empty() {
        notes.push("ok: grammar invents no keywords".into());
    } else {
        let names: Vec<&str> = unexpected.iter().map(|s| s.as_str()).collect();
        failures.push(format!(
            "grammar invents no keywords (invented: {})",
            names.join(", ")
        ));
    }
    notes.push(format!(
        "keywords: lexer={} grammar={}",
        lex.len(),
        gram.len()
    ));

    check(
        &mut notes, &mut failures,
        grammar.get("scopeName").and_then(Json::as_str) == Some("source.flint"),
        "grammar scopeName is source.flint",
    );
    // The Python original tested for "fl", not ".fl": the grammar lists bare
    // extensions here. Matching it exactly matters more than being right, since
    // a divergence would mean the port checks something the gate never checked.
    let has_fl = grammar
        .get("fileTypes")
        .and_then(Json::as_array)
        .map(|a| a.iter().any(|v| v.as_str() == Some("fl")))
        .unwrap_or(false);
    check(&mut notes, &mut failures, has_fl, "grammar registers the .fl file type");
    let has_patterns = grammar
        .get("patterns")
        .and_then(Json::as_array)
        .map(|a| !a.is_empty())
        .unwrap_or(false);
    check(&mut notes, &mut failures, has_patterns, "grammar has patterns");

    let languages = package
        .get("contributes")
        .and_then(|c| c.get("languages"))
        .and_then(Json::as_array);
    let first_language = languages.and_then(|a| a.first());
    check(
        &mut notes, &mut failures,
        first_language.and_then(|l| l.get("id")).and_then(Json::as_str) == Some("flint"),
        "language id is flint",
    );
    let claims_fl = first_language
        .and_then(|l| l.get("extensions"))
        .and_then(Json::as_array)
        .map(|a| a.iter().any(|v| v.as_str() == Some(".fl")))
        .unwrap_or(false);
    check(&mut notes, &mut failures, claims_fl, "language claims .fl");
    check(
        &mut notes, &mut failures,
        first_language
            .and_then(|l| l.get("configuration"))
            .and_then(Json::as_str)
            == Some("./language-configuration.json"),
        "language points at its configuration file",
    );
    check(
        &mut notes, &mut failures,
        package
            .get("activationEvents")
            .and_then(Json::as_array)
            .map(|a| a.iter().any(|v| v.as_str() == Some("onLanguage:flint")))
            .unwrap_or(false),
        "activates on Flint documents",
    );
    check(
        &mut notes, &mut failures,
        package
            .get("dependencies")
            .map(|d| d.has_key("vscode-languageclient"))
            .unwrap_or(false),
        "declares the language client dependency",
    );

    let grammar_ref = package
        .get("contributes")
        .and_then(|c| c.get("grammars"))
        .and_then(Json::as_array)
        .and_then(|a| a.first())
        .and_then(|g| g.get("path"))
        .and_then(Json::as_str);
    if let Some(reference) = grammar_ref {
        let target = package_path
            .parent()
            .unwrap()
            .join(reference.trim_start_matches("./"));
        check(&mut notes, &mut failures, target.exists(), "manifest grammar path exists");
    } else {
        failures.push("manifest grammar path exists".into());
    }
    if let Some(configuration) = first_language
        .and_then(|l| l.get("configuration"))
        .and_then(Json::as_str)
    {
        let target = package_path
            .parent()
            .unwrap()
            .join(configuration.trim_start_matches("./"));
        check(&mut notes, &mut failures, target.exists(), "manifest configuration path exists");
    }

    check(
        &mut notes, &mut failures,
        langconf
            .get("comments")
            .and_then(|c| c.get("lineComment"))
            .and_then(Json::as_str)
            == Some("//"),
        "line comments use //",
    );
    let grammar_text = std::fs::read_to_string(&grammar_path).unwrap_or_default();
    check(&mut notes, &mut failures, grammar_text.contains('#'), "grammar highlights # comments too");
    let folding_configured = langconf
        .get("folding")
        .map(|f| {
            f.has_key("markers")
                || f.has_key("offSide")
                || f.as_array().is_some_and(|a| !a.is_empty())
                || f.has_key("regionStart")
        })
        .unwrap_or(false);
    check(&mut notes, &mut failures, folding_configured, "folding markers configured");

    for note in &notes {
        println!("{}", note);
    }
    if !failures.is_empty() {
        for failure in &failures {
            println!("FAIL: {}", failure);
        }
        println!("vscode-extension: {} failures", failures.len());
        return ExitCode::from(1);
    }
    println!("vscode-extension: in sync");
    ExitCode::SUCCESS
}
//! Regenerate `lsp/src/builtins.rs` from the compiler's own builtin
//! registrations.
//!
//! The language server completes builtins from a table that must never drift
//! from what the compiler registers. A hand-maintained list is how a server
//! starts lying to users, so the table is generated from `src/main.cpp` and
//! `tests/test_lsp.sh` fails on drift.
//!
//! Ported from `tools/gen_lsp_builtins.py`; the output is byte-identical.

use std::collections::BTreeSet;
use std::process::ExitCode;

use crate::common::{read, repo_root};

const SRC: &str = "src/main.cpp";
const OUT: &str = "lsp/src/builtins.rs";

/// Every name registered by an `add("name", ...)` call.
///
/// Deliberately a literal scan rather than a real C++ parse: `src/main.cpp` is
/// 12k lines and the registration syntax is uniform. The gate that consumes
/// this is what protects against drift.
fn registrations(src: &str) -> BTreeSet<String> {
    let mut names = BTreeSet::new();
    let needle = "add(\"";
    let mut rest = src;
    while let Some(at) = rest.find(needle) {
        let after = &rest[at + needle.len()..];
        let end = match after.find('"') {
            Some(e) => e,
            None => break,
        };
        let name = &after[..end];
        // The Python original matched [A-Za-z_][A-Za-z0-9_]*; reject anything
        // that would not have matched, so the two agree exactly.
        let valid = !name.is_empty()
            && name.starts_with(|c: char| c.is_ascii_alphabetic() || c == '_')
            && name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_');
        if valid {
            names.insert(name.to_string());
        }
        rest = &after[end..];
    }
    names
}

fn render(names: &BTreeSet<String>) -> String {
    let flint: Vec<&String> = names
        .iter()
        .filter(|n| n.starts_with("flint_"))
        .collect();
    let raw: Vec<&String> = names
        .iter()
        .filter(|n| !n.starts_with("flint_"))
        .collect();

    let mut lines: Vec<String> = Vec::new();
    lines.push("//! The compiler-registered builtin table, GENERATED from src/main.cpp".into());
    lines.push("//! (`add(\"name\", ...)` registrations) by tools/gen_lsp_builtins.py.".into());
    lines.push("//!".into());
    lines.push("//! Generated rather than hand-written because a hand-written list is".into());
    lines.push("//! how a language server starts lying: a builtin that exists but is missing".into());
    lines.push("//! here is invisible to completion forever, and nobody notices until a user".into());
    lines.push("//! reports it. `tests/test_lsp.sh` regenerates this file and fails on drift.".into());
    lines.push("".into());
    lines.push("/// Flint runtime builtins callable from user code (`flint_*`).".into());
    lines.push(format!(
        "pub const BUILTINS: &[&str] = &[ // {} names, generated",
        flint.len()
    ));
    for name in &flint {
        lines.push(format!("    \"{}\",", name));
    }
    lines.push("];".into());
    lines.push("".into());
    lines.push("/// Direct C-ABI entry points registered without the `flint_` prefix.".into());
    lines.push("/// Kept separate because completion should present them differently.".into());
    lines.push(format!(
        "pub const RAW_EXTERNALS: &[&str] = &[ // {} names, generated",
        raw.len()
    ));
    for name in &raw {
        lines.push(format!("    \"{}\",", name));
    }
    lines.push("];".into());
    lines.push("".into());
    lines.join("\n")
}

pub fn run(args: &[String]) -> ExitCode {
    let root = repo_root();
    let src_path = root.join(SRC);
    let out_path = root.join(OUT);

    let src = match read(&src_path) {
        Some(s) => s,
        None => {
            eprintln!("error: cannot read {}", SRC);
            return ExitCode::from(1);
        }
    };
    let names = registrations(&src);
    if names.is_empty() {
        eprintln!("error: no builtin registrations found in {}", SRC);
        return ExitCode::from(1);
    }
    let body = render(&names);

    let check = args.iter().any(|a| a == "--check");
    if check {
        let current = match read(&out_path) {
            Some(c) => c,
            None => {
                eprintln!("error: {} does not exist; run without --check", OUT);
                return ExitCode::from(1);
            }
        };
        if current != body {
            eprintln!(
                "error: {} is stale ({} registrations in {}); run cargo run --manifest-path tools/rust/Cargo.toml -- gen-lsp-builtins",
                OUT,
                names.len(),
                SRC
            );
            return ExitCode::from(1);
        }
        println!("builtins table in sync: {} names", names.len());
        return ExitCode::SUCCESS;
    }

    if let Err(e) = std::fs::write(&out_path, &body) {
        eprintln!("error: cannot write {}: {}", OUT, e);
        return ExitCode::from(1);
    }
    println!("wrote {}: {} builtins", OUT, names.len());
    ExitCode::SUCCESS
}
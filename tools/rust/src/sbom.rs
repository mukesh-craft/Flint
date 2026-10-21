//! Emit a minimal SPDX-2.3-style SBOM for the Flint release bundle.
//!
//! Scans the repo for release components (binaries, runtime, self-hosted
//! compiler sources, driver/tools, docs), records version (VERSION file),
//! file size + sha256, and the LLVM pin (driver/VERSIONS). Emits JSON.
//!
//! Ported from `tools/sbom.py`; the JSON is byte-identical, including the
//! 2-space indent Python's json.dumps(indent=2) produces.

use std::process::ExitCode;

use sha2::{Digest, Sha256};

use crate::common::repo_root;

const COMPONENTS: &[(&str, &str, &str)] = &[
    ("flintc", "flintc", "native compiler binary (C++/LLVM)"),
    ("runtime", "runtime/runtime.c", "portable C runtime"),
    ("stage1-lexer", "stage1/lexer.fl", "self-hosted lexer"),
    ("stage2-parser", "stage2/parser.fl", "self-hosted parser"),
    ("stage3-emitter", "stage3/emitter.fl", "self-hosted emitter"),
    ("driver", "driver/flintc.fl", "pipeline driver"),
    ("merge-tool", "tools/merge_sexp.fl", "S-expr merger"),
    ("formatter", "flint-fmt", "source formatter"),
    ("fuzz-generator", "fuzz/generate.py", "differential fuzz generator"),
];

fn sha256_of(bytes: &[u8]) -> String {
    let digest = Sha256::digest(bytes);
    let mut out = String::with_capacity(64);
    for byte in digest {
        out.push_str(&format!("{:02x}", byte));
    }
    out
}

fn version(root: &std::path::Path) -> String {
    std::fs::read_to_string(root.join("VERSION"))
        .map(|s| s.trim().to_string())
        .unwrap_or_else(|_| "0.0.0-unknown".to_string())
}

fn llvm_pin(root: &std::path::Path) -> String {
    let mut pin = "unknown".to_string();
    if let Ok(text) = std::fs::read_to_string(root.join("driver").join("VERSIONS")) {
        for line in text.lines() {
            let stripped = line.trim();
            if stripped.is_empty() || stripped.starts_with('#') {
                continue;
            }
            if stripped.contains("LLVM") {
                pin = stripped.to_string();
                break;
            }
        }
    }
    pin
}

/// Python's json.dumps escapes a few characters that Rust's debug formatting
/// does not, and does not escape non-ASCII. Matching it keeps the SBOM
/// byte-identical to the version this replaced.
fn json_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out
}

pub fn run() -> ExitCode {
    let root = repo_root();
    let ver = version(&root);

    let mut packages: Vec<String> = Vec::new();
    for (name, rel, desc) in COMPONENTS {
        let path = root.join(rel);
        // A missing component is skipped, exactly as the Python version did:
        // the SBOM describes what the release actually contains.
        let (size, hash) = match std::fs::metadata(&path).and_then(|m| {
            std::fs::read(&path).map(|bytes| (m.len() as i64, sha256_of(&bytes)))
        }) {
            Ok(v) => v,
            Err(_) => continue,
        };
        packages.push(format!(
            "    {{\n      \"name\": \"flint-{}\",\n      \"version\": \"{}\",\n      \
             \"description\": \"{}\",\n      \"file\": \"{}\",\n      \
             \"size_bytes\": {},\n      \"sha256\": \"{}\"\n    }}",
            json_escape(name),
            json_escape(&ver),
            json_escape(desc),
            json_escape(rel),
            size,
            hash
        ));
    }

    println!("{{");
    println!("  \"spdxVersion\": \"SPDX-2.3\",");
    println!("  \"name\": \"flint\",");
    println!("  \"documentVersion\": \"{}\",", json_escape(&ver));
    println!("  \"toolchain\": \"{}\",", json_escape(&llvm_pin(&root)));
    if packages.is_empty() {
        println!("  \"packages\": []");
    } else {
        println!("  \"packages\": [\n{}\n  ]", packages.join(",\n"));
    }
    println!("}}");
    ExitCode::SUCCESS
}
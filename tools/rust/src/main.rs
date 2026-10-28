//! Flint development tooling, ported from Python.
//!
//! Why this exists: every tool here used to require `python3` on the build
//! machine. Most of them are gates, so a contributor without Python could not
//! run the checks that keep the compiler and its editor support honest.
//!
//! Each port was verified to produce byte-identical output to the Python
//! original before the original was deleted; that equivalence is checked by
//! `tests/test_tools.sh`, which keeps the Python versions around specifically
//! as the oracle.
//!
//! Note on performance: this port is a dependency cleanup, not a speedup. The
//! tools run once per gate invocation, so the win is "one toolchain", not
//! milliseconds.

use std::process::ExitCode;

mod common;
mod decl_order;
mod gen_lsp_builtins;
mod sbom;
mod vscode;

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    let command = match args.get(1) {
        Some(c) => c.as_str(),
        None => {
            eprintln!(
                "usage: flint-tool <command> [args]\n\n\
                 commands:\n  \
                 gen-lsp-builtins [--check]   regenerate lsp/src/builtins.rs from src/main.cpp\n  \
                 sbom                         emit an SPDX-2.3 SBOM as JSON\n  \
                 decl-order <file.fl>...      check declaration order for method receivers\n  \
                 check-vscode                 verify the VS Code extension matches the compiler"
            );
            return ExitCode::from(2);
        }
    };
    match command {
        "gen-lsp-builtins" => gen_lsp_builtins::run(&args[2..]),
        "sbom" => sbom::run(),
        "decl-order" => decl_order::run(&args[2..]),
        "check-vscode" => vscode::run(),
        other => {
            eprintln!("flint-tool: unknown command '{}'", other);
            ExitCode::from(2)
        }
    }
}
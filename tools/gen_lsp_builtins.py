#!/usr/bin/env python3
"""Regenerate lsp/src/builtins.rs from the compiler's own builtin registrations.

The language server completes builtins from a table that must never drift from
what the compiler actually registers. Hand-maintaining that list is how a
server starts lying to users, so it is generated from src/main.cpp and the
`tests/test_lsp.sh` gate fails on drift.

Usage: python3 tools/gen_lsp_builtins.py          # rewrite the file
       python3 tools/gen_lsp_builtins.py --check  # fail if it would change
"""
import re
import sys

SRC = "src/main.cpp"
OUT = "lsp/src/builtins.rs"


def render(names):
    flint = [n for n in names if n.startswith("flint_")]
    raw = [n for n in names if not n.startswith("flint_")]
    lines = [
        "//! The compiler-registered builtin table, GENERATED from src/main.cpp",
        "//! (`add(\"name\", ...)` registrations) by tools/gen_lsp_builtins.py.",
        "//!",
        "//! Generated rather than hand-written because a hand-written list is",
        "//! how a language server starts lying: a builtin that exists but is missing",
        "//! here is invisible to completion forever, and nobody notices until a user",
        "//! reports it. `tests/test_lsp.sh` regenerates this file and fails on drift.",
        "",
        "/// Flint runtime builtins callable from user code (`flint_*`).",
        "pub const BUILTINS: &[&str] = &[ // %d names, generated" % len(flint),
    ]
    lines += ['    "%s",' % n for n in flint]
    lines += [
        "];",
        "",
        "/// Direct C-ABI entry points registered without the `flint_` prefix.",
        "/// Kept separate because completion should present them differently.",
        "pub const RAW_EXTERNALS: &[&str] = &[ // %d names, generated" % len(raw),
    ]
    lines += ['    "%s",' % n for n in raw]
    lines += ["];", ""]
    return "\n".join(lines)


def main():
    src = open(SRC).read()
    names = sorted(set(re.findall(r'add\("([A-Za-z_][A-Za-z0-9_]*)"', src)))
    if not names:
        print("error: no builtin registrations found in " + SRC)
        return 1
    body = render(names)
    if "--check" in sys.argv:
        try:
            current = open(OUT).read()
        except FileNotFoundError:
            print("error: %s does not exist; run without --check" % OUT)
            return 1
        if current != body:
            print("error: %s is stale (%d registrations in %s); run python3 tools/gen_lsp_builtins.py"
                  % (OUT, len(names), SRC))
            return 1
        print("builtins table in sync: %d names" % len(names))
        return 0
    open(OUT, "w").write(body)
    print("wrote %s: %d builtins" % (OUT, len(names)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

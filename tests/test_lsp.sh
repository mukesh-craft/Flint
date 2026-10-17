#!/usr/bin/env bash
# lsp gate: the Rust language server, tested as a real process over real
# JSON-RPC framing, plus two drift gates that keep it honest.
#
#   1. protocol   — initialize/capabilities, diagnostics (broken + clean),
#                   completion, hover, definition, references, documentSymbol,
#                   didChange, unknown-method error, shutdown/exit.
#   2. keywords   — the server's keyword table must equal the compiler lexer's
#                   keyword table exactly (extracted from src/main.cpp). A
#                   server that drifts here silently loses completions.
#   3. builtins   — lsp/src/builtins.rs must equal the compiler's registered
#                   builtins. Regenerable with tools/gen_lsp_builtins.py.
#
# Usage: bash tests/test_lsp.sh [path/to/flintc]   (default: ./flintc)
set -u
WORK="${TMPDIR:-$HOME/.tmp}"
mkdir -p "$WORK"
FLINTC="${1:-./flintc}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SERVER="lsp/target/release/flint-lsp"
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "PASS $1"; }
bad() { fail=$((fail+1)); echo "FAIL $1${2:+ — $2}"; }

command -v rustc >/dev/null 2>&1 || { echo "SKIP lsp gate (rustc not installed: pkg install rust)"; exit 0; }

# Rebuild whenever a source file is newer than the binary. Skipping the build
# because "the file exists" is how this gate once tested a stale server and
# reported phantom protocol failures.
needs_build=0
if [ ! -x "$SERVER" ]; then
    needs_build=1
else
    for src in lsp/src/*.rs lsp/Cargo.toml; do
        [ -e "$src" ] && [ "$src" -nt "$SERVER" ] && needs_build=1 && break
    done
fi
if [ "$needs_build" = 1 ]; then
    echo "building the language server..."
    (cd lsp && cargo build --release) > "$WORK/lsp-build.log" 2>&1 \
        || { echo "FAIL server build"; tail -n 10 "$WORK/lsp-build.log"; exit 1; }
fi
[ -x "$SERVER" ] && ok "server binary exists ($SERVER)" || bad "server binary exists"

if python3 tests/test_lsp.py "$SERVER" "$FLINTC"; then
    ok "protocol conformance"
else
    bad "protocol conformance"
fi

# --- keyword drift: server table vs compiler lexer ----------------------------
python3 - <<'PYKW' && ok "keyword table matches the compiler lexer" || bad "keyword table matches the compiler lexer"
import re, sys
src = open('src/main.cpp').read()
lexer_block = src.split('Token readIdentifier()', 1)[1].split('};', 1)[0]
lexer_kw = set(re.findall(r's == "([a-z_0-9]+)"\)\s+return makeToken\(TokenType::KW_', lexer_block))
server = open('lsp/src/analyzer.rs').read()
table = server.split('pub const KEYWORDS', 1)[1].split('];', 1)[0]
server_kw = set(re.findall(r'\("([a-z_0-9]+)"', table))
missing = sorted(lexer_kw - server_kw)
extra = sorted(server_kw - lexer_kw)
if missing or extra:
    print("keyword drift: missing from server=%s extra in server=%s" % (missing, extra))
    sys.exit(1)
print("  lexer keywords: %d, server keywords: %d, identical" % (len(lexer_kw), len(server_kw)))
PYKW

# --- builtin drift: generated table vs compiler registrations ---------------
python3 - <<'PYBI' && ok "builtin table matches compiler registrations" || bad "builtin table matches compiler registrations"
import re, sys
src = open('src/main.cpp').read()
registered = set(re.findall(r'add\("([A-Za-z_][A-Za-z0-9_]*)"', src))
builtins_rs = open('lsp/src/builtins.rs').read()
bblock = builtins_rs.split('pub const BUILTINS', 1)[1].split('];', 1)[0]
rblock = builtins_rs.split('pub const RAW_EXTERNALS', 1)[1].split('];', 1)[0]
listed = set(re.findall(r'"([A-Za-z_][A-Za-z0-9_]*)"', bblock + rblock))
if registered != listed:
    print("builtin drift: missing=%s extra=%s" % (
        sorted(registered - listed)[:10], sorted(listed - registered)[:10]))
    sys.exit(1)
print("  registered builtins: %d, listed in server: %d, identical" % (len(registered), len(listed)))
PYBI

# --- the server must not hardcode /tmp (the bug that broke the old one) -----
if rg -q '"/tmp"' lsp/src 2>/dev/null || grep -rq '"/tmp"' lsp/src; then
    bad "no hardcoded /tmp in server sources"
else
    ok "no hardcoded /tmp in server sources"
fi

echo "lsp: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
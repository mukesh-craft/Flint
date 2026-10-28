#!/usr/bin/env bash
# tools gate: the Rust dev tooling must behave exactly like the Python it
# replaced.
#
# The four tools in tools/rust were ported from Python. A port that is "close"
# is worse than no port: gen_lsp_builtins feeds LSP completion, sbom feeds the
# release, check_vscode keeps the editor honest about keywords, and decl-order
# catches a silent codegen failure mode. So the Python originals are kept as
# oracles and this gate compares them directly.
#
# Once a tool has been stable it can be deleted from the Python side and its
# oracle removed here; do not delete both in one step.
#
# Usage: bash tests/test_tools.sh [path/to/flint-tool]  (default: the built one)
set -u
TOOL="${1:-tools/rust/target/release/flint-tool}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "PASS $1"; }
bad() { fail=$((fail+1)); echo "FAIL $1${2:+ — $2}"; }

if [ ! -x "$TOOL" ]; then
    bad "flint-tool is built" "missing $TOOL (cargo build --release --manifest-path tools/rust/Cargo.toml)"
    echo "tools: $pass passed, $fail failed"
    exit 1
fi

# --- gen-lsp-builtins: the generated table must be byte-identical ----------
cp lsp/src/builtins.rs "$WORK/builtins.ref"
if "$TOOL" gen-lsp-builtins > "$WORK/gen.log" 2>&1; then
    if cmp -s "$WORK/builtins.ref" lsp/src/builtins.rs; then
        ok "gen-lsp-builtins reproduces lsp/src/builtins.rs byte for byte"
    else
        bad "gen-lsp-builtins output matches the committed table" \
            "$(diff "$WORK/builtins.ref" lsp/src/builtins.rs | head -3 | tr '\n' ' ')"
    fi
else
    bad "gen-lsp-builtins runs" "$(tail -2 "$WORK/gen.log" | tr '\n' ' ')"
fi
# --check must agree with what the file currently holds.
if "$TOOL" gen-lsp-builtins --check > /dev/null 2>&1; then
    ok "gen-lsp-builtins --check reports the table in sync"
else
    bad "gen-lsp-builtins --check reports the table in sync"
fi

# --- sbom: identical JSON, including formatting ----------------------------
if python3 tools/sbom.py > "$WORK/sbom.py.json" 2>/dev/null; then
    "$TOOL" sbom > "$WORK/sbom.rs.json" 2>&1
    if cmp -s "$WORK/sbom.py.json" "$WORK/sbom.rs.json"; then
        ok "sbom output is byte-identical to the Python original"
    else
        bad "sbom output matches the Python original" \
            "$(diff "$WORK/sbom.py.json" "$WORK/sbom.rs.json" | head -5 | tr '\n' ' ')"
    fi
else
    bad "python3 tools/sbom.py oracle runs"
fi

# --- decl-order: same verdict on the self-hosted stages --------------------
STAGES="stage1/lexer.fl stage2/parser.fl stage3/emitter.fl"
# shellcheck disable=SC2086
python3 tests/test_decl_order.py $STAGES > "$WORK/decl.py" 2>&1; py_status=$?
# shellcheck disable=SC2086
"$TOOL" decl-order $STAGES > "$WORK/decl.rs" 2>&1; rs_status=$?
if [ "$py_status" = "$rs_status" ] && cmp -s "$WORK/decl.py" "$WORK/decl.rs"; then
    ok "decl-order agrees with the Python original (exit $rs_status)"
else
    bad "decl-order agrees with the Python original" \
        "py=$py_status rs=$rs_status: $(diff "$WORK/decl.py" "$WORK/decl.rs" | head -3 | tr '\n' ' ')"
fi

# --- check-vscode: same verdict, and it must pass -------------------------
python3 tools/check_vscode_extension.py > "$WORK/vs.py" 2>&1; py_status=$?
"$TOOL" check-vscode > "$WORK/vs.rs" 2>&1; rs_status=$?
if [ "$py_status" = "$rs_status" ] && cmp -s "$WORK/vs.py" "$WORK/vs.rs"; then
    ok "check-vscode agrees with the Python original (exit $rs_status)"
else
    bad "check-vscode agrees with the Python original" \
        "py=$py_status rs=$rs_status: $(diff "$WORK/vs.py" "$WORK/vs.rs" | head -5 | tr '\n' ' ')"
fi

echo "tools: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
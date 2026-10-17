#!/usr/bin/env bash
# --debug: real DWARF line tables in artifacts.
#
# Before this gate, compiled binaries had no debug info: gdb/lldb could not
# map anything to Flint source and the tooling table honestly said so
# (memory.md 2026-10-10). The flag adds a DIBuilder compile unit + file +
# per-function subprogram + per-statement DILocation, and module passes run
# at O0 under --debug because O2 constant-folds statements and erases their
# line info. JIT run with a debug module hangs at mainFn (reproduced
# consistently), so bare-run mode refuses with an actionable message; the
# artifact path is one command (`--debug -o prog && ./prog`).
#
# Oracle: llvm-dwarfdump on device (no lldb/gdb needed for the assertions).
# Line numbers below are pinned against tests/t_debug.fl: doubled() starts
# at line 4, its return is line 5, main() at 8, statements at 9/10/11/12.
# Usage: bash tests/test_dwarf.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
TMPD="$TMPBASE/slipstream/dwarf"
mkdir -p "$TMPD"
pass=0; fail=0

check() { # name expected actual
  if [ "$2" = "$3" ]; then pass=$((pass+1)); echo "PASS $1"; else fail=$((fail+1)); echo "FAIL $1"; echo "  want: $2"; echo "  got:  $3"; fi
}

have_dwarfdump() { command -v llvm-dwarfdump > /dev/null 2>&1; }

# 0. default builds stay debug-free
"$FLINTC" tests/t_debug.fl --emit-llvm -o "$TMPD/plain.ll" > /dev/null 2>&1
check "no DI without --debug" "0" "$(grep -c 'DICompileUnit' "$TMPD/plain.ll")"

# 1. IR carries the compile unit, file and subprograms
"$FLINTC" tests/t_debug.fl --debug --emit-llvm -o "$TMPD/dbg.ll" > /dev/null 2>&1
check "DICompileUnit in IR" "1" "$(grep -c 'DICompileUnit' "$TMPD/dbg.ll")"
check "DIFile names the source" '1' \
  "$(grep -c 'DIFile(filename: "t_debug.fl"' "$TMPD/dbg.ll")"
check "DISubprogram doubled" '1' \
  "$(grep -c 'name: "doubled"' "$TMPD/dbg.ll")"
check "DILocation for stmt line 9" '1' \
  "$(grep -c '!DILocation(line: 9' "$TMPD/dbg.ll")"
check "DILocation for stmt line 10" '1' \
  "$(grep -c '!DILocation(line: 10' "$TMPD/dbg.ll")"

# 2. the object carries real DWARF with the same facts
"$FLINTC" tests/t_debug.fl --debug --emit-llvm -o "$TMPD/dbg.o" > /dev/null 2>&1
if have_dwarfdump; then
  check "object has compile unit" '1' \
    "$(llvm-dwarfdump --debug-info "$TMPD/dbg.o" 2>/dev/null | grep -c 'DW_TAG_compile_unit')"
  check "subprogram doubled" '1' \
    "$(llvm-dwarfdump --debug-info "$TMPD/dbg.o" 2>/dev/null | grep -c 'DW_AT_name	("doubled")')"
  check "line table rows" '13' \
    "$(llvm-dwarfdump --debug-line "$TMPD/dbg.o" 2>/dev/null | grep -c 'is_stmt')"
  check "file in line table" '1' \
    "$(llvm-dwarfdump --debug-line "$TMPD/dbg.o" 2>/dev/null | grep -c '"t_debug.fl"')"
  # line 5 (the return in doubled) and line 11 (print statement) both map;
  # rows may repeat, so the assertion is "at least one row".
  l5="$(llvm-dwarfdump --debug-line "$TMPD/dbg.o" 2>/dev/null | awk '{print $2}' | grep -c '^5$')"
  [ "$l5" -ge 1 ] && check "line 5 mapped" ok ok || check "line 5 mapped" ok "missing"
  l11="$(llvm-dwarfdump --debug-line "$TMPD/dbg.o" 2>/dev/null | awk '{print $2}' | grep -c '^11$')"
  [ "$l11" -ge 1 ] && check "line 11 mapped" ok ok || check "line 11 mapped" ok "missing"
fi

# 3. --debug without -o refuses loudly (JIT with debug hangs)
out="$("$FLINTC" tests/t_debug.fl --debug 2>&1)"; rc=$?
check "bare --debug rc" '1' "$rc"
if echo "$out" | grep -q 'builds an artifact with DWARF'; then pass=$((pass+1)); echo "PASS bare --debug message"; else fail=$((fail+1)); echo "FAIL bare --debug message: $out"; fi

# 4. the linked artifact runs and print() output is unchanged
"$FLINTC" tests/t_debug.fl --debug -o "$TMPD/dbgbin" > /dev/null 2>&1
check "artifact runs (12)" '12' "$(timeout 30 "$TMPD/dbgbin")"

# 5. plain run still fine (default path untouched)
check "plain run unchanged" '12' "$(timeout 30 "$FLINTC" tests/t_debug.fl)"

echo "dwarf gate: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

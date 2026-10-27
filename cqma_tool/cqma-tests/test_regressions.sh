#!/usr/bin/env bash
# Regression gate for the CQMA false-clean fixes.
#
# These are the cases that used to look CLEAN even though analysis did not
# finish:
#   * every stage disabled
#   * --max-files truncated the input set
#   * the "clang" analyzer command failed (e.g. a GCC --cc)
#   * valgrind timed out or wrote no log
#   * valgrind reported errors whose detail the parser did not extract
#
# The same-size-leak dedup case is a unit test in src/main.rs (cargo test),
# because it does not need a real sanitizer runtime.
#
# Usage: bash test_regressions.sh [path/to/cqma]
set -u
CQMA="${1:-${CQMA:-$(cd "$(dirname "$0")/../cqma-rs" && pwd)/target/release/cqma}}"
if [ ! -x "$CQMA" ]; then
  echo "FAIL: cqma binary not found: $CQMA" >&2
  exit 1
fi
T="$(mktemp -d "${TMPDIR:-/tmp}/cqma-regress.XXXXXX")"
trap 'rm -rf "$T"' EXIT
pass=0; fail=0

check_json() { # file python-expression description
  local file="$1" expr="$2" desc="$3"
  if python3 - "$file" "$expr" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
sys.exit(0 if eval(sys.argv[2], {"d": d}) else 1)
PY
  then
    echo "PASS: $desc"; pass=$((pass+1))
  else
    echo "FAIL: $desc"; fail=$((fail+1))
    python3 - "$file" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
print(json.dumps({k: d.get(k) for k in ("complete","coverage_gaps","score","grade","exit_code")}, indent=2))
PY
  fi
}

printf 'int main(void){return 0;}\n' > "$T/clean.c"
printf 'int main(void){return 0;}\n' > "$T/other.c"

# 1. No stages requested: an empty stage list is not a clean run.
"$CQMA" "$T/clean.c" --no-compile --no-cppcheck --no-analyzer --no-runtime \
  --output "$T/all.md" --json "$T/all.json" --quiet
rc=$?
[ "$rc" -eq 3 ] && { echo "PASS: all-disabled exit 3"; pass=$((pass+1)); } \
  || { echo "FAIL: all-disabled exit $rc (want 3)"; fail=$((fail+1)); }
check_json "$T/all.json" 'd["complete"] is False and d["score"] is None and any("all disabled" in g for g in d["coverage_gaps"])' \
  "all-disabled reports incomplete"

# 2. --max-files drops a file: the omitted input is a coverage gap.
"$CQMA" "$T" --max-files 1 --no-runtime --no-cppcheck --no-analyzer \
  --output "$T/max.md" --json "$T/max.json" --quiet
rc=$?
[ "$rc" -eq 3 ] && { echo "PASS: max-files exit 3"; pass=$((pass+1)); } \
  || { echo "FAIL: max-files exit $rc (want 3)"; fail=$((fail+1)); }
check_json "$T/max.json" 'd["complete"] is False and any("skipped before analysis" in g for g in d["coverage_gaps"])' \
  "max-files reports the skipped input"

# 3. A clang analyzer command that exits nonzero must not look clean.
cat > "$T/fake-clang" <<'EOF'
#!/bin/sh
if [ "$1" = "--version" ]; then echo "clang version 99.0.0"; exit 0; fi
echo "fake clang: analyzer failed" >&2
exit 1
EOF
chmod +x "$T/fake-clang"
"$CQMA" "$T/clean.c" --cc "$T/fake-clang" --cxx "$T/fake-clang" \
  --no-compile --no-cppcheck --no-runtime \
  --output "$T/an.md" --json "$T/an.json" --quiet
rc=$?
[ "$rc" -eq 3 ] && { echo "PASS: failed analyzer exit 3"; pass=$((pass+1)); } \
  || { echo "FAIL: failed analyzer exit $rc (want 3)"; fail=$((fail+1)); }
check_json "$T/an.json" 'd["complete"] is False and d["stages"][0]["status"] == "failed" and any(f["operational"] and f["check"] == "stage-failed" for f in d["findings"])' \
  "failed analyzer is visible and operational"

# 4. Valgrind that times out must be reported as a timeout, not a clean run.
cat > "$T/fake-cc" <<'EOF'
#!/bin/sh
exit 1
EOF
cat > "$T/valgrind-timeout" <<'EOF'
#!/bin/sh
sleep 5
exit 0
EOF
chmod +x "$T/fake-cc" "$T/valgrind-timeout"
mkdir -p "$T/timeout-bin"
cp "$T/valgrind-timeout" "$T/timeout-bin/valgrind"
PATH="$T/timeout-bin:$PATH" "$CQMA" "$T/clean.c" --cc "$T/fake-cc" --cxx "$T/fake-cc" \
  --no-compile --no-cppcheck --no-analyzer --binary "$T/clean.c" --timeout-run 1 \
  --output "$T/vt.md" --json "$T/vt.json" --quiet
rc=$?
[ "$rc" -eq 3 ] && { echo "PASS: valgrind timeout exit 3"; pass=$((pass+1)); } \
  || { echo "FAIL: valgrind timeout exit $rc (want 3)"; fail=$((fail+1)); }
check_json "$T/vt.json" 'd["complete"] is False and any(f["check"] == "timeout" and f["operational"] for f in d["findings"])' \
  "valgrind timeout is operational and incomplete"

# 5. A completed valgrind run with a valid summary log is clean.
cat > "$T/valgrind-ok" <<'EOF'
#!/bin/sh
log=""
for arg in "$@"; do
  case "$arg" in --log-file=*) log="${arg#--log-file=}" ;; esac
done
[ -n "$log" ] && printf '==1== HEAP SUMMARY:\n==1== ERROR SUMMARY: 0 errors from 0 contexts\n' > "$log"
exit 0
EOF
chmod +x "$T/valgrind-ok"
mkdir -p "$T/ok-bin"
cp "$T/valgrind-ok" "$T/ok-bin/valgrind"
PATH="$T/ok-bin:$PATH" "$CQMA" "$T/clean.c" --cc "$T/fake-cc" --cxx "$T/fake-cc" \
  --no-compile --no-cppcheck --no-analyzer --binary "$T/clean.c" \
  --output "$T/vo.md" --json "$T/vo.json" --quiet
rc=$?
[ "$rc" -eq 0 ] && { echo "PASS: valid valgrind run exit 0"; pass=$((pass+1)); } \
  || { echo "FAIL: valid valgrind run exit $rc (want 0)"; fail=$((fail+1)); }
check_json "$T/vo.json" 'd["complete"] is True and d["score"] == 100 and d["grade"] == "CLEAN"' \
  "valid valgrind run is clean"

# 6. A summary that says errors exist but whose detail was not parsed must not
#    be a clean run (the old code kept the operational finding but still
#    scored 100 because the stage status was "ran").
cat > "$T/valgrind-errors" <<'EOF'
#!/bin/sh
log=""
for arg in "$@"; do
  case "$arg" in --log-file=*) log="${arg#--log-file=}" ;; esac
done
[ -n "$log" ] && printf '==1== HEAP SUMMARY:\n==1== ERROR SUMMARY: 2 errors from 2 contexts\n' > "$log"
exit 0
EOF
chmod +x "$T/valgrind-errors"
mkdir -p "$T/err-bin"
cp "$T/valgrind-errors" "$T/err-bin/valgrind"
PATH="$T/err-bin:$PATH" "$CQMA" "$T/clean.c" --cc "$T/fake-cc" --cxx "$T/fake-cc" \
  --no-compile --no-cppcheck --no-analyzer --binary "$T/clean.c" \
  --output "$T/ve.md" --json "$T/ve.json" --quiet
rc=$?
[ "$rc" -eq 3 ] && { echo "PASS: unparsed valgrind errors exit 3"; pass=$((pass+1)); } \
  || { echo "FAIL: unparsed valgrind errors exit $rc (want 3)"; fail=$((fail+1)); }
check_json "$T/ve.json" 'd["complete"] is False and d["score"] is None and any(f["check"] == "error-summary" and f["operational"] for f in d["findings"]) and any("not fully parsed" in g for g in d["coverage_gaps"])' \
  "unparsed valgrind errors are a coverage gap"

echo "cqma-regressions: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

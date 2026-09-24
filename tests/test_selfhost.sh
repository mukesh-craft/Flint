#!/usr/bin/env bash
# Self-host bootstrap Phase A/B/C (see memory.md STAGE 4 PLAN).
# Phase A: C++ flintc AOT-builds the Flint pipeline (lex.0/parse.0/emit.0).
# Phase B: pipeline self-compiles flint_emit.fl -> emit.1 (must run ladder).
# Phase C: emit.1 recompiles -> byte-identical .ll (fixed point).
# Usage: bash tests/test_selfhost.sh [phase]   (default: all)
# Phases: A (AOT builds) B (self-compile emit.1) C (fixed point)
#         stable (verify + promote emit.1 to $STABLE_DIR)
set -u
FLINTC="${FLINTC:-./flintc}"
OUT="/data/data/com.termux/files/usr/tmp/slipstream/self"
mkdir -p "$OUT"
PHASE="${1:-all}"

merge_sexp() {
  # merge_sexp <main.sexp> <out.sexp> [extra.sexp...]: drop (import ...) forms,
  # concatenate top-level forms (main file first, so its main wins).
  # String-aware split (mirrors tools/merge_sexp.fl).
  python3 - "$1" "$2" "${@:3}" <<'PYEOF'
import sys
def split_forms(body):
    forms = []
    i = 0
    n = len(body)
    def skip_str(k):
        # Exact `(str LEN RAW)` skip (mirrors tools/merge_sexp.fl):
        # `(str ` prefix, digits, exactly ONE separator space, LEN
        # verbatim bytes, `)`. Never ws-skip: RAW may start with spaces.
        j = k + 5
        e = j
        while e < n and body[e].isdigit():
            e += 1
        ln = int(body[j:e])
        j = e + 1
        return j + ln + 1
    while i < n:
        if body[i] == '(':
            j = i + 1
            while j < n and body[j] not in ' )':
                j += 1
            tag = body[i+1:j]
            d = 0
            k = i
            while k < n:
                if body.startswith('(str ', k):
                    k = skip_str(k)
                    continue
                if body[k] == '(': d += 1
                elif body[k] == ')':
                    d -= 1
                    if d == 0: break
                k += 1
            forms.append((tag, body[i:k+1]))
            i = k + 1
        else:
            i += 1
    return forms
def load(p):
    s = open(p).read().strip()
    assert s.startswith('(prog ') and s.endswith(')')
    return split_forms(s[len('(prog '):-1])
main_forms = load(sys.argv[1])
extra = []
for p in sys.argv[3:]:
    extra += load(p)
kept = [(t, f) for (t, f) in main_forms if t != 'import']
for (t, f) in extra:
    if t == 'import':
        continue
    kept.append((t, f))
with open(sys.argv[2], 'w') as fh:
    fh.write('(prog ' + ' '.join(f for (_, f) in kept) + ')\n')
print('merged %d forms -> %s' % (len(kept), sys.argv[2]))
PYEOF
}

if [ "$PHASE" = all ] || [ "$PHASE" = A ]; then
  echo "=== Phase A: AOT builds"
  timeout -s KILL 1200 "$FLINTC" stage1/flint_lex.fl -o "$OUT/lex.0" || { echo "FAIL lex.0 build"; exit 1; }
  timeout -s KILL 1800 "$FLINTC" stage2/flint_parse.fl -o "$OUT/parse.0" || { echo "FAIL parse.0 build"; exit 1; }
  timeout -s KILL 1800 "$FLINTC" stage3/flint_emit.fl -o "$OUT/emit.0" || { echo "FAIL emit.0 build"; exit 1; }
  ls -la "$OUT"/lex.0 "$OUT"/parse.0 "$OUT"/emit.0
  # smoke: pipeline compiles g0_hello
  "$OUT/parse.0" stage3/ladder/g0_hello.sexp > /dev/null 2>&1 || true
  timeout -s KILL 300 "$FLINTC" stage3/flint_emit.fl -- stage3/ladder/g0_hello.sexp "$OUT/smoke.ll" || { echo "FAIL smoke"; exit 1; }
  echo "Phase A OK"
fi

if [ "$PHASE" = all ] || [ "$PHASE" = B ]; then
  echo "=== Phase B: self-compile emit"
  timeout -s KILL 900 "$OUT/parse.0" stage3/flint_emit.fl > "$OUT/emit.main.sexp" || { echo "FAIL parse emit"; exit 1; }
  timeout -s KILL 900 "$OUT/parse.0" stage1/flint_lex.fl > "$OUT/lex.main.sexp" || { echo "FAIL parse lex"; exit 1; }
  merge_sexp "$OUT/emit.main.sexp" "$OUT/emit.merged.sexp" "$OUT/lex.main.sexp"
  timeout -s KILL 900 "$OUT/emit.0" "$OUT/emit.merged.sexp" "$OUT/emit_self.ll" || { echo "FAIL emit self"; exit 1; }
  clang "$OUT/emit_self.ll" runtime/runtime.c -lm -o "$OUT/emit.1" || { echo "FAIL link emit.1"; exit 1; }
  # emit.1 must compile the ladder
  timeout -s KILL 300 "$OUT/emit.1" stage3/ladder/g0_hello.sexp "$OUT/smoke1.ll" || { echo "FAIL emit.1 smoke"; exit 1; }
  echo "Phase B OK"
fi

if [ "$PHASE" = all ] || [ "$PHASE" = C ]; then
  echo "=== Phase C: fixed point"
  timeout -s KILL 900 "$OUT/emit.1" "$OUT/emit.merged.sexp" "$OUT/emit_self2.ll" || { echo "FAIL emit.1 self"; exit 1; }
  if diff -q "$OUT/emit_self.ll" "$OUT/emit_self2.ll" > /dev/null; then
    echo "Phase C OK: fixed point byte-identical"
  else
    echo "FAIL fixed point differs:"; diff "$OUT/emit_self.ll" "$OUT/emit_self2.ll" | head -n 20; exit 1
  fi
fi

if [ "$PHASE" = all ] || [ "$PHASE" = stable ]; then
  echo "=== Stable promotion (flintc-stable)"
  STABLE_DIR="${STABLE_DIR:-/data/data/com.termux/files/usr/tmp/slipstream/stable}"
  mkdir -p "$STABLE_DIR"
  # Re-verify before promoting: fixed point + ladder smoke through emit.1.
  diff -q "$OUT/emit_self.ll" "$OUT/emit_self2.ll" > /dev/null \
    || { echo "FAIL stable (no fixed point; run phases B C first)"; exit 1; }
  timeout -s KILL 300 "$OUT/emit.1" stage3/ladder/g0_hello.sexp "$STABLE_DIR/smoke.ll" \
    || { echo "FAIL stable (emit.1 smoke)"; exit 1; }
  diff -q stage3/ladder/g0_hello.ll "$STABLE_DIR/smoke.ll" > /dev/null \
    || { echo "FAIL stable (smoke differs from golden)"; exit 1; }
  cp "$OUT/emit.1" "$OUT/parse.0" "$STABLE_DIR/" 2>/dev/null \
    || cp "$OUT/emit.1" "$STABLE_DIR/" 2>/dev/null \
    || { echo "FAIL stable (install)"; exit 1; }
  llvm-config --version > "$STABLE_DIR/VERSIONS.toolchain" 2>/dev/null
  cp driver/VERSIONS "$STABLE_DIR/VERSIONS" 2>/dev/null || true
  echo "Stable OK: $STABLE_DIR ($(ls "$STABLE_DIR" | tr '\n' ' '))"
fi
echo "selfhost done"

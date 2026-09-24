#!/usr/bin/env bash
# Merge tool test: Flint merger vs frozen expected AND vs python reference.
# Usage: bash tests/test_merge.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
TMPD="/data/data/com.termux/files/usr/tmp/slipstream/merge"
mkdir -p "$TMPD"
if ! timeout -s KILL 300 "$FLINTC" tools/merge_sexp.fl -o "$TMPD/merge_sexp.bin" 2>/dev/null; then echo "FAIL merge (build)"; exit 1; fi
if ! "$TMPD/merge_sexp.bin" tests/fixtures/merge/a.sexp "$TMPD/out.sexp" tests/fixtures/merge/b.sexp 2>/dev/null; then echo "FAIL merge (run)"; exit 1; fi
if ! diff -q tests/fixtures/merge/expected.sexp "$TMPD/out.sexp" > /dev/null; then echo "FAIL merge (expected diff)"; diff tests/fixtures/merge/expected.sexp "$TMPD/out.sexp" | head -n 4; exit 1; fi
python3 - tests/fixtures/merge/a.sexp "$TMPD/py.sexp" tests/fixtures/merge/b.sexp <<'PYEOF'
import sys
def split_forms(body):
    # String-aware top-level split (mirrors tools/merge_sexp.fl).
    forms = []
    i = 0
    n = len(body)
    def skip_str(k):
        # Exact `(str LEN RAW)` skip (mirrors tools/merge_sexp.fl):
        # never ws-skip around LEN/content — RAW may start with spaces.
        assert body[k:k+5] == '(str '
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
    return split_forms(s[len('(prog '):-1])
forms = [(t,f) for (t,f) in load(sys.argv[1]) if t != 'import']
for p in sys.argv[3:]:
    forms += [(t,f) for (t,f) in load(p) if t != 'import']
open(sys.argv[2],'w').write('(prog ' + ' '.join(f for (_,f) in forms) + ')\n')
PYEOF
if ! diff -q "$TMPD/py.sexp" "$TMPD/out.sexp" > /dev/null; then echo "FAIL merge (python cross-check)"; exit 1; fi
# error paths: missing file, bad sexp
if "$TMPD/merge_sexp.bin" "$TMPD/nope.sexp" "$TMPD/o.sexp" 2>/dev/null; then echo "FAIL merge (missing ok)"; exit 1; fi
printf '(prog (fn broken' > "$TMPD/bad.sexp"
if "$TMPD/merge_sexp.bin" "$TMPD/bad.sexp" "$TMPD/o.sexp" 2>/dev/null; then echo "FAIL merge (bad ok)"; exit 1; fi
# adversarial strings: leading-space + paren content must split exactly
# (regression: ws-skipping splitters fused forms and dropped closers).
printf '(prog (fn a (tparams) () i64 (block (decl implicit s - (str 2   )) (decl implicit p - (str 1 ()) (return (var s)))) (fn b (tparams) () i64 (block (return (num 1)))) )\n' > "$TMPD/adv_a.sexp"
printf '(prog (fn c (tparams) () i64 (block (return (num 2))) ))\n' > "$TMPD/adv_b.sexp"
if ! "$TMPD/merge_sexp.bin" "$TMPD/adv_a.sexp" "$TMPD/adv.sexp" "$TMPD/adv_b.sexp" 2>/dev/null; then echo "FAIL merge (adversarial run)"; exit 1; fi
python3 - "$TMPD/adv.sexp" <<'PYEOF2'
import sys
body = open(sys.argv[1]).read().strip()
assert body.startswith('(prog ') and body.endswith(')')
inner = body[len('(prog '):-1]
n = len(inner)
forms = []
i, depth = 0, 0
start = None
while i < n:
    if inner.startswith('(str ', i):
        j = i + 5
        e = j
        while e < n and inner[e].isdigit():
            e += 1
        ln = int(inner[j:e])
        j = e + 1
        assert inner[j + ln] == ')', "string not exact"
        i = j + ln + 1
        continue
    if inner[i] == '(':
        if depth == 0:
            start = i
        depth += 1
    elif inner[i] == ')':
        depth -= 1
        assert depth >= 0
        if depth == 0:
            forms.append(inner[start:i + 1])
    i += 1
assert depth == 0, "merged output unbalanced"
assert len(forms) == 3, "want 3 forms, got %d" % len(forms)
assert '(str 2   ))' in forms[0], "space-string corrupted"
assert '(str 1 ()' in forms[0], "paren-string corrupted"
print("adversarial merge: 3 balanced forms, strings intact")
PYEOF2
[ $? -eq 0 ] || { echo "FAIL merge (adversarial check)"; exit 1; }
echo "merge: 6 passed, 0 failed"

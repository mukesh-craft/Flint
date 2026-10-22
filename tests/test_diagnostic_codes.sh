#!/usr/bin/env bash
# diagnostics gate: every diagnostic has a stable code, the codes are
# unique per message, the rendered form carries the source line, and the
# JSON form is well-formed.
#
# Why this gate exists: a diagnostic code is a PROMISE. Once E0042 means
# "expected expression after '='", it must mean that forever -- the docs
# link to it, the tools quote it, and a user greps for it. A code that
# is renumbered, reused for a different message, or missing entirely
# breaks that promise silently.
#
# Usage: bash tests/test_diagnostic_codes.sh [path/to/flintc]
set -u
FLINTC="${1:-./flintc}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "PASS $1"; }
bad() { fail=$((fail+1)); echo "FAIL $1${2:+ — $2}"; }

# --- 1. the codes in the source: unique, and one message per code ------
python3 - <<'PY' > "$WORK/codes.txt" 2>&1
import importlib.util, re, sys
# the catalogue generator knows how to read a call site's arguments and
# normalize its message into a PATTERN -- one pattern per diagnostic.
spec = importlib.util.spec_from_file_location("errcpp", "tools/errors_cpp.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
src = open('src/main.cpp').read()
calls = []
for kind, call in mod.CALLS:
    for m in re.finditer(r"\b" + call + r"\s*\(", src):
        args = mod.arg_texts(src, m.end() - 1)
        if len(args) < 2:
            continue
        code = args[0].strip()
        if 'char*' in code:
            continue    # the DECLARATION, not a call site
        if not (code.startswith('"') and code.endswith('"')):
            print("NOCODE\t%s" % args[0].replace('\n', ' ')[:60])
            continue
        code = code[1:-1]
        parts = mod.split_top_level_plus(args[1])
        pat = []
        for p in parts:
            lit = mod.literal_of(p)
            pat.append(lit if lit is not None else "<...>")
        pattern = "".join(pat).strip()
        pattern = re.sub(r"\s*<\.\.\.>\s*", " <...> ", pattern)
        pattern = re.sub(r"\s{2,}", " ", pattern).strip()
        calls.append((code, pattern))
# a code must map to exactly ONE diagnostic pattern
bycode = {}
for code, pat in calls:
    bycode.setdefault(code, set()).add(pat)
for code in sorted(bycode):
    if len(bycode[code]) > 1:
        print("DUP\t%s\t%d distinct messages" % (code, len(bycode[code])))
print("CODES\t%d call sites, %d distinct codes" % (len(calls), len(bycode)))
PY
if grep -q '^NOCODE' "$WORK/codes.txt"; then
  bad "every diagnostic call site has a code" "$(grep -c '^NOCODE' "$WORK/codes.txt") without one"
else
  ok "every diagnostic call site has a code"
fi
if grep -q '^DUP' "$WORK/codes.txt"; then
  bad "a code maps to one message" "$(grep '^DUP' "$WORK/codes.txt" | head -1)"
else
  ok "a code maps to one message"
fi
echo "  $(grep '^CODES' "$WORK/codes.txt")"

# --- 2. the code table on disk matches the source ----------------------
python3 - <<'PY' > "$WORK/table.txt" 2>&1
import csv, re, sys
src = open('src/main.cpp').read()
live = set(re.findall(r'\b(?:codegenError|parseError)\("(E\d{4})"', src))
try:
    rows = list(csv.DictReader(open('docs/error-codes.csv')))
except Exception as e:
    print("ERROR\tcannot read docs/error-codes.csv: %s" % e); sys.exit(0)
tabled = set(r['code'] for r in rows)
missing = sorted(live - tabled)
extra = sorted(tabled - live)
if missing:
    print("MISSING\t%s" % ", ".join(missing[:10]))
if extra:
    print("EXTRA\t%s" % ", ".join(extra[:10]))
print("TABLE\t%d in the source, %d in the table" % (len(live), len(tabled)))
PY
if grep -qE '^(MISSING|EXTRA)' "$WORK/table.txt"; then
  bad "the code table matches the source" "$(grep -E '^(MISSING|EXTRA)' "$WORK/table.txt" | head -1)"
else
  ok "the code table matches the source"
fi
echo "  $(grep '^TABLE' "$WORK/table.txt")"

# --- 3. the rendered form: the code, the location, the source line ----
cat > "$WORK/bad.fl" <<'FL'
fn main() {
    x = 1
    y = )
}
FL
out="$(timeout -s KILL 120 "$FLINTC" "$WORK/bad.fl" 2>&1)"
if printf '%s' "$out" | grep -qE '^error\[E[0-9]{4}\]: '; then
  ok "the rendered form carries the code"
else
  bad "the rendered form carries the code" "$(printf '%s' "$out" | head -1)"
fi
if printf '%s' "$out" | grep -qE '^\s*--> .*:[0-9]+:[0-9]+$'; then
  ok "the rendered form carries the location"
else
  bad "the rendered form carries the location" "$(printf '%s' "$out" | head -3 | tr '\n' ' ')"
fi
if printf '%s' "$out" | grep -qE '^\s*[0-9]+ \| +y = \)$'; then
  ok "the rendered form quotes the source line"
else
  bad "the rendered form quotes the source line" "$(printf '%s' "$out" | head -5 | tr '\n' ' ')"
fi
if printf '%s' "$out" | grep -qE '^\s*\| *\^'; then
  ok "the rendered form underlines the position"
else
  bad "the rendered form underlines the position" "$(printf '%s' "$out" | head -5 | tr '\n' ' ')"
fi

# --- 4. the JSON form: one object per line, and it parses --------------
json="$(timeout -s KILL 120 "$FLINTC" --error-format json "$WORK/bad.fl" 2>&1)"
printf '%s\n' "$json" > "$WORK/out.jsonl"
if python3 - "$WORK/out.jsonl" <<'PY'
import json, sys
ok = True
for i, line in enumerate(open(sys.argv[1])):
    line = line.strip()
    if not line.startswith('{'):
        continue
    try:
        d = json.loads(line)
    except Exception as e:
        print("BADJSON line %d: %s" % (i+1, e)); ok = False; continue
    for k in ('message', 'code', 'level', 'spans'):
        if k not in d:
            print("MISSINGKEY %s in line %d" % (k, i+1)); ok = False
    if d.get('code', '') and not d['code'].startswith('E'):
        print("BADCODE %r" % d.get('code')); ok = False
    if not d.get('spans'):
        print("NOSPAN in line %d" % (i+1)); ok = False
    else:
        sp = d['spans'][0]
        for k in ('file_name', 'line_start', 'column_start', 'is_primary'):
            if k not in sp:
                print("MISSINGSPANKEY %s in line %d" % (k, i+1)); ok = False
sys.exit(0 if ok else 1)
PY
then ok "the JSON form is well-formed"
else bad "the JSON form is well-formed" "see above"
fi
if [ "$(printf '%s\n' "$json" | grep -c '^{')" -ge 1 ]; then
  ok "the JSON form is emitted"
else
  bad "the JSON form is emitted" "no JSON object in the output"
fi

# --- 5. the suggestions: a typo gets a near name ------------------------
# One fixture each: the compiler reports the FIRST error and stops, so
# the two cases must not share a program.
cat > "$WORK/typo_fn.fl" <<'FL'
fn hello() -> i64 { return 1 }
fn main() {
    print(helo())
}
FL
out="$(timeout -s KILL 120 "$FLINTC" "$WORK/typo_fn.fl" 2>&1)"
if printf '%s' "$out" | grep -q 'there is a name with a similar spelling: `hello`'; then
  ok "a mistyped function gets a suggestion"
else
  bad "a mistyped function gets a suggestion" "$(printf '%s' "$out" | head -2 | tr '\n' ' ')"
fi
cat > "$WORK/typo_var.fl" <<'FL'
fn main() {
    counter = 1
    print(countr)
}
FL
outv="$(timeout -s KILL 120 "$FLINTC" "$WORK/typo_var.fl" 2>&1)"
if printf '%s' "$outv" | grep -q 'there is a name with a similar spelling: `counter`'; then
  ok "a mistyped variable gets a suggestion"
else
  bad "a mistyped variable gets a suggestion" "$(printf '%s' "$outv" | head -2 | tr '\n' ' ')"
fi
# a name that is NOT close must NOT get a suggestion (a wrong suggestion
# is worse than none)
cat > "$WORK/noty.po" <<'FL'
fn main() {
    zzzzzzzz = 1
    print(qqqqqqqq)
}
FL
out2="$(timeout -s KILL 120 "$FLINTC" "$WORK/noty.po" 2>&1)"
if printf '%s' "$out2" | grep -q 'similar spelling'; then
  bad "an unrelated name gets no suggestion" "it suggested something"
else
  ok "an unrelated name gets no suggestion"
fi

# --- 6. an unknown format is refused -----------------------------------
if timeout -s KILL 120 "$FLINTC" --error-format nope "$WORK/bad.fl" >/dev/null 2>&1; then
  bad "an unknown --error-format is refused" "it was accepted"
else
  ok "an unknown --error-format is refused"
fi

echo "diagnostic-codes: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

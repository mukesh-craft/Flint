#!/usr/bin/env bash
# Gate: the compiler's own size attribution must agree with reality.
#
# `flintc --print-size` answers "how big is this artifact and what is in
# it?" from the artifact itself. This gate checks that the answer is TRUE:
# the reported file size must match the file on disk, the section bytes must
# not exceed the file, and the JSON form must parse.
#
# A size report that lies is worse than no size report: the whole point of
# the Phase 0 measurement work is that a size change is visible where the
# change is made.
#
# Usage: bash tests/test_print_size.sh [path/to/flintc]  (default: ./flintc)
set -u
cd "$(dirname "$0")/.."
FLINTC="${1:-./flintc}"
T="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/print_size"
mkdir -p "$T"
pass=0; fail=0
ok()   { pass=$((pass+1)); }
bad()  { echo "FAIL $1"; fail=$((fail+1)); }

cat > "$T/hello.fl" <<'EOF'
fn main() {
    print("hello")
}
EOF

# A program that pulls in more of the standard library, so the report has
# something to attribute.
cat > "$T/bigger.fl" <<'EOF'
fn main() {
    s = "abc"
    print(s)
}
EOF

for src in hello bigger; do
    out="$T/$src.bin"
    json="$T/$src.json"
    rm -f "$out" "$json"
    if ! "$FLINTC" "$T/$src.fl" --print-size --print-size-json="$json" -o "$out" > "$T/$src.report" 2>&1; then
        bad "$src: compile failed"
        continue
    fi
    if [ ! -f "$out" ]; then bad "$src: no artifact"; continue; fi
    real=$(stat -c %s "$out")
    reported=$(awk '/file bytes:/ {print $3}' "$T/$src.report")
    if [ -z "$reported" ]; then bad "$src: no file size in the report"; continue; fi
    if [ "$reported" != "$real" ]; then
        bad "$src: reported $reported bytes, file is $real"
        continue
    fi
    ok
    # Sections must fit inside the file.
    sec=$(awk '/section bytes:/ {print $3}' "$T/$src.report")
    if [ -n "$sec" ] && [ "$sec" -gt "$real" ]; then
        bad "$src: section bytes ($sec) exceed the file size ($real)"
        continue
    fi
    ok
    # The dependency list must be present and mention libc.
    if ! grep -q "dynamic deps:" "$T/$src.report"; then
        bad "$src: no dependency list"; continue
    fi
    if ! grep -q "libc" "$T/$src.report"; then
        bad "$src: dependency list does not mention libc"; continue
    fi
    ok
    # The JSON form must parse and agree with the text form.
    if ! python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert d['file_bytes']>0; assert 'sections' in d and 'needed' in d" "$json" 2>/dev/null; then
        bad "$src: JSON report does not parse or is missing keys"
        continue
    fi
    ok
    jbytes=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['file_bytes'])" "$json")
    if [ "$jbytes" != "$real" ]; then
        bad "$src: JSON says $jbytes bytes, file is $real"
        continue
    fi
    ok
done

# A program that does NOT exist must fail loudly, not silently.
if "$FLINTC" "$T/nope.fl" --print-size -o "$T/nope.bin" >/dev/null 2>&1; then
    bad "missing input was accepted"
else
    ok
fi

echo "print-size: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

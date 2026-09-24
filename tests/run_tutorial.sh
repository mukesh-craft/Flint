#!/usr/bin/env bash
# Tutorial runner: every tutorial/0*.fl must exit 0 and print its EXPECT block.
# Usage: bash tests/run_tutorial.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
pass=0; fail=0
for f in tutorial/0*.fl; do
    name="$(basename "$f")"
    # expected output = trailing EXPECT comment block, '// ' prefix stripped
    want="$(awk '/^\/\/ EXPECT:$/{flag=1;next} flag{sub(/^\/\//,""); sub(/^ /,""); print}' "$f")"
    got="$("$FLINTC" "$f" 2>&1)"
    rc=$?
    if [ $rc -ne 0 ]; then echo "FAIL $name (exit $rc)"; echo "$got" | head -n 3; fail=$((fail+1)); continue; fi
    if [ "$got" = "$want" ]; then pass=$((pass+1));
    else echo "FAIL $name (output mismatch)"; echo "--- want ---"; printf '%s\n' "$want"; echo "--- got ---"; printf '%s\n' "$got" | head -n 12; fail=$((fail+1)); fi
done
echo "tutorial: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

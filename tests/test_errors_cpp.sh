#!/usr/bin/env bash
# errors gate (C++ compiler): every diagnostic src/main.cpp can print must be
# catalogued in docs/errors-cpp.md, and every catalogued diagnostic must
# exist in the source -- so a new message cannot ship undocumented, and a
# removed message cannot linger in the docs.
#
# The self-hosted emitter's diagnostics are catalogued separately
# (docs/errors.md, tests/test_errors.sh).
#
# Usage: bash tests/test_errors_cpp.sh          # gate only
#        bash tests/test_errors_cpp.sh --fix    # regenerate the list section
set -u
cd "$(dirname "$0")/.."

if [ "${1:-}" = "--fix" ]; then
    python3 - <<'PYFIX'
import re, subprocess
pats = subprocess.run(['python3', 'tools/errors_cpp.py'], capture_output=True,
                       text=True).stdout.strip().split('\n')
pats = [p for p in pats if not p.startswith('Total:')]
doc = open('docs/errors-cpp.md').read()
head = doc.split('## Complete message list', 1)[0]
body = "\n".join("- %s%s%s" % ("``" if "`" in p else "`", p, "``" if "`" in p else "`")
                 for p in sorted(pats))
open('docs/errors-cpp.md', 'w').write(
    head + '## Complete message list\n\n' + body
    + "\n\nTotal: %d diagnostics.\n" % len(pats))
print("errors-cpp-catalog: regenerated %d diagnostics" % len(pats))
PYFIX
    exit 0
fi

python3 - <<'PYEOF'
import re, subprocess, sys
out = subprocess.run(['python3', 'tools/errors_cpp.py'], capture_output=True, text=True).stdout
emitted = set(p for p in out.strip().split('\n') if p and not p.startswith('Total:'))
doc = open('docs/errors-cpp.md').read()
documented = set(re.findall(r'^- (?:``|`)(.+?)(?:``|`)$', doc, re.M))
missing = sorted(emitted - documented)
extra = sorted(documented - emitted)
if missing:
    print("undocumented diagnostics (run: bash tests/test_errors_cpp.sh --fix):")
    for m in missing:
        print("  - " + m)
if extra:
    print("documented but never emitted:")
    for m in extra:
        print("  - " + m)
if missing or extra:
    sys.exit(1)
print("errors-cpp-catalog: %d/%d in sync" % (len(emitted), len(emitted)))
PYEOF

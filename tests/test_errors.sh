#!/usr/bin/env bash
# errors gate: every diagnostic stage3/flint_emit.fl can print must be
# catalogued in docs/errors.md (and vice versa) — no silent new errors.
# Usage: bash tests/test_errors.sh
set -u
python3 - <<'PYEOF'
import re
import sys
src = open('stage3/flint_emit.fl').read()
emitted = set(re.findall(r'print\("((?:emit error|codegen)[^"]*)"\)', src))
doc = open('docs/errors.md').read()
documented = set(re.findall(r'`((?:emit error|codegen)[^`]*)`', doc))
missing = sorted(emitted - documented)
extra = sorted(documented - emitted)
if missing:
    print("undocumented diagnostics:")
    for m in missing:
        print("  - " + m)
if extra:
    print("documented but never emitted:")
    for m in extra:
        print("  - " + m)
if missing or extra:
    sys.exit(1)
print("errors-catalog: %d/%d in sync" % (len(emitted), len(documented)))
PYEOF

#!/usr/bin/env bash
# errors gate: every diagnostic stage3/flint_emit.fl can print must be
# catalogued in docs/errors.md (and vice versa) — no silent new errors.
# Usage: bash tests/test_errors.sh          # gate only
#        bash tests/test_errors.sh --fix    # regenerate the catalog section
set -u
if [ "${1:-}" = "--fix" ]; then
python3 - <<'PYFIX'
import re
src = open('stage3/flint_emit.fl').read()
emitted = sorted(set(re.findall(r'print\("((?:emit error|codegen)[^"]*)"\)', src)))
doc = open('docs/errors.md').read()
head, _ = doc.split('## Complete message list', 1)
old = re.findall(r'^- `((?:emit error|codegen)[^`]*)`$',
                 doc.split('## Complete message list', 1)[1], re.M)
# The lambda family stays first (curated grouping); the rest is sorted.
family = old[:11]
block = "\n".join("- `%s`" % m for m in family + sorted(set(old[11:]) | set(emitted)))
open('docs/errors.md', 'w').write(
    head + '## Complete message list\n\n' + block
    + "\n\nTotal: %d messages.\n" % len(emitted))
print("errors-catalog: regenerated %d messages" % len(emitted))
PYFIX
    exit $?
fi
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

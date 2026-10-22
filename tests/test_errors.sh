#!/usr/bin/env bash
# errors gate: every diagnostic stage3/emitter.fl can print must be
# catalogued in docs/errors.md (and vice versa) — no silent new errors.
# Usage: bash tests/test_errors.sh          # gate only
#        bash tests/test_errors.sh --fix    # regenerate the catalog section
set -u
if [ "${1:-}" = "--fix" ]; then
python3 - <<'PYFIX'
import re
src = open('stage3/emitter.fl').read()
emitted = sorted(set(re.findall(r'print\("((?:emit error|codegen)[^"]*)"\)', src)))
doc = open('docs/errors.md').read()
head, _ = doc.split('## Complete message list', 1)
old = re.findall(r'^- `((?:emit error|codegen)[^`]*)`$',
                 doc.split('## Complete message list', 1)[1], re.M)
# The lambda family stays first (curated grouping); the rest is sorted.
# A message that the emitter no longer prints is DROPPED: the catalog
# must mirror the emitter exactly, in both directions.
# The curated lambda family stays first. Its entries are re-matched to the
# emitted set by their TEXT (a code was added to every message, so the old
# text and the new text differ by the code) -- otherwise the family would
# keep the pre-code spellings and the gate would call them dead.
def without_code(m):
    # "emit error[E0123]: text" -> "emit error: text" (the code goes away)
    if '[' in m and ']' in m and m.index(']') < m.index(':'):
        i = m.index('[')
        j = m.index(']')
        return m[:i] + m[j+1:]
    return m
by_text = {without_code(m): m for m in emitted}
family = []
for m in old[:11]:
    t = without_code(m)
    family.append(by_text[t] if t in by_text else m)
rest = (set(old[11:]) & set(emitted)) | set(emitted)
block = "\n".join("- `%s`" % m for m in family + sorted(rest))
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
src = open('stage3/emitter.fl').read()
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

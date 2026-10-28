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
import csv, re, subprocess
pats = subprocess.run(['python3', 'tools/errors_cpp.py'], capture_output=True,
                       text=True).stdout.strip().split('\n')
pats = [p for p in pats if not p.startswith('Total:')]
doc = open('docs/errors-cpp.md').read()
head = doc.split('## Complete message list', 1)[0]
body = "\n".join("- %s%s%s" % ("``" if "`" in p else "`", p, "``" if "`" in p else "`")
                 for p in sorted(pats))
rows = list(csv.DictReader(open('docs/error-codes.csv')))
n_codes = len(rows)
open('docs/errors-cpp.md', 'w').write(
    head + '## Complete message list\n\n' + body
    + "\n\nTotal: %d fixed-pattern diagnostics (%d stable codes; the other %d build "
      "their message at runtime and are listed as `<variable message>` in "
      "docs/error-codes.csv).\n" % (len(pats), n_codes, n_codes - len(pats)))
# Keep the machine-readable PATTERNS in sync too. `pats` lines are
# "E0042  literal <...> pattern"; the variable-message codes are left alone.
fixed = {}
for line in pats:
    code, sep, pat = line.partition('  ')
    if sep and re.fullmatch(r'E\d{4}', code):
        fixed[code] = pat
for r in rows:
    if r['code'] in fixed:
        r['message_pattern'] = fixed[r['code']]
with open('docs/error-codes.csv', 'w', newline='') as f:
    f.write('code,message_pattern\n')
    for r in rows:
        f.write('%s,"%s"\n' % (r['code'], r['message_pattern'].replace('"', '""')))
print("errors-cpp-catalog: regenerated %d fixed-pattern diagnostics, %d codes"
      % (len(pats), n_codes))
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

# The CSV is the machine-readable code table. Checking only the CODE SET
# (tests/test_diagnostic_codes.sh) lets a PATTERN drift silently: E0001 and
# E0048 were wrong in the CSV while every gate stayed green. Compare the
# pattern text too.
import csv
pats = {}
for line in out.strip().split('\n'):
    code, sep, pat = line.partition('  ')
    if sep and re.fullmatch(r'E\d{4}', code):
        pats[code] = pat
rows = list(csv.DictReader(open('docs/error-codes.csv')))
csv_patterns = {r['code']: r['message_pattern'] for r in rows}
drift = [(c, csv_patterns.get(c), p) for c, p in pats.items()
         if csv_patterns.get(c) != p]
if drift:
    print("CSV pattern drift (%d):" % len(drift))
    for c, got, want in drift[:10]:
        print("  %s\n    csv:  %r\n    want: %r" % (c, got, want))
    sys.exit(1)
print("errors-cpp-catalog: %d/%d in sync (md + csv patterns)" % (len(emitted), len(emitted)))
PYEOF

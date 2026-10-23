#!/usr/bin/env bash
# Gate: the documentation stays navigable and non-repetitive.
#
# Two properties are checked over every Markdown file in the tree:
#
#   1. every RELATIVE link resolves (a dangling link is a dead pointer for
#      the next session);
#   2. no two live documents repeat a paragraph. The rule is in
#      docs/INDEX.md: one fact, one home. `memory.md` is exempt -- it is the
#      chronological ledger, and by definition it contains the text of past
#      decisions, not current status. Archived reports (IMPROVEMENT-PLAN)
#      are exempt for the same reason.
#
# Usage: bash tests/test_docs.sh
set -u
cd "$(dirname "$0")/.."

python3 - <<'PY'
import os, re, sys

ROOT = os.getcwd()
DOCS = []
for base in ('.', 'docs'):
    for name in sorted(os.listdir(base)):
        if name.endswith('.md') and os.path.isfile(os.path.join(base, name)):
            DOCS.append(os.path.normpath(os.path.join(base, name)))

# --- 1. links -------------------------------------------------------------
link_re = re.compile(r'!?\[[^\]]*\]\(([^)]+)\)')
bad = []
checked = 0
for doc in DOCS:
    text = open(doc, encoding='utf-8').read()
    for raw in link_re.findall(text):
        target = raw.strip().split()[0].strip('<>')
        if not target or target.startswith(('#', 'http://', 'https://', 'mailto:')):
            continue
        target = target.split('#', 1)[0]
        if not target:
            continue
        path = os.path.normpath(os.path.join(os.path.dirname(doc), target))
        checked += 1
        if not os.path.exists(path):
            bad.append(f'{doc}: {raw} -> {path}')
if bad:
    print(f'FAIL: {len(bad)} dangling relative link(s)')
    for b in bad:
        print('  ' + b)
    sys.exit(1)
print(f'ok: {checked} relative links resolve')

# --- 2. INDEX lists every document ---------------------------------------
index = open('docs/INDEX.md', encoding='utf-8').read()
missing = [d for d in DOCS
           if d != 'docs/INDEX.md' and os.path.basename(d) not in index]
if missing:
    print('FAIL: docs/INDEX.md does not list: ' + ', '.join(missing))
    sys.exit(1)
print(f'ok: docs/INDEX.md lists all {len(DOCS) - 1} other documents')

# --- 3. paragraph duplication --------------------------------------------
EXEMPT = {'memory.md', 'docs/IMPROVEMENT-PLAN.md'}

def paragraphs(path):
    text = open(path, encoding='utf-8').read()
    blocks = [b.strip() for b in re.split(r'\n\s*\n', text)]
    return [b for b in blocks if len(b.split()) >= 25]

def shingles(text, n=8):
    words = re.findall(r'[a-z0-9_]+', text.lower())
    return {tuple(words[i:i + n]) for i in range(max(0, len(words) - n + 1))}

live = [d for d in DOCS if d not in EXEMPT]
paras = {d: paragraphs(d) for d in live}
dups = []
for i, a in enumerate(live):
    for b in live[i + 1:]:
        for ia, pa in enumerate(paras[a]):
            sa = shingles(pa)
            if not sa:
                continue
            for ib, pb in enumerate(paras[b]):
                sb = shingles(pb)
                if not sb:
                    continue
                overlap = len(sa & sb) / min(len(sa), len(sb))
                if overlap >= 0.55:
                    dups.append((overlap, a, ia, b, ib, pa.splitlines()[0]))
if dups:
    dups.sort(reverse=True)
    print(f'FAIL: {len(dups)} repeated paragraph(s) between live documents')
    for ov, a, ia, b, ib, head in dups:
        print(f'  {ov:.2f} {a}:{ia} <-> {b}:{ib}  {head[:80]}')
    print('  move the fact to its owner (docs/INDEX.md) and link instead')
    sys.exit(1)
print(f'ok: no repeated paragraphs across {len(live)} live documents')
PY

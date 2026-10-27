#!/usr/bin/env python3
"""Check that cqma found every injected defect in a fixture.

Usage: verify.py <source.cpp> <report.json>

Each defect is tagged [Dn]/[Ln] in the source. The tag index selects the
detector class that must have fired near that line. Exit code is non-zero if
any defect is missing, so this doubles as a regression gate.
"""
import json
import re
import sys

# index -> (label, regex over "<tool>:<check>")
EXPECTED = {
    1: ("unused function", r"(cppcheck:unusedFunction|compiler:-Wunused-function)"),
    2: ("null dereference", r"(clang-analyzer:core\.NullDereference|cppcheck:nullPointer)"),
    3: ("buffer overflow", r"(cppcheck:bufferAccessOutOfBounds|cppcheck:arrayIndexOutOfBounds|compiler:-Wfortify-source)"),
    4: ("double free", r"(cppcheck:doubleFree|clang-analyzer:cplusplus\.NewDelete)"),
    5: ("use after free", r"(cppcheck:deallocret|clang-analyzer:cplusplus\.NewDelete)"),
    6: ("shadowing", r"(compiler:-Wshadow|cppcheck:shadowArgument|cppcheck:shadowVariable)"),
    7: ("sign conversion", r"(compiler:-Wsign-conversion|compiler:-Wshorten-64-to-32|compiler:-Wconversion)"),
    8: ("uninitialized read", r"(compiler:-Wsometimes-uninitialized|compiler:-Wuninitialized|cppcheck:uninitvar|clang-analyzer:core\.uninitialized)"),
    9: ("runtime heap leak", r"leak-tracker:leak"),
    10: ("static path leak", r"(cppcheck:memleak|clang-analyzer:cplusplus\.NewDeleteLeaks|clang-analyzer:unix\.Malloc)"),
}


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, report = sys.argv[1], sys.argv[2]
    lines = open(src).read().splitlines()
    markers = {}
    for i, line in enumerate(lines, 1):
        m = re.search(r"\[[DL](\d+)\]", line)
        if m:
            markers[int(m.group(1))] = i

    findings = json.load(open(report))["findings"]
    keyed = [
        (f.get("line") or 0, f["tool"] + ":" + f["check"], f)
        for f in findings
    ]

    failures = 0
    print(f"{'tag':4} {'line':>5}  {'defect':22} result")
    print("-" * 78)
    for idx in sorted(markers):
        line = markers[idx]
        label, pattern = EXPECTED[idx]
        hits = [
            (ln, key, f)
            for ln, key, f in keyed
            if abs(ln - line) <= 5 and re.search(pattern, key)
        ]
        if hits:
            ln, key, f = hits[0]
            print(f"D{idx:<3} {line:>5}  {label:22} FOUND  {key} @ {ln}")
        else:
            near = [
                key for ln, key, _ in keyed if abs(ln - line) <= 5
            ]
            print(f"D{idx:<3} {line:>5}  {label:22} MISSING (near: {', '.join(near) or 'none'})")
            failures += 1

    print("-" * 78)
    total = len(markers)
    print(f"{total - failures}/{total} injected defects detected")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

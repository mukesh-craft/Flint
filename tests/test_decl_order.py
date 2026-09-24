#!/usr/bin/env python3
"""Declaration-order tracker for Flint sources.
C++ resolves method calls (.set/.get/.has) by declaration order: a global
used with a method inside fn F must be declared textually BEFORE F, else
the call silently falls back to `fa_*` and fails closed at codegen
("undefined function"). This checker fails loudly on any violation.

Usage: python3 tests/test_decl_order.py stage1/flint_lex.fl stage2/flint_parse.fl stage3/flint_emit.fl [more...]
Exit 0 = all method receivers declared before first use.
"""
import re
import sys

# A method call `recv.name(` where recv must be a known map/vec/sb global.
METHODS = {"set", "get", "has"}


def strip_strings_comments(line):
    out = []
    instr = False
    i = 0
    while i < len(line):
        ch = line[i]
        if instr:
            if ch == "\\":
                i += 2
                continue
            elif ch == '"':
                instr = False
        elif ch == '"':
            instr = True
        elif ch == "#":
            break
        else:
            out.append(ch)
        i += 1
    return "".join(out)


def main(paths):
    bad = 0
    for path in paths:
        lines = open(path).read().split("\n")
        # top-level globals: NAME = ... or mut NAME ... at column 0
        gdecl = {}
        curfn = None
        for idx, ln in enumerate(lines):
            m = re.match(r"^(mut\s+)?([A-Za-z_][A-Za-z_0-9]*)\s*=", ln)
            if m and not ln.startswith(" ") and not ln.startswith("\t"):
                gdecl[m.group(2)] = idx + 1
            fm = re.match(r"^fn\s+", ln)
            if fm:
                curfn = (idx + 1)
            for mm in re.finditer(r"([A-Za-z_][A-Za-z_0-9]*)\.(set|get|has)\(", strip_strings_comments(ln)):
                recv = mm.group(1)
                if recv in gdecl and curfn is not None and gdecl[recv] > curfn:
                    # receiver declared AFTER current function: use-before-decl
                    # (only matters if receiver is a global, i.e. in gdecl)
                    print(f"{path}:{idx+1}: use-before-decl: {recv}.{mm.group(2)} "
                          f"in fn at line {curfn}, global declared at line {gdecl[recv]}")
                    bad += 1
    if bad:
        print(f"decl-order: {bad} violation(s)")
        return 1
    print("decl-order: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

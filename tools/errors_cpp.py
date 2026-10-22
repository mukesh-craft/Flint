#!/usr/bin/env python3
"""Extract the C++ compiler's diagnostics as PATTERNS.

The compiler builds many messages by concatenation ("argument 2 of 'f' has
type ..."). A catalog of literal fragments is useless to a user searching
for a message, so each call site is rendered as a pattern: the literal
parts in order, with `<...>` where a value is interpolated.

Usage:
    python3 tools/errors_cpp.py            # print the catalog (stdout)
    python3 tools/errors_cpp.py --format md
"""
import re
import sys

SRC = "src/main.cpp"

# Diagnostic call sites. Each is (name, pattern-for-the-first-argument).
CALLS = [
    ("codegenError", r"codegenError"),
    ("parseError", r"parseError"),
]


def split_top_level_plus(text):
    """Split an expression on top-level '+' (not inside a string or parens)."""
    parts, buf, depth, i, in_str = [], [], 0, 0, False
    while i < len(text):
        c = text[i]
        if in_str:
            buf.append(c)
            if c == "\\" and i + 1 < len(text):
                buf.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            buf.append(c)
            i += 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == "+" and depth == 0:
            parts.append("".join(buf))
            buf = []
            i += 1
            continue
        buf.append(c)
        i += 1
    parts.append("".join(buf))
    return [p.strip() for p in parts]


def literal_of(part):
    """Return the literal text of a part, or None if it is not a literal.

    `std::string("literal")` IS a literal -- the wrapper is only a way to
    build one. Unwrapping it matters: without it, three different
    diagnostics ("undefined var 'x'", "cannot borrow moved variable 'x'",
    ...) collapse into the same pattern, and a diagnostic code (which is
    assigned per pattern) would be shared by messages that are not the
    same.
    """
    part = part.strip()
    m = re.fullmatch(r'"((?:[^"\\]|\\.)*)"', part)
    if m:
        return m.group(1)
    m = re.fullmatch(r'std::string\(\s*"((?:[^"\\]|\\.)*)"\s*\)', part)
    if m:
        return m.group(1)
    return None


def arg_text(src, open_paren_idx):
    """Return the text of the first argument, respecting nesting/quotes."""
    depth, i, in_str = 0, open_paren_idx, False
    while i < len(src):
        c = src[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            i += 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                return src[open_paren_idx + 1:i]
        i += 1
    return ""


def arg_texts(src, open_paren_idx):
    """Return the texts of every top-level argument of a call.

    A diagnostic call is `codegenError("E0042", "message" + x)`: the first
    argument is the CODE and the second is the message.
    """
    # The scan starts AFTER the opening paren, so the depth is 0 for the
    # arguments themselves.
    depth, i, in_str, args, cur = 0, open_paren_idx + 1, False, [], []
    while i < len(src):
        c = src[i]
        if in_str:
            if c == "\\":
                cur.append(src[i:i+2]); i += 2; continue
            if c == '"':
                in_str = False
            cur.append(c); i += 1; continue
        if c == '"':
            in_str = True; cur.append(c); i += 1; continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                args.append("".join(cur)); return args
            depth -= 1
        elif c == "," and depth == 0:
            args.append("".join(cur)); cur = []; i += 1; continue
        cur.append(c); i += 1
    args.append("".join(cur))
    return args


def patterns(src):
    """Yield (pattern -> {kinds, codes}) for every diagnostic call site."""
    out = {}
    for kind, call in CALLS:
        for m in re.finditer(r"\b" + call + r"\s*\(", src):
            args = arg_texts(src, m.end() - 1)
            if len(args) < 2:
                continue
            code = args[0].strip()
            if not (code.startswith('"') and code.endswith('"')):
                continue
            code = code[1:-1]
            text = args[1]
            if not text:
                continue
            parts = split_top_level_plus(text)
            pat = []
            for p in parts:
                lit = literal_of(p)
                if lit is not None:
                    pat.append(lit)
                else:
                    # An interpolated value. Collapse a leading std::string(
                    # wrapper so the placeholder stays readable.
                    pat.append("<...>")
            pattern = "".join(pat).strip()
            if not pattern or pattern == "<...>":
                continue
            # Normalize: a placeholder in the middle of a word reads better
            # with spaces around it.
            pattern = re.sub(r"\s*<\.\.\.>\s*", " <...> ", pattern)
            pattern = re.sub(r"\s{2,}", " ", pattern).strip()
            entry = out.setdefault(pattern, {"kinds": set(), "codes": set()})
            entry["kinds"].add(kind)
            entry["codes"].add(code)
    return out


def main():
    src = open(SRC).read()
    pats = patterns(src)
    md = "--format md" in sys.argv
    if md:
        print("<!-- Generated by tools/errors_cpp.py; do not edit by hand. -->")
        print("<!-- Regenerate: python3 tools/errors_cpp.py --format md -->\n")
        for p in sorted(pats):
            # A pattern that itself contains a backtick needs the double
            # backtick fence, or the code span would end early.
            fence = "``" if "`" in p else "`"
            codes = sorted(pats[p]["codes"])
            code = codes[0] if codes else ""
            print("- %s%s%s %s" % (fence, code, fence, fence + p + fence))
        print("\nTotal: %d diagnostics." % len(pats))
    else:
        for p in sorted(pats):
            codes = sorted(pats[p]["codes"])
            print((codes[0] + " ") if codes else "", p)
        print("Total: %d" % len(pats))


if __name__ == "__main__":
    main()

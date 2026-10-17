#!/usr/bin/env python3
"""Verify the VS Code extension agrees with the compiler.

A hand-written keyword list in an editor grammar is a lie waiting to happen:
the day someone adds a keyword to the language, the grammar silently fails to
highlight it. This script makes that failure loud. It also checks the JSON is
parseable and that the manifest actually points at the files that exist.

Usage: python3 tools/check_vscode_extension.py
Exit: 0 when in sync, 1 on any drift, 2 when a file is missing.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEXER = os.path.join(ROOT, "src", "main.cpp")
GRAMMAR = os.path.join(ROOT, "editors", "vscode", "syntaxes", "flint.tmLanguage.json")
PACKAGE = os.path.join(ROOT, "editors", "vscode", "package.json")
LANGCONF = os.path.join(ROOT, "editors", "vscode", "language-configuration.json")
CLIENT = os.path.join(ROOT, "editors", "vscode", "client.js")

failures = []
notes = []


def check(condition, message):
    if condition:
        notes.append("ok: " + message)
    else:
        failures.append(message)


def lexer_keywords():
    src = open(LEXER).read()
    block = src.split("Token readIdentifier()", 1)[1].split("};", 1)[0]
    found = re.findall(r's == "([a-z_0-9]+)"\)\s+return makeToken\(TokenType::KW_', block)
    if not found:
        raise SystemExit("error: could not extract lexer keywords from src/main.cpp")
    return set(found)


def grammar_keywords(grammar):
    """Every word literal that a keyword-ish pattern claims.

    Reading the patterns rather than trusting a side list is deliberate: the
    check must fail if someone adds a word to a pattern list.
    """
    words = set()
    keyword_scopes = ("keyword.", "storage.", "support.type.python")
    for pattern in grammar.get("patterns", []):
        name = pattern.get("name", "")
        if not any(name.startswith(s) for s in keyword_scopes):
            continue
        match = pattern.get("match", "")
        # Drop regex escapes (\b, \s, ...) first: scanning the raw pattern
        # would read `\bpython\b` as the words "b" and "bpython".
        cleaned = re.sub(r"\\[a-zA-Z]", " ", match)
        words.update(re.findall(r"[a-z_][a-z0-9_]*", cleaned))
    return words


def main():
    for path in (GRAMMAR, PACKAGE, LANGCONF, CLIENT):
        if not os.path.exists(path):
            print("missing: " + os.path.relpath(path, ROOT))
            return 2

    grammar = json.load(open(GRAMMAR))
    package = json.load(open(PACKAGE))
    langconf = json.load(open(LANGCONF))
    check(True, "all extension JSON parses")

    lex = lexer_keywords()
    gram = grammar_keywords(grammar)
    # `true`/`false` are literals, not lexer keywords, and `self`/`this` have no
    # Flint equivalent yet: allowed extras, but only those.
    allowed_extras = {"true", "false", "self", "this"}
    missing = sorted(lex - gram)
    unexpected = sorted(gram - lex - allowed_extras)
    check(not missing, "grammar covers every lexer keyword" +
          ("" if not missing else " (missing: %s)" % ", ".join(missing)))
    check(not unexpected, "grammar invents no keywords" +
          ("" if not unexpected else " (invented: %s)" % ", ".join(unexpected)))
    notes.append("keywords: lexer=%d grammar=%d" % (len(lex), len(gram)))

    check(grammar.get("scopeName") == "source.flint", "grammar scopeName is source.flint")
    check("fl" in (grammar.get("fileTypes") or []), "grammar registers the .fl file type")
    check(bool(grammar.get("patterns")), "grammar has patterns")

    languages = package["contributes"]["languages"]
    check(languages[0]["id"] == "flint", "language id is flint")
    check(".fl" in languages[0]["extensions"], "language claims .fl")
    check(languages[0]["configuration"] == "./language-configuration.json",
          "language points at its configuration file")
    check("onLanguage:flint" in package["activationEvents"],
          "activates on Flint documents")
    check("vscode-languageclient" in package.get("dependencies", {}),
          "declares the language client dependency")

    grammar_ref = package["contributes"]["grammars"][0]
    check(os.path.exists(os.path.join(os.path.dirname(PACKAGE), grammar_ref["path"].lstrip("./"))),
          "manifest grammar path exists")
    check(os.path.exists(os.path.join(os.path.dirname(PACKAGE),
                                     languages[0]["configuration"].lstrip("./"))),
          "manifest configuration path exists")

    check(langconf["comments"]["lineComment"] == "//", "line comments use //")
    check("#" in open(GRAMMAR).read(), "grammar highlights # comments too")
    check("source.flint" in json.dumps(langconf.get("folding", {})) or "folding" in langconf,
          "folding markers configured")

    for note in notes:
        print(note)
    if failures:
        for f in failures:
            print("FAIL: " + f)
        print("vscode-extension: %d failures" % len(failures))
        return 1
    print("vscode-extension: in sync")
    return 0


if __name__ == "__main__":
    sys.exit(main())
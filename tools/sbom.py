#!/usr/bin/env python3
"""Generate a minimal SPDX-2.3-style SBOM for the Flint release bundle.

Scans the repo for release components (binaries, runtime, self-hosted
compiler sources, driver/tools, docs), records version (VERSION file),
file size + sha256, and LLVM pin (driver/VERSIONS). Emits JSON to stdout.

Usage: python3 tools/sbom.py > sbom.json
"""
import hashlib
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def version():
    try:
        with open(os.path.join(ROOT, "VERSION")) as f:
            return f.read().strip()
    except OSError:
        return "0.0.0-unknown"


def llvm_pin():
    pin = "unknown"
    try:
        with open(os.path.join(ROOT, "driver", "VERSIONS")) as f:
            for line in f:
                stripped = line.strip()
                if stripped.startswith("#") or not stripped:
                    continue
                if "LLVM" in stripped:
                    pin = stripped
                    break
    except OSError:
        pass
    return pin


COMPONENTS = [
    ("flintc", "flintc", "native compiler binary (C++/LLVM)"),
    ("runtime", "runtime/runtime.c", "portable C runtime"),
    ("stage1-lexer", "stage1/flint_lex.fl", "self-hosted lexer"),
    ("stage2-parser", "stage2/flint_parse.fl", "self-hosted parser"),
    ("stage3-emitter", "stage3/flint_emit.fl", "self-hosted emitter"),
    ("driver", "driver/flintc.fl", "pipeline driver"),
    ("merge-tool", "tools/merge_sexp.fl", "S-expr merger"),
    ("formatter", "flint-fmt", "source formatter"),
    ("fuzz-generator", "fuzz/generate.py", "differential fuzz generator"),
]


def main():
    ver = version()
    packages = []
    for name, rel, desc in COMPONENTS:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            continue
        packages.append({
            "name": "flint-" + name,
            "version": ver,
            "description": desc,
            "file": rel,
            "size_bytes": os.path.getsize(path),
            "sha256": sha256_of(path),
        })
    sbom = {
        "spdxVersion": "SPDX-2.3",
        "name": "flint",
        "documentVersion": ver,
        "toolchain": llvm_pin(),
        "packages": packages,
    }
    print(json.dumps(sbom, indent=2))


if __name__ == "__main__":
    main()

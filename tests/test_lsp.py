#!/usr/bin/env python3
"""Protocol-level LSP client for the gate.

Drives the real server binary over stdio with real JSON-RPC framing: this is
the only test that proves the wire contract end to end (unit tests inside the
crate never touch the process boundary, and a server that mis-frames hangs a
real editor).
"""

import json
import os
import subprocess
import sys
import time

SERVER = sys.argv[1]
FLINTC = sys.argv[2]
URI = "file:///gate/probe.fl"

BROKEN = 'fn main() {\n    nope_undefined()\n}\n'
CLEAN = (
    "# Adds two numbers.\n"
    "fn add(a: i64, b: i64) -> i64 {\n"
    "    return a + b\n"
    "}\n"
    "\n"
    "fn main() {\n"
    "    print(add(1, 2))\n"
    "    print(add(20, 22))\n"
    "}\n"
)

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(("PASS " if ok else "FAIL ") + name + ((" — " + detail) if detail and not ok else ""))


class Client:
    def __init__(self, env_extra=None):
        env = dict(os.environ)
        env["FLINT_LSP_FLINTC"] = FLINTC
        if env_extra:
            env.update(env_extra)
        self.p = subprocess.Popen(
            [SERVER], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=env)
        self.next_id = 0

    def send(self, payload):
        body = json.dumps(payload).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.p.stdin.flush()

    def request(self, method, params, timeout=25):
        self.next_id += 1
        rid = self.next_id
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = self.read_message(timeout=max(0.2, deadline - time.time()))
            if msg is None:
                continue
            if msg.get("id") == rid:
                return msg
        return None

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def read_message(self, timeout=25):
        deadline = time.time() + timeout
        header = b""
        while b"\r\n\r\n" not in header:
            if time.time() > deadline:
                return None
            ch = self.p.stdout.read(1)
            if not ch:
                return None
            header += ch
        length = None
        for line in header.decode(errors="replace").split("\r\n"):
            if line.lower().startswith("content-length:"):
                length = int(line.split(":", 1)[1].strip())
        if length is None:
            return None
        body = b""
        while len(body) < length:
            chunk = self.p.stdout.read(length - len(body))
            if not chunk:
                return None
            body += chunk
        return json.loads(body.decode())

    def wait_diagnostics(self, uri, timeout=40):
        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = self.read_message(timeout=max(0.2, deadline - time.time()))
            if msg and msg.get("method") == "textDocument/publishDiagnostics":
                if msg["params"]["uri"] == uri:
                    return msg["params"]["diagnostics"]
        return None

    def close(self):
        try:
            self.request("shutdown", None, timeout=5)
            self.notify("exit", None)
        except Exception:
            pass
        try:
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


def main():
    c = Client()
    init = c.request("initialize", {"processId": None, "rootUri": None, "capabilities": {}})
    check("initialize responds", init is not None and "result" in (init or {}))
    caps = (init or {}).get("result", {}).get("capabilities", {})
    for cap in ("hoverProvider", "definitionProvider", "referencesProvider", "documentSymbolProvider"):
        check("capability %s advertised" % cap, caps.get(cap) is True, str(caps.get(cap)))
    check("completionProvider advertised", isinstance(caps.get("completionProvider"), dict))
    check("textDocumentSync full sync", (caps.get("textDocumentSync") or {}).get("change") == 1)
    check("serverInfo names the server",
          (init or {}).get("result", {}).get("serverInfo", {}).get("name") == "flint-lsp-rs")

    # Unknown method must produce a JSON-RPC error, not silence.
    unknown = c.request("textDocument/rename", {
        "textDocument": {"uri": URI}, "position": {"line": 0, "character": 0},
        "newName": "x"})
    check("unknown method -> -32601",
          unknown is not None and unknown.get("error", {}).get("code") == -32601,
          json.dumps(unknown)[:160] if unknown else "no response")

    # Broken document must publish a diagnostic naming the real problem.
    c.notify("textDocument/didOpen", {"textDocument": {"uri": URI, "text": BROKEN, "version": 1}})
    diags = c.wait_diagnostics(URI)
    check("broken document publishes diagnostics", bool(diags), repr(diags))
    if diags:
        joined = " ".join(d["message"] for d in diags)
        check("diagnostic names the undefined function", "nope_undefined" in joined, joined[:160])
        d0 = diags[0]
        check("diagnostic range is 0-based {line,char}",
              isinstance(d0["range"]["start"]["line"], int)
              and d0["range"]["start"]["line"] >= 0, json.dumps(d0)[:160])
        check("diagnostic severity is error", d0.get("severity") == 1)

    # Clean document must publish an empty list (an all-clear is a real answer).
    clean_uri = "file:///gate/clean.fl"
    c.notify("textDocument/didOpen", {"textDocument": {"uri": clean_uri, "text": CLEAN, "version": 1}})
    clean_diags = c.wait_diagnostics(clean_uri)
    check("clean document publishes no diagnostics", clean_diags == [], repr(clean_diags))

    comp = c.request("textDocument/completion", {
        "textDocument": {"uri": clean_uri}, "position": {"line": 9, "character": 0}})
    items = (comp or {}).get("result", {}).get("items", [])
    labels = {i["label"] for i in items}
    check("completion includes keywords", {"fn", "let", "match", "continue", "and"} <= labels,
          str(sorted(labels))[:200])
    check("completion includes user declarations", {"add", "main"} <= labels)
    check("completion includes runtime builtins", any(l.startswith("flint_vec_") for l in labels))
    check("completion kinds are LSP constants",
          all(i.get("kind") in (3, 6, 13, 14, 22) for i in items),
          str(sorted({i.get("kind") for i in items})))
    kinds = {i["label"]: i.get("kind") for i in items}
    check("keyword kind is 14", kinds.get("fn") == 14, str(kinds.get("fn")))
    check("builtin kind is 3 (function)", kinds.get("flint_vec_new") == 3)

    # Hover on `add` at its use site: doc comment + signature.
    hover = c.request("textDocument/hover", {
        "textDocument": {"uri": clean_uri}, "position": {"line": 6, "character": 11}})
    value = ((hover or {}).get("result") or {}).get("contents", {}).get("value", "")
    check("hover includes doc comment", "Adds two numbers" in value, value[:120])
    check("hover includes signature", "fn add(a: i64, b: i64)" in value, value[:120])

    # `return` on line 2 is a real keyword, so hover must explain its role.
    keyword_hover = c.request("textDocument/hover", {
        "textDocument": {"uri": clean_uri}, "position": {"line": 2, "character": 5}})
    kv = ((keyword_hover or {}).get("result") or {}).get("contents", {}).get("value", "")
    check("hover on a keyword explains its role", "return" in kv.lower() and "keyword" not in kv.lower(),
          kv[:120])

    definition = c.request("textDocument/definition", {
        "textDocument": {"uri": clean_uri}, "position": {"line": 6, "character": 11}})
    locs = (definition or {}).get("result") or []
    check("definition jumps to the declaration",
          bool(locs) and locs[0]["range"]["start"]["line"] == 1,
          json.dumps(locs)[:160])

    refs = c.request("textDocument/references", {
        "textDocument": {"uri": clean_uri}, "position": {"line": 6, "character": 11},
        "context": {"includeDeclaration": True}})
    rlocs = (refs or {}).get("result") or []
    check("references finds declaration + both uses", len(rlocs) == 3, json.dumps(rlocs)[:200])

    syms = c.request("textDocument/documentSymbol", {"textDocument": {"uri": clean_uri}})
    sarr = (syms or {}).get("result") or []
    snames = [s["name"] for s in sarr]
    check("documentSymbol lists declarations", snames == ["add", "main"], str(snames))
    check("documentSymbol kinds are LSP constants",
          all(s.get("kind") in (10, 12, 13, 23) for s in sarr), str([s.get("kind") for s in sarr]))

    # didChange must refresh diagnostics (full sync).
    c.notify("textDocument/didChange", {
        "textDocument": {"uri": clean_uri, "version": 2},
        "contentChanges": [{"text": 'fn main() {\n    also_undefined()\n}\n'}]})
    changed = c.wait_diagnostics(clean_uri)
    check("didChange republishes diagnostics", bool(changed), repr(changed))
    if changed:
        check("didChange reflects the new text",
              "also_undefined" in " ".join(d["message"] for d in changed),
              json.dumps(changed)[:200])

    sd = c.request("shutdown", None, timeout=10)
    check("shutdown responds with null", sd is not None and sd.get("result", "missing") is None,
          json.dumps(sd)[:120] if sd else "no response")
    c.notify("exit", None)
    try:
        code = c.p.wait(timeout=10)
        check("exit terminates the process", code == 0, "exit code %s" % code)
    except Exception as e:
        check("exit terminates the process", False, str(e))

    failed = [r for r in results if not r[1]]
    print("lsp-protocol: %d passed, %d failed" % (len(results) - len(failed), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
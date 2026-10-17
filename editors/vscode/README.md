# Flint for VS Code

Syntax highlighting plus the Rust language server for Flint: diagnostics,
completion, hover, go-to-definition, find-references, and a document outline.

## What you get

| Feature | Provided by |
|---|---|
| Syntax highlighting (26 TextMate patterns, keywords generated from the compiler lexer) | `syntaxes/flint.tmLanguage.json` |
| Bracket matching, auto-closing pairs, `#`/`//` comment continuations, region folding | `language-configuration.json` |
| Red squiggles with real compiler messages | `flint-lsp` (runs `flintc`) |
| Keyword / type / declaration / 515-builtin completion | `flint-lsp` |
| Hover with the declaration signature and its doc comment | `flint-lsp` |
| Go to definition, find all references, document outline | `flint-lsp` |

The language server is Rust, not Python: one static binary, no interpreter on
the target device, and no `/tmp` assumption (the previous server wrote scratch
files to a path that does not exist on Android).

## Requirements

1. `flint-lsp` on your `PATH`, or set `flint.lsp.serverPath`.
2. `flintc` reachable from the server, or set the `--flintc` argument.
   Diagnostics come from the real compiler — the server never re-implements it.

## Configuration

| Setting | Default | Meaning |
|---|---|---|
| `flint.lsp.serverPath` | `flint-lsp` | Path to the server binary |
| `flint.lsp.arguments` | `["--flintc", "flintc"]` | Arguments passed to the server |
| `flint.lsp.trace` | `off` | JSON-RPC tracing (`messages`, `verbose`) |

## Build the server and bundle it

From the repository root:

```bash
cd lsp && cargo build --release      # -> lsp/target/release/flint-lsp
```

To ship the server inside the extension, drop the binary in
`editors/vscode/bin/flint-lsp` (or any path you set via
`flint.lsp.serverPath`). `cargo build --release` is already configured for a
phone-friendly profile: `opt-level = "z"`, LTO, stripped.

## Debug the extension host

1. Open `editors/vscode` in VS Code.
2. `npm install` (pulls `vscode-languageclient`).
3. Press `F5` to launch an Extension Development Host.
4. Open a `.fl` file; errors appear in the "Flint Language Server" output
   channel. Set `flint.lsp.trace` to `verbose` to see the JSON-RPC frames.

## What is intentionally absent

- Semantic tokens (highlighting is syntactic; the server reports no semantic
  scopes yet).
- Formatting and code actions: run `flint-fmt --check` in a task or a CI gate
  instead of pretending the editor owns formatting.
- Rename: refused by the server with JSON-RPC `-32601` rather than half-done.

## License

Apache-2.0, same as Flint itself.
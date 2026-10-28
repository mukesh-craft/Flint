# Credits and AI-assistance disclosure

## Maintainer

Flint is maintained by **Mukesh** as a solo project. The repository history
starts on 2026-07-02; the version in [`VERSION`](VERSION) is the released
compiler version.

## AI assistance

Flint is developed with AI coding assistants, including **OpenCode**. The
assistance is real and is not hidden:

- implementation drafts and refactors in `src/main.cpp`, `stage1/`–`stage3/`,
  and `runtime/`;
- test scaffolding and gate scripts;
- documentation drafts, link checks, and consistency edits.

What the maintainer owns:

- the architecture and the phase order — the decisions are recorded in
  [`STATE.md`](STATE.md), [`docs/LANGUAGE-ROADMAP.md`](docs/LANGUAGE-ROADMAP.md),
  and [`memory.md`](memory.md);
- release decisions and the final review of a change;
- the rule that a public claim needs a command in this tree behind it. AI
  output is a draft until a gate and a review accept it.

The session ledger in [`memory.md`](memory.md) includes failed attempts and
the lessons from them, not only the successes. If you find a claim in the
documentation that no command in this repository supports, that is a bug:
please open an issue with the file and the claim.

## Third-party components

Flint links or embeds LLVM/Clang, the Rust standard library and the crates
listed in `runtime/rust/`, OpenBLAS (optional), libcurl and sqlite3 (only
when a program reaches them), and Python (only for `python { }` / `py_eval`).
Their licenses remain their own; Flint itself is Apache-2.0 (see
[`LICENSE`](LICENSE)).

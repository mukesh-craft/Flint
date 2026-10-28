# Design philosophy and non-goals

**Status:** living document, last reviewed 2026-10-25. This file owns the
*why*: the constraints Flint is built around, the choices that follow from
them, and the things Flint deliberately does not do. It does not own status
counts or measurements — those live in [`STATE.md`](../STATE.md), and the
phase order lives in [`docs/LANGUAGE-ROADMAP.md`](LANGUAGE-ROADMAP.md).

Flint is a solo-maintained systems language. The design is intentionally
narrow: a small language that runs on the device in your hand, produces a
small native binary, and fails loudly instead of guessing.

## The constraint that shapes everything: the device is the target

Flint is developed and tested on a phone (Termux, AArch64), not on a
workstation with a cross-compilation SDK. That single constraint explains
most of the language:

- **One compiler binary does the work.** `flintc` compiles, JIT-runs, emits
  IR/object files, emits interfaces, runs tests, and reports artifact size.
  There is no separate driver, linker wrapper, or build server to install on
  the device.
- **The standard library is native and optional at link time.** A hello-world
  links only what it reaches: an 8 KB-class artifact, `libc` only
  ([`STATE.md`](../STATE.md) §6.1 has the current bytes, the date, and the
  attribution). The link fix that made this true is also recorded there; the
  regression it removed was 743,520 B.
- **No server-side artifact compiler.** The ML stack (tensors, tape autograd,
  NN layers, optimizers) is reachable from Flint on the device. The
  gradient checker and the XOR/two-layer models are the evidence, not a
  benchmark claim.
- **Cross-target output is emit-only.** `--target <triple>` writes `.ll` or
  `.o` for that triple (including `wasm32-unknown-wasi`), but linking and
  running stay on the host. There is no WASI sysroot on the device, so a
  WASM executable is not a supported artifact.

## Safety posture: loud, local, and predictable

Flint's safety story is deliberately unglamorous:

- **Ownership + borrow checking** catches use-after-move and conflicting
  borrows at compile time.
- **Generational Aegis leases** back the static rules at runtime and turn a
  stale handle into a panic, not a silent read of freed memory.
- **Bounds, overflow, and division-by-zero are checked by default.** `--unsafe`
  removes those checks; it is a measured trade-off, not the default.
- **No `null` keyword.** Absence is represented by `Option`, and `unwrap`
  panics with a message rather than dereferencing a null pointer.
- **One error per input.** A rejected program must produce one diagnostic,
  not a cascade of compiler-internal follow-ons. `tests/test_one_error.sh`
  is the machine-checkable form of that promise.

The cost is real: the checks are not free, and the current integer-loop
performance is 1.35–2.25× slower than C/C++/Rust on the recorded workloads
(the table and its conditions are in
[`docs/LANGUAGE-ROADMAP.md`](LANGUAGE-ROADMAP.md) §0). Floating-point loops
are at parity. Flint does not claim to be faster than C.

## Explicitness over convenience

Where a convenience would hide a decision, Flint makes the decision visible:

- Variables are immutable unless declared `mut`.
- String interpolation takes an identifier (`{name}`), not an arbitrary
  expression; `{{` and `}}` are the literal-brace escapes. The rule is
  narrow because the alternative (a full expression parser inside every
  string) is a second grammar with its own diagnostics.
- Closures capture by value. A closure that escapes its defining frame gets
  its environment box promoted to the heap at the escape site (Phase 2 / 2D);
  a closure that stays inside the frame keeps a stack box and allocates
  nothing. The compiler does not guess: the escape sites are return, global
  store, and call argument.
- Generics are monomorphized, so there is no runtime dictionary or boxing
  cost. The pipeline's explicit form is `f::<T>(...)`; the C++ compiler and
  the self-hosted pipeline must agree on the instance name.

## The self-hosted pipeline is a test, not a second product

The lexer, parser, and emitter are Flint programs (`stage1/`–`stage3/`). The
C++ compiler bootstraps them, and the bootstrap fixed point must be
byte-identical. This is deliberately uncomfortable: it forces the language to
be expressive enough to describe itself, and it exposes every place where the
C++ compiler and the pipeline disagree.

The known cost is **two frontends that can drift** (D5 in
[`COMPATIBILITY.md`](../COMPATIBILITY.md)). The mitigation is not a promise
that they will never drift; it is a gate that measures the drift
(`tests/test_differential.sh`) and a policy that the pipeline may never be
more permissive than the C++ compiler.

## What Flint deliberately does not do

These are non-goals for the current design, not bugs waiting to be fixed:

- **No tracing garbage collector.** Memory is owned, borrowed, or leased;
  the runtime backstop is for stale handles, not for collection.
- **No exceptions.** `Result`/`Option`, `try`, and `?` are the error path.
- **No async runtime.** Threads and channels are the concurrency primitives;
  `parallel for` was removed because it could not capture variables and had
  no result channel.
- **No implicit narrowing conversions.** Mixed `i64`/`f64` arithmetic
  promotes; a lossy conversion is written explicitly.
- **No macro system and no compile-time reflection.** The pipeline's
  S-expressions are an internal contract, not a user-facing macro language.
- **No C header emission.** `--emit-header` does not exist: a C compiler
  cannot consume Flint prototypes today.
- **No generic `[T]` containers yet.** Deferred by maintainer decision; the
  representation and ABI are not chosen (`STATE.md` §4.9).
- **No claim of Windows/macOS execution.** The code is portable in principle;
  this repository's evidence is Termux AArch64. The tier matrix in
  [`docs/tiers.md`](tiers.md) states what is actually exercised.

## Known trade-offs we would rather state than hide

- **An escaped closure's heap box is not freed.** There is no closure
  destructor yet. A program that creates escaping closures in a loop grows
  its heap. This is a real limitation, recorded in `STATE.md` §4.4.
- **The C++ bootstrap and the pipeline can diverge.** The differential gate
  covers the shapes it knows; it is not a proof of equivalence.
- **The LSP is useful but incomplete.** It has diagnostics, completion,
  hover, definition, references, and outline; it has no semantic tokens, no
  formatting request, and it refuses `rename` rather than half-doing it.
- **Python embedding is AOT-only.** JIT mode cannot resolve Python symbols;
  use `-o`.
- **Single-maintainer bus factor.** The repository is written to be
  inspectable: gates, decisions, and measurements are in-tree, and
  [`memory.md`](../memory.md) records what failed and why.

## How a claim gets into the documentation

A feature is documented only after a command in this tree produced the
result. The pattern is:

1. Write the smallest fixture that would fail without the feature.
2. Add it to a gate (`tests/`), not just to an example.
3. Record the measurement with its date, device, and control
   (`STATE.md` §8; benchmark rows carry their session date).
4. Put the claim in one document and link from the others
   (`docs/INDEX.md`).

If a statement here is wrong, it is a bug in the documentation and should be
reported like any other bug. The AI-assistance disclosure is in
[`CREDITS.md`](../CREDITS.md).

# STATE — Flint compiler: continuation document

**This file is the entry point for any future session.** It is written so that
a reader who has *no* prior context can (a) verify the current state, (b)
understand what is unfinished and why, and (c) continue the work without
re-deriving anything or repeating a failed experiment.

Companion documents, and what each is for:

| file | purpose | read it when |
|---|---|---|
| `STATE.md` (this file) | the continuation document: state, recipes, open items, decisions, traps | **always, first** |
| `memory.md` | the project ledger: the 2026-09/10 audit (bug table B1–B9), milestone specs, per-session addenda | looking up *why* a decision was made |
| `docs/LANGUAGE-ROADMAP.md` | the 30-section assessment and roadmap (claims tagged [M]/[P]/[D]/…) | planning work; do not edit the evidence in it |
| `COMPATIBILITY.md` | C++ vs self-host divergence log (D1–D5) | touching the self-host pipeline |
| `docs/errors-cpp.md` | the 135 catalogued C++ compiler diagnostics | touching diagnostics |

---

## 0. How to use this file

1. **Read §1 (state) and §4 (open items).** Everything else is reference.
2. **Before you touch code, run the §2 gates.** A stale `flintc` is the
   single most common source of wrong conclusions in this project (§8.1).
3. **Before you trust a claim in any ledger, apply the §3 verification
   protocol.** Three separate times, a confident written claim turned out to
   be false (§3.2). The ledgers are *hints*, not evidence.
4. **Update this file in place** after each meaningful step: the open-items
   table, the findings, the decision log. Do not append a "session log" —
   rewrite the affected section. Keep it under ~12K tokens; if it grows
   past that, compress §10 (history) and move detail into `memory.md`.
5. **One commit per coherent change**, with the measurement in the message
   (see §6.4 for the format).

---

## 1. Current position

**Flint is a 0.22.0 systems language with a safety model, a
batteries-included standard library, and a compiler that is also its
toolchain.** The C++ compiler (`src/main.cpp`, ~13K lines) is authoritative;
a self-hosted Flint pipeline (stage1/2/3) exists as the bootstrap ladder.

Measured on this device (Termux AArch64, 3.6 GB RAM, clang 21.1.8, rustc
1.99.1, Python 3.14.6) — reproduce with §2.3:

| workload | Flint | C | C++ | Rust | Python |
|---|---|---|---|---|---|
| sum_array 10M | 19.1 ms | 10.1 ms | 8.3 ms | 9.7 ms | 160.1 ms |
| primes 10M | 495 ms | 220 ms | 192 ms | 207 ms | 2051 ms |
| fib(45) | 12.9 s | 9.6 s | 9.3 s | 8.9 s | 579 s |
| pi 100M | 779 ms | 783 ms | 778 ms | 779 ms | 97.8 s |
| hello-world, stripped | 4,536 B | 4,368 B | 4,416 B | 339,656 B | ~276 MB stdlib |
| build time (hello, cold) | 144 ms | 439 ms | 404 ms | 674 ms | — |
| cold start (hello) | 37 ms | 33 ms | 39 ms | 42 ms | 150 ms |
| dynamic deps (hello) | libc only | libc+libm | libc+libm | libc | — |

All 21 test suites green at HEAD (`bc6b747`): smoke 48, lexdiff 131,
parse_gate 132, fixpoint 132, errors 128/128, errors-cpp 135/135,
differential 11/11, generics 7, no-hang 88, AI 26, rust-stdlib 20, rust-ffi
8, rust-oracle 2, https, python, lsp, fmt, opt-identity, cache,
fuzz-corpus, aegis, driver, tutorial, registry.

---

## 2. Environment, build, and verification recipes

### 2.1 Build (this device)

```bash
cd /data/data/com.termux/files/home/Flint

# fast syntax check (~3-8 min; the full compile is the slow part)
timeout 500 clang++ -std=c++17 -fsyntax-only -I$(llvm-config --includedir) src/main.cpp

# full build (12-15 min) — ALWAYS detached: a tool timeout kills the
# process group and orphans the compile (it has happened)
(setsid nohup bash build.sh > build.log 2>&1 < /dev/null &)
# poll:  grep -c "error:" build.log
#        grep -n "Build complete" build.log
#        ls -la flintc      # mtime MUST be newer than your last edit
```

Use `bash build.sh`, never `./build.sh` — the shebang is `#!/usr/bin/env bash`
and `/usr/bin/env` does not exist on Termux.

### 2.2 The gates

```bash
T=/data/data/com.termux/files/usr/tmp/opencode
bash tests/run.sh                  # smoke (48)
bash tests/test_lexdiff.sh         # 131  — token contract
bash tests/test_parse_gate.sh      # 132  — parser contract
bash tests/test_fixpoint.sh        # 132  — self-host byte-identical
bash tests/test_errors.sh          # 128/128 — stage3 emitter catalog
bash tests/test_errors_cpp.sh      # 135/135 — C++ compiler catalog
bash tests/test_differential.sh    # 11/11  — C++ vs self-host pipeline
bash tests/test_generics.sh        # 7
bash tests/test_no_hang.sh         # 88
bash tests/test_ai.sh tests/test_rust_stdlib.sh tests/test_rust_ffi.sh \
    tests/test_rust_oracle.sh tests/test_https.sh tests/test_python.sh \
    tests/test_lsp.sh tests/test_fmt.sh tests/test_opt_identity.sh \
    tests/test_cache.sh tests/test_fuzz_corpus.sh tests/test_aegis.sh
bash tools/docs_check.sh           # README counts vs tree
```

Run long gate sequences **detached** with a results file, because a tool
timeout kills the group mid-run and you lose the whole sequence:

```bash
rm -f $T/gates.txt
(setsid nohup bash -c 'for g in test_lexdiff test_fixpoint test_differential; do
    bash tests/$g.sh > '"$T"'/gate_$g.txt 2>&1; echo "$g=$?" >> '"$T"'/gates.txt
  done; echo ALLDONE >> '"$T"'/gates.txt' < /dev/null &)
```

### 2.3 The benchmark harnesses (new, 2026-10-20)

```bash
bash tools/bench_compare.sh        # Flint vs C/C++/Rust/Python, same workloads
bash tools/bench_tooling.sh        # size, build time, cold start (unique run id
                                   #   per run so a cached artifact is never
                                   #   measured as a build)
./flintc --check-bce <file>.fl -o /tmp/x   # checks emitted vs elided after O2
```

`tools/bench_compare.sh` compiles the mirrors in `benchmarks/` (C, C++, and
new Rust mirrors) and prints in-program times. `tools/bench_tooling.sh`
measures raw + stripped size, cold build time and cold start.

### 2.4 The IR comparison (the tool that settled the performance gap)

```bash
./flintc benchmarks/sum_array.fl --emit-llvm -o /tmp/f.ll
clang -O2 -S -emit-llvm benchmarks/sum_array.c -o /tmp/c.ll
llvm-size -A /tmp/f.ll ; llvm-nm --size-sort -S /tmp/f.ll | tail -20
```

This is the technique that found the real cause of the integer-loop gap
(§6.1). Use it before proposing any codegen change.

---

## 3. The verification protocol (read this before trusting any claim)

### 3.1 The rule

> A claim is **evidence** only if a command in this repository produced it.
> Everything else is a hypothesis.

Written claims in this project have been wrong **three times**, each in a way
that cost real time:

| # | the claim | reality | cost |
|---|---|---|---|
| 1 | "N1 (payload-enum `match` in a non-`main` function) hangs the compiler; not yet root-caused" (`memory.md:3793`) | **already fixed** — `tests/t_payload_match.fl` passes; the root cause was the single-binding arm parser plus `consume()` not advancing on error | a wrong "top open bug" ordering |
| 2 | "`flintc api` + agent-bench: AI-agent UX none of the five ship" | **no `flintc api` command exists** (`README.md:420` says so) | a false claim in a comparison table |
| 3 | "hello binary 4.3 KB (beats C!)" | was **743,520 B** — the link line lacked `-Wl,--gc-sections` | a 130× size regression shipped unnoticed |

### 3.2 The protocol (five minutes, always)

1. **Reproduce it**: write the smallest program that shows the behaviour and
   run it. Do not trust a description.
2. **Measure it**: with a number, a command, and the machine (this device,
   Termux AArch64).
3. **Map it to the source**: `grep`/`read` the actual implementation. The
   implementation frequently contradicts the comment next to it.
4. **Check the binary, not the source**, when the claim is about a *built
   artifact* (see §6.1: the source had `--gc-sections` in a comment, the
   binary did not have the flag at all).
5. **Re-verify every printed line against its assertion, one by one.** A
   fixture that "looks right" once shipped a real bug (the enum
   accumulator, §8.2).

### 3.3 What to do when a ledger contradicts the code

Fix the ledger in the same commit as the code change, and say which was
wrong. The ledgers are only useful while they are corrected in place.

---

## 4. Open items (the actual work queue)

Ordered by engineering value. "Verify" is the command that shows the item is
still open; "done" is the observable state that closes it.

### 4.1 P0 — Measurement infrastructure (roadmap Phase 0) — PARTLY DONE

**Done (2026-10-20)**:
- `flintc --print-size` — the compiler reports its own artifact's size
  attribution: file bytes, section bytes, header/metadata bytes, `.bss`
  reserved, per-class symbol attribution (Rust stdlib / C runtime /
  application), the section list, the dynamic dependency list, and the
  objects offered to the linker. `--print-size-json=PATH` writes the same
  data as JSON. Every number is read from the artifact, so it cannot drift
  from what was produced.
- `tests/test_print_size.sh` — a gate that checks the report is TRUE: the
  reported size must equal the file on disk, sections must fit inside the
  file, the dependency list must mention libc, and the JSON must parse and
  agree with the text form.
- `tools/bench_gate.sh` — the measurement gate: runs the workloads, writes
  a per-commit summary, and fails when runtime > +3 %, binary > +5 % or
  compile time > +10 % against a baseline. Baselines are **per machine**
  (absolute timings are machine-specific — a published suite measured a 41 %
  run-to-run spread on one laptop and 15 % on another while its *verdicts*
  reproduced), so a baseline from another machine is refused rather than
  silently compared.
- CI: the size gate runs as a required step; the measurement job is
  **advisory** until a per-runner baseline exists.

**Also landed in the same pass**:
- `tests/test_one_error.sh` — the machine-checkable form of "one error per
  program": every known-bad fixture must produce exactly ONE error line. The
  parse side had this contract since M6; this pass closed the codegen side
  (emitStmt/emitExpr now stop after the first error) and added the gate.
- Codegen diagnostics now carry a source location
  (`codegen: 9:12: cannot return a function value: ...`), matching the
  parse errors, which have always had one.

**Still open**:
- A *history* (the JSON per commit is produced, but nothing accumulates or
  charts it yet) and a dashboard page.
- The runtime dashboard should cover more than two workloads (the roadmap
  §21 list: allocation, strings, collections, concurrency, networking, and
  one realistic application).
- `--print-size` should also report the **monomorphization** share
  (twiggy's `monos` idea): how many bytes are duplicated generic
  instantiations. That is the one contributor in the size analysis that is
  currently unmeasurable.
- The gate thresholds are advisory on CI until a per-runner baseline is
  committed.

**Verify**: `bash tests/test_print_size.sh` and `bash tools/bench_gate.sh`.

### 4.2 P0 — Phase 2: a type checker and a typed HIR (REVISED 2026-10-20)

**Read this before starting Phase 2.** The first draft of this item said
"add a typed HIR/MIR". The research pass (below) and a red-team pass on
that draft found it was **vague in a way that would have wasted weeks**:

- Flint has **no type checker**. Types are guessed in ~6 places
  (`exprFlintType`, `emitVarDecl`'s ladder, `inferGenericArgType`,
  `typeMangleTag`, the annotation checks). A "typed HIR" that merely records
  those guesses would be a typed tree full of lies — and the M2c work
  (removing the silent i64 defaults) is evidence of how wrong the guesses
  were. **So the first deliverable is a real type checker, not a data
  structure.**
- The **self-host pipeline cannot consume such an HIR yet**: stage2/stage3
  have no generics at all (measured: 0 `typeParams`), so they cannot even
  *express* an HIR with generic parameters. The pipeline converges **after**
  the C++ side, if at all (§4.9).
- The **LSP is in Rust, the compiler in C++**. "Share the HIR" is not free
  and not automatic — it is a decision (see §4.7 and the decision list).
- **Checks are code today**, emitted inline. They must become *data*
  (IR-level operations) before a profile or an analysis can remove them.
- A full **SSA MIR is a backend rewrite** — the thing we must not do. A
  typed HIR plus a side CFG (for region inference only) is enough.

**The revised plan, in delivery order** (each step independently gated and
shippable; the full design is in `docs/LANGUAGE-ROADMAP.md` §13/§27, the
research behind it in §6.11):

| step | deliverable | gate |
|---|---|---|
| **2A.0** | a **type checker** for the subset that is decidable today: local variable declarations, function signatures, literal types | it must AGREE with the compiler's current behaviour on the whole corpus (a differential gate) — and the gate must also report where the current guesses are WRONG, which turns it into a bug-finder |
| **2A.1** | **types carry facts**: an array's *length* when it is known (literals), the resolved trait for a call, the instantiated type arguments | a fact is only used when it is exact; a wrong fact must be impossible by construction |
| **2B** | **checks become IR-level operations** (a bounds check and an overflow check as explicit operations, not inline compare+branch) | the profile decides insertion (safe/unsafe), and an analysis can drop an operation that is proven unnecessary |
| **2C** | **analyses on the HIR**: range/length analysis (static bounds proofs), then escape analysis (Swift's criteria: does not escape + uses post-dominate the allocation) | a check that cannot be proven is KEPT — the safe direction; a failed closure promotion produces a *diagnostic saying why* |
| **2D** | **regions** (NLL-style, from the CFG) → escaping closures | deferred until 2C's escape analysis exists; they are the same machinery |
| **2E** | **the pipeline consumes the same semantics** (D5) | only after the language features the HIR needs exist in the pipeline |

**First slice LANDED (2026-10-20, commit 42b3a04):** step 2A.1's length
fact. A type may carry FACTS, not just a kind (k2's idea). The fact is
the EXACT length of an array value, as an SSA value: a constant for a
literal initializer, and the argument of the `flint_array_alloc` call
otherwise (the runtime sets `len: n`).

- **Win, measured**: the bounds check used to compare the index against
  the length LOADED FROM THE SLICE while the loop bound was a different
  value, so LLVM could not relate them and the check survived in every
  loop (3 of 6 checks survive in `sum_array`). With the fact the check
  compares against the same value the loop uses, and LLVM folds it: a
  literal array goes from 4 surviving checks to 0, and a runtime array
  accessed with `arr[i]` produces **0 bounds-check calls** in the final
  IR.
- **Safety**: a fact is used only when its value DOMINATES the use (a
  DominatorTree check — a value from inside a loop is not available after
  the loop, and using it would be invalid IR); the R2 BCE-lite proof is
  implemented for real (a literal index into a literal-length array is
  provably in bounds); a fact is CLEARED on assignment; and an
  out-of-range literal index still TRAPS (gate
  `tests/test_bounds_safety.sh`).
- **What it does NOT do (measured, not guessed)**: the benchmark
  `sum_array` uses the BUILTIN `flint_array_get(arr, i)`, whose check is
  emitted at the call site against the loaded length, and the runtime's
  own check still applies. Removing THAT needs **range analysis**
  (proving `i < n` from the loop bound) — step 2C — plus an unchecked
  runtime variant. The 13 % therefore still needs 2C; this slice is its
  foundation, not the win.

**Dead ends found while doing it (do not retry):**
- The vestigial `IndexExprAST::unchecked` flag ("R2 BCE-lite: proven
  in-bounds") was never set anywhere — a dead field that claimed to
  implement bounds-check elimination. It is now actually used, but the
  flag alone was never the missing piece: what was missing was a PROOF.
- The `--check-bce` COUNT is confounded: LLVM clones loops, so the
  "surviving checks" number can exceed the "emitted" number (negative
  "elided"). Measure the real thing instead: the number of
  `call void @flint_bounds_check` in the final IR, and the runtime.
- A fact must not be used where its value does not dominate. Flint's
  block scoping hides the obvious case (a binding declared in a loop is
  not visible after it), but the general rule still needs the
  DominatorTree check.

### 4.3 P1 — Range analysis → check elimination (roadmap Phase 3/5)

**What**: the measured 13 % overhead comes from two checks per loop
iteration that LLVM cannot remove, and their presence also blocks
auto-vectorization (§6.1).

**Do**:
1. *Same-value bounds* (the "reslicing" technique): when a loop's bound and
   an access's length are the same SSA value, LLVM folds the check away.
   This is the cheapest fix and needs no new IR.
2. *Range analysis* on HIR to remove overflow checks where provable, and
   bounds checks where the length is known.
3. Re-measure: the target is "safe within 1.2× of `--unsafe`" and "integer
   loops within 1.3× of C".

**Verify**: `./flintc --check-bce benchmarks/sum_array.fl` prints "6 checks
emitted, 3 survive O2 (3 elided)" today; the target is fewer survivors.

**Do NOT** build a vectorizer: LLVM's own vectorizer will do the work once
the checks are gone (§6.1). If SIMD is wanted in the stdlib, follow Go
1.27's portable `simd` package (size-agnostic, emulated fallback) rather
than compiler intrinsics.

### 4.4 P1 — Regions, escape analysis, escaping closures (roadmap Phase 3)

**What**: a closure's value is an env box in the defining frame, so it
cannot escape (the compiler refuses to return or store it). This is a
consequence of the representation, not a missing feature.

**Do**: escape analysis → a closure whose captures do not outlive the
defining function can live in the per-thread arena (already in the Rust
runtime) instead of the frame, and the restriction lifts for it; a
capture that does outlive it is moved automatically, with the compiler
*reporting* the decision.

**Done when**: `let cb = |x| x + 1; store_it(cb)` compiles, and the
benchmark shows no heap allocation for the non-escaping case.

### 4.5 P1 — Capability types (Send/Sync-like) (roadmap Phase 3)

**What**: there is no static race freedom. `parallel for` was removed
(correctly — it captured by value across pthread workers).

**Do, in order**: (1) marker capabilities on leases + a check where a value
crosses a thread boundary; (2) typed channels as the safe way to share
mutable state; (3) safe parallel loops as *reductions* with an explicit
result channel and capability-checked closures (the shape `memory.md` B6
already specifies: "capture-free builtins, not syntax sugar"); (4) atomics;
(5) async/await **only after** (1)–(4), because async's safety depends on
capabilities.

**Reference**: Rust's Polonius Alpha (nightly since 2026-08) is the current
state of the art in flow-sensitive borrow analysis — the lesson for Flint
is that *flow-sensitivity* is what makes a lifetime system practical
(Rust's NLL still rejects "problem case #3", conditional borrows).

### 4.6 P1 — Package ecosystem (roadmap Phase 8)

**What**: zero third-party packages. The stdlib is strong (sqlite, http,
crypto, png, brotli, argon2, regex, csv, fts5, bigint, time, toml/yaml);
what is missing is a way to *distribute* code.

**Do**: `flint` subcommands (`add`, `build`, `publish`, `audit`, `search`),
a static registry (self-hostable), minimal-version selection with a
published deterministic algorithm, semver enforcement, `flint.lock` with
content hashes, content-addressed cache, signing, advisories, provenance
on publish.

**Scale context (2026 registries)**: npm ~3.75 M, PyPI ~0.71 M, Go modules
~2.16 M, Maven ~0.75 M, crates.io ~0.21 M [P: ODAI-LIB ecosystem dump,
2025-12/2026-01; crates.io also indexed at 129k–361k depending on
counting method]. A new language does not need to match those; it needs
*ten* good packages and a resolver.

**Done when**: a package can be published by one user and installed by
another from a lockfile, reproducibly.

**Independent of the compiler work** — it can proceed in parallel.

### 4.7 P2 — Compiler-backed LSP (roadmap Phase 7)

**What**: `lsp/` is a separate Rust implementation; it cannot ask the
compiler what a type is, so editor diagnostics are a reimplementation and
will drift exactly as the self-host pipeline did (D5).

**Do**: serve the LSP from the compiler's own type checker (needs 4.2);
add rename/refactor (needs the symbol table); add per-file incremental
analysis (the compiler caches whole modules by source hash today).

**Speed matters more than features**: TypeScript's native port (TS7) is
8–10× faster than the JS compiler on real projects [P: TypeScript dev
blog, 2025-12] — the precedent is that a language server backed by a slow
type checker is unusable, and the fix was to make the *compiler* fast, not
the server.

**Done when**: the LSP's diagnostics are byte-identical to the CLI's over
the corpus (a test compares them).

### 4.8 P2 — Type-system features (roadmap Phase 4)

Ranked in `docs/LANGUAGE-ROADMAP.md` §11. Short version: associated types
and `dyn` trait objects first (they decide whether third-party packages
appear), then operator overloading, then nullable types (`T?` — cheap and
high value), then const generics, then async/await (needs 4.5), and
type-level programming last (highest complexity, lowest immediate value).

**Do NOT** add a second ownership system: Flint's leases are the
user-visible model, and adding Rust's borrow checker on top would create
two competing systems (§8.3).

### 4.9 P2 — Close D5: the self-host feature gap

**What**: the self-host pipeline (stage2/stage3) has no traits, no
generics, no `::<`, no enum equality, while the C++ compiler has all four.
The differential gate is green (11/11) *because* it does not cover them.

**Do**: after 4.2, make the pipeline emit the shared HIR rather than
re-implementing the semantics. Until then, D5 is the honest record.

**Verify**: `grep -c typeParams stage2/flint_parse.fl` → 0 today.

### 4.10 P3 — Items that need a decision or a different machine

| item | why it is blocked |
|---|---|
| **M7 `[T]` containers** | deferred by maintainer decision (2026-10-19); needs the Rust `{length, elem_size, ptr}` representation and an explicit ABI decision |
| **B8 type-level generic enums** (`Option<T>`) | changes the type system and the ABI; ROADMAP.md:466 overclaims — needs a decision |
| **Signed release tags** | the repo has 0 tags; the procedure is documented in `docs/RELEASE.md`; this device has no GPG key |
| **Windows / macOS real runs, CI** | never executed from here; the workflows exist (`.github/workflows/flint.yml`) but no runner here |
| **QBE backend** | needs an external `qbe` binary (not installed); LLVM is the default and tested |
| **WASM** | objects are valid; there is no WASI runtime here to execute them |
| **Nightly fuzzing** | the corpus and generator exist and replay in CI; there is no always-on runner here |

---

## 5. The plan (one page)

```
Phase 0  MEASUREMENT   bench + size + safety gates in CI, --print-size      ← 4.1
Phase 1  RECOVER       regressions (link fix DONE; IR comparison next)        ← 6.1
Phase 2  UNIFY         type checker → typed HIR (2A) → facts (2A.1) →          ← 4.2
                        check ops as data (2B) → analyses (2C) → regions (2D)
Phase 3  SAFETY        range analysis, BCE, regions, capabilities, T?        ← 4.3-4.5
Phase 4  TYPE SYSTEM   assoc types, dyn, operator overloading, const generics ← 4.8
Phase 5  OPTIMIZATION  BCE/overflow elimination, devirt, specialization, PGO  ← 4.3
Phase 6  SIZE          per-subsystem stdlib objects, profile-driven arenas   ← 6.4
Phase 7  TOOLING       compiler-backed LSP, rename/refactor, incremental     ← 4.7
Phase 8  ECOSYSTEM     package manager, registry, lockfile, signing         ← 4.6
Phase 9  DOMAINS       @domain(...) source declarations, policy table         ← 6.5
Phase 10 DIFFERENTIATE deterministic+safe+agent-native niche                 ← 6.6
```

Phase 0 and Phase 8 are independent of everything else and can run in
parallel with the compiler work. Phase 2 is the long pole.

**Domain system — the sharpened design** (evidence from the 2026-10-20
research pass): a domain is a **policy**, not a compiler variant. The
precedents are explicit: Reactant.jl lowers Julia to MLIR/XLA and reaches
cuBLAS-level GEMM but takes *seconds* to compile [P: JuliaCon 2026 / FOSDEM
2026]; JLIR, a Julia-native dialect IR, does the same class of work in
<1.5 ms but reaches 1.2 TF/s vs Reactant's 14.8 TF/s because it does not
generate shared-memory tiling [P: arXiv 2609.04585]. The lesson for Flint:
*lightweight, data-driven pass selection* (a table, not code paths) with
*optional* deep integration (XLA-class) for one domain only. MLIR's own
drawback — "strong compile-time type requirements and a low-level C++
extension model can be a poor match" [P: arXiv 2609.04585] — is exactly the
argument for a table-driven Flint policy rather than a dialect framework.

---

## 6. Findings and decisions (the "why" behind the plan)

### 6.1 The binary-size regression: found, root-caused, fixed (commit `46a007e`)

**Symptom**: a hello-world linked to 743,520 B (565,600 B stripped) — about
130× C — and reserved 72 MB of `.bss` in every process, with six dynamic
dependencies.

**Attribution method** (reusable — this is the §4.1 tool spec):
1. `llvm-size -A` for sections; `llvm-nm --size-sort -S` for symbols.
2. **Intercept the compiler's own link command** — a shim `clang` on `PATH`
   that logs `argv` and execs the real one. (The shebang must be
   `/data/data/com.termux/files/usr/bin/bash`; `/bin/bash` does not exist.)
3. Relink the same object with subsets of the object set.

**Attribution**: `.text` 313,196 B, `.rodata` 153,088 B, `.eh_frame(+hdr)`
68,940 B, `.bss` 74,239,728 B reserved; the application's own object is
1,352 B. The bulk was the *whole* standard library linked into every
program.

**Root cause**: the link line had no `-Wl,--gc-sections` — even though
`build.sh` compiles the runtime with `-ffunction-sections -fdata-sections`,
so the capability existed. The source even carried a comment claiming
"743 KB instead of 8 KB": the number the compiler actually produced.

**Fix**: `--gc-sections`; `--as-needed`; sqlite3/curl added only when a
linked stdlib object references them; OpenBLAS only when the program
reaches a tensor/NN/AI entry point (`flt_*`, not `flint_matmul` — the
tensor API's prefix is `flt_`); OpenBLAS wrapped in `--no-as-needed`
because `flint_blas.o` references `cblas_dgemm` **weakly** and
`--as-needed` ignores weak undefined symbols.

**Result**: 8,048 B raw / 4,536 B stripped; `.bss` 24 B; `libc` only.

**Two traps that make this a class, not an incident**:
- The compiler **caches the linked binary** by source hash, so a stale
  artifact survives a source fix and a size measurement can be wrong even
  when the source is right.
- The weak-symbol/`--as-needed` interaction silently demoted the BLAS
  matmul to the portable loop (n=200: 14.4 s vs 2.9 s) — a 5× regression
  that **no test caught**, because the results were still correct.

### 6.2 The performance gap is measured, not guessed

Optimized-IR comparison (§2.4) of `sum_array`:

| | C | Flint |
|---|---|---|
| instructions in `main` | 52 | **49** (Flint is smaller) |
| vector operations | **16** (`<2 x i64>`) | **0** |
| `icmp` in `main` | 2 | 4 |

So the gap is **not** code bloat. C auto-vectorizes the integer reduction;
Flint cannot, because C's out-of-bounds and signed overflow are UB (assumed
away) while Flint's are defined traps, and because the array length is a
runtime value unrelated to the loop bound. The two checks are what block
the reassociation a vectorized reduction needs. The f64 benchmark is at
parity precisely because clang will not reassociate an FP reduction either —
**the gap appears exactly where C's UB buys it more**.

Consequence: the fix is **analysis** (range analysis + same-value bounds),
after which LLVM's own vectorizer can vectorize. Flint does not need a
vectorizer of its own. A C-like "wrapping arithmetic" type is the
alternative and deserves consideration.

**This measurement also disproved one of the roadmap's own proposals**
("inline the array accessors" — the IR shows the fast path is already
inlined). It is struck through in the roadmap with the measurement that
caught it.

### 6.3 Where the remaining size lives (for §4.1's `--print-size`)

`flint_std.o` is 825 KB of object code holding sqlite, brotli, png, crypto,
encoding, compression, TOML/YAML and the arena, in **one** object — so
`--gc-sections` can drop *sections* but the object is pulled in wholesale
when any symbol is referenced. Splitting it per subsystem is the Phase 6
work. The Rust stdlib also reserves 72 MB of fixed arenas (arena 48 MB,
compress 16 MB, sqlite 4 MB, crypto 1 MB) — a *memory* profile decision,
visible in the dashboard rather than accidental.

Precedent for the attribution tool: Bloaty (per compile-unit / symbol /
section / segment, with size diffs for CI), cargo-bloat (per crate),
twiggy (monomorphization), and a pure-Rust equivalent that flags
monomorphized instantiations [P: google/bloaty docs; nornir_introspect].

### 6.4 The commit-message format (every change)

```
runtime:       -3.2 %
binary:        -41 KB (stripped, hello-world)
compile time:  +1.8 % (cold)
memory:        -5 % (compiler peak RSS)
safety:        unchanged (6 checks emitted, 3 elided)
```
plus the gate result. A commit without numbers is a commit without evidence.

### 6.5 Domain declarations — the sharpened design

`@domain(ai)` as a **file-level declaration** (before any item), parsed as a
distinct token. It is *compiler policy*, never semantics, and never
`--unsafe`. The policy is a **table** (`tools/domains.toml` style), so adding
a domain is a data change, not a compiler change — the guard against
"dozens of independent compilers". The layer order is
source > project file > target (may veto) > compiler flags. The LSP reads
the domain for completion and hints; **diagnostics never change**.

### 6.6 Where Flint could beat C/C++/Rust [I]

- **Deterministic memory management with a runtime backstop**: leases +
  generational collector + deterministic free. Go and Python have a GC;
  Rust has neither GC nor a backstop; Zig has safe modes but no ownership
  model.
- **Safety without the borrow-checker tax**: Rust's own community still
  argues about the ergonomics of NLL/Polonius [P: Rust internals, 2026];
  Flint's lease model can stay out of the type system.
- **An agent-native toolchain**: a stable token dump, a catalogued
  diagnostic contract, one binary, an agent benchmark. Nothing else in the
  comparison ships an agent-facing contract. (LSP-based navigation is worth
  31–34 % of tool-output tokens to an agent [P: CircleCI, 2026-06].)
- **Domain-aware specialization without weakening guarantees** — only
  credible if §6.5's separation holds.

### 6.7 A defect the code-quality pass found (and the rule it revealed)

`g = h; g(5)` — copying a *named function's* value into another binding —
failed with **"undefined function 'g'"**. The value was bound with a pointer
type instead of a fn type, so the call site could not recognise it.

Root cause: **three sites answered "is this binding a function value?" with
three different rules** — one required `isLambda && fnType`, another required
`!isLambda && fnType`, a third accepted either. The binding site used the
first, so named-function values (added after M4) were never recognised.

Fix: one predicate, `Symbol::isFunctionValue(sym)`, used by every site that
asks the question (binding, storing in a global, returning, calling). The
regression fixture is `tests_manual/t_fn_value_copy.fl`.

This is the *same* failure shape as the P0-NEW-1 drift (two copies of one
rule) and as the three wrong ledger claims (§3.1): **a rule that is written
down more than once is a rule that will disagree with itself.** The fix is
not to write the rule correctly three times; it is to write it once.

### 6.8 Two more defects the code-quality pass found

**Invalid IR for `fn`-typed returns.** A function whose return type is a
FUNCTION TYPE and which falls off the end built its default return value
with `ConstantInt::get(retType, 0)` — an integer constant with a POINTER
type, because an `fn(...)` return type is a pointer (M4). The result was an
internal LLVM error instead of a working program:

```
Function return type does not match operand type of return inst!
  ret i0 0
```

The terminator now uses the null value **of the return type**. Fixture:
`tests_manual/t_fn_return_type.fl`. This one only appeared when the M4
feature (fn-typed returns) met an older code path — a reminder that a new
type can invalidate code that predates it.

**Codegen kept going after an error.** After `codegenError`, the emitter
continued, so the IR no longer matched the source and LLVM's verifier
printed a second message about the compiler's internals on top of the
user's real error. `emitStmt`/`emitExpr` now stop once an error has been
reported.

### 6.9 `--unsafe` is implemented (roadmap §10)

The roadmap specified an escape hatch; the compiler had half of it (overflow
checks only) and none of the guarantees. It is now:

| requirement | implementation |
|---|---|
| manual, terminal-only | only the `--unsafe` command-line flag sets it; no domain, profile, package or tool can |
| never automatic | asserted by `tests/test_unsafe_flag.sh` |
| visible | every unsafe build prints what is disabled and what still applies |
| auditable | the artifact carries `flint:safety=unsafe` (or `...=safe`), kept alive through `--gc-sections` via `llvm.used` |
| deliberate and isolated | the two PERFORMANCE checks (overflow, bounds) are removed; ownership, borrow/lease, escape, null and divide-by-zero rules still apply |

`--safe` and `--release` remain no-ops for compatibility and restore the
default (safe) posture.

**Also fixed here:** `--check-bce` was silent on a cache hit — the module
cache stores the already-optimized module, so the "checks emitted" count was
gone. A diagnostic that depends on cache state is not a diagnostic; the flag
now bypasses the cache.

### 6.10 Phase 2 research and red team (2026-10-20)

**Sources consulted** (all current, 2026 unless noted):

- **k2's compiler architecture** (k2-lang.org/docs/compiler-architecture) —
  the closest working template to what Phase 2 needs: AST → HIR (names
  resolved) → HIR (fully typed) → HIR (comptime-folded) → MIR
  (monomorphized) → MIR (**safety checks inserted as a pass**) → backend.
  Three ideas taken: (a) *bidirectional* type inference — "check mode pushes
  an expected type down, synthesize mode pulls a type up"; (b) *one IR, two
  backends*, so both share 100 % of the semantics; (c) *check insertion is a
  pass*, so "ReleaseFast" simply has no checks in the IR — "there is nothing
  for the optimizer to remove".
- **rustc's borrow-check guide** (rustc-dev-guide) — the MIR is "far less
  complex than the HIR", the radical desugaring prevents borrow-checker
  bugs, and MIR is what makes *non-lexical lifetimes* possible (regions from
  the CFG). Also: MIR type checking is a second pass whose purpose is to
  collect region constraints — and it doubles as a self-check ("if something
  fails in MIR type checking that passed in HIR type checking, something has
  gone wrong").
- **rust-analyzer and rustc** (RFC 2912, the shared-parser issue #10765) —
  the LSP is a *third frontend*; sharing the parser with rustc took four
  years and is still open ("parser/AST has wide APIs, so extracting that is
  a whole lot of poorly factorable work"). The solution Rust reached:
  extract shared *libraries* (`rustc_type_ir`, `rustc_next_trait_solver`,
  `rustc_lexer`, `rustc_ast_ir`) and let each frontend implement an
  `Interner` trait.
- **Swift's StackPromotion / AllocBoxToStack** (swiftlang/swift) — stack
  promotion needs two conditions (the allocation does not escape; its uses
  post-dominate the allocation), and when promotion fails Swift *diagnoses
  why* ("mustbeonstack": "Due to user: ...").
- **BCE realism**: Rust's own numbers — "bounds check elimination: usually
  works, but not reliable; good: constant bounds, straight-line code; bad:
  checks in loops" (Rust Heart LLVM, 2024-10). SAFECode eliminated "many"
  runtime bounds checks with *interprocedural* static analysis.

**Red team on the first draft of the Phase 2 plan** (the plan said "add a
typed HIR/MIR"):

| # | objection | severity | what it changes |
|---|---|---|---|
| A | Flint has **no type checker**; a HIR recording the current guesses would be a typed tree full of lies | **blocker** | the first deliverable is a real (bidirectional) type checker, with a differential gate against today's behaviour |
| B | the self-host pipeline cannot express generics/traits (0 `typeParams` in stage2) so it cannot consume such an HIR | **blocker** | the pipeline converges *after* the C++ side; D5 is not a Phase 2 deliverable |
| C | the LSP is in Rust, the compiler in C++: sharing the HIR is a *decision* (shared library + query API, a Flint rewrite, or accept drift) | serious | the LSP item moves to the decision list; it is not assumed |
| D | checks are code, not data, so no profile or analysis can remove one | serious | "checks become IR-level operations" is its own step (2B) |
| E | a full SSA MIR is a backend rewrite | serious | MIR is deferred; a side CFG is enough for region inference |

**Dead ends and non-findings (do not retry):**
- "Just build an SSA MIR like Rust" — rejected (objection E): it is a
  rewrite of the backend.
- "Let the HIR record what the current inference guesses" — rejected
  (objection A): it would encode the wrong types; the M2c work is the
  evidence of how wrong they were.
- "Make the Rust LSP consume the C++ HIR directly" — rejected for now
  (objection C): cross-language sharing is the 4-year problem Rust still
  has open; the cheap alternative is a query API on the compiler (the
  "flintc api" idea) and it is a maintainer decision.
- "Write the LSP in Flint" — blocked by D5: the pipeline has no traits or
  generics yet.

### 6.11 Decision log

| decision | rationale | rejected alternative |
|---|---|---|
| C++ compiler stays authoritative; pipeline converges on shared HIR | it has the full feature set and the whole backend; a rewrite would violate the incremental-migration rule | rewrite the compiler in Flint |
| Leases + regions, not Rust's borrow checker | one ownership model, not two | add NLL/Polonius-style checking on top of leases |
| `--unsafe` (build flag) **and** `unsafe {}` (source blocks) are different things | a global flag is too blunt for libraries; blocks are too verbose for a whole program; neither may be implied by a profile/domain/tool | one mechanism only |
| Vectorization comes from LLVM, not from Flint | measured: the checks are what block it (§6.2) | write a vectorizer |
| Domains are a data table, not code paths | keeps "adding a domain" from forking the compiler | a dialect framework (MLIR-style) |
| No second ownership system | leases are the user-facing model | borrow checker + leases |
| Measure before optimizing; the 743 KB binary proved it | two regressions passed a green suite | optimize the latest compiler blindly |
| One predicate for "is this a function value?" (`Symbol::isFunctionValue`) | three sites spelled it differently and a copy of a function value lost its type | keep the three ad-hoc rules |

### 6.8 Known dead ends (do not retry)

- **Byte-wise / element-wise enum payload comparison** — garbage bytes past
  a variant's payload make identical values compare unequal. Solved instead
  by per-variant *field* comparison (`emitEnumEqual`).
- **`hasStructName()`** does not exist on `llvm::StructType` in this LLVM
  version — use `isLiteral()` to tell identified structs from literals.
- **The C++ "single-pass emit path" (`Parser::emitXxx`) is unreachable** in
  a normal compile: it is entered only from `Codegen::emitFunction` for a
  function with no AST body. Measured: 0 hits over 137 corpus files with
  the `FLINT_TRACE_EMITPATH=1` probe. Fixes applied to that path are
  harmless but not load-bearing; the real second implementation is the
  self-host Flint pipeline (D5).
- **Whole-function text surgery by brace matching** — it landed wrong
  during M5. Read exact function bounds; replace atomically; syntax-check.
- **`open('w')` without writing** — it once zeroed `src/main.cpp`. Use the
  `write`/`edit` tools.
- **Believing a ledger claim** — three times (§3.1).

---

## 7. Files and anchors

| path | what lives there |
|---|---|
| `src/main.cpp` | the whole compiler (~13K lines). Key sites: `spawnLinker` (~293, the link line — §6.1), `emitCompareValues` / `createCallChecked` / `emitPrintCall` / `emitArray*Core` (the shared cores, P0-NEW-1), `emitEnumEqual` (P0-NEW-2), `inferGenericArgType` (M2c, strict), `namedFnThunk` / `namedFnValue` (named-fn-as-value), `llvmType` (array case = `{ptr,i64}`), `Type::fn_` |
| `tests/run.sh` | the smoke suite + `check_fail` cases (48 checks) |
| `tests/test_*.sh` | the 21 gates (§2.2) |
| `tests_manual/` | fixtures for **new syntax** (not globbed by the self-host parse gates): `t_explicit_targs*.fl`, `t_named_fn_*.fl`, `t_generic_no_infer.fl`, and the M4 lambda fixtures |
| `benchmarks/` | the workloads and their C/C++/Rust/Python mirrors |
| `tools/bench_compare.sh`, `tools/bench_tooling.sh` | the harnesses (§2.3) |
| `tools/errors_cpp.py`, `docs/errors-cpp.md`, `tests/test_errors_cpp.sh` | the C++ diagnostic catalog and its two-way gate |
| `stage1/`, `stage2/`, `stage3/` | the self-host pipeline (Flint). `stage2` has no `typeParams` today (D5) |
| `runtime/`, `runtime/rust/src/` | the C and Rust standard library |
| `lsp/` | the Rust language server (separate implementation — §4.7) |
| `agent-bench/` | the agent-facing task fixtures |
| `docs/LANGUAGE-ROADMAP.md` | the assessment and roadmap (do not edit its evidence) |

---

## 8. Traps (device and process)

1. **Stale `flintc`** — editing the source does not change the binary. A
   full build takes 12–15 min; a "failing" test may be testing yesterday's
   compiler. Always check `ls -la flintc` against your edit time.
2. **A tool timeout kills the process group** — never run a >8 min command
   in the foreground. Detach and poll.
3. **The build can be OOM-killed silently** (3.6 GB RAM, a 13K-line
   LLVM-heavy C++ file is hungry). If the log stops at
   "Building Flint compiler" with no error, re-run it.
4. **`/tmp` is not writable** — use
   `/data/data/com.termux/files/usr/tmp/opencode`.
5. **Pipes eat the exit code** — `./flintc f.fl | head; echo $?` reports
   `head`'s code. Verify with `>/dev/null 2>&1` and no pipe.
6. **`pkill -f` can match your own shell** — use the bracket trick
   (`pkill -f "test_selfhos[t]"`).
7. **Shebangs**: `#!/usr/bin/env bash` does not work on Termux; use
   `#!/data/data/com.termux/files/usr/bin/bash` in helper scripts.
8. **`~/.cache/flintc` holds compiled modules AND linked binaries** — a
   stale hit can make a measurement wrong (§6.1). `rm -rf ~/.cache/flintc`
   before measuring.
9. **The differential gate needs the self-host binaries**
   (`$TMPDIR/slipstream/self/{lex,parse,emit}.0`). If tmp was cleaned it
   reports "1 passed, 10 failed" with `self binaries` — that is a missing
   artifact, not a divergence. Rebuild: `bash tests/test_selfhost.sh A`.
10. **Verify every printed line against its assertion.** A fixture that
    "looks right" shipped a real bug once (the enum accumulator ANDed
    instead of ORing variant bodies).

---

## 9. Glossary

| term | meaning |
|---|---|
| **AEGIS** | Flint's memory model: static leases (ownership) + a generational runtime backstop + deterministic free; no GC pauses |
| **lease** | the user-visible ownership concept in Flint (not a Rust-style lifetime) |
| **D5** | the recorded gap: the self-host pipeline lacks traits, generics, `::<`, enum equality (`COMPATIBILITY.md`) |
| **emit path** | the single-pass Parser code path in the C++ compiler (currently **unreachable**, §6.8) |
| **env box** | the M4 closure representation: `{fnptr, caps...}`; the value of a closure or named function |
| **HIR/MIR** | the typed intermediate forms Flint does **not** have yet (§4.2) |
| **ladder** | the self-host bootstrap stages (stage1 lexer, stage2 parser, stage3 emitter) |
| **the two frontend paths** | the C++ Codegen (AST) and the single-pass Parser (emit) — the second is unreachable now |
| **domain** | a source-declared compilation *policy* (`@domain(ai)`), never a semantic change and never `--unsafe` |
| **safe vs unsafe** | "safe" = the compiler proved a check unnecessary; "unsafe" = the programmer asked for it (`--unsafe`, `unsafe {}`) — architecturally separate |
| **A1/A2 markers** | legacy milestone markers in the older ledger; not current terminology |

---

## 10. History (condensed)

- **2026-09-05 → 2026-09-26**: the original language bring-up (lexer,
  parser, codegen, stdlib, TLS, AEGIS, the self-host ladder, the LSP).
- **2026-10-15 → 2026-10-19**: the M0–M6 milestone wave — traits (M3),
  lambda-as-a-value (M4), `parallel for` removal (M5), diagnostics (M6);
  M7 (`[T]`) deferred by maintainer decision.
- **2026-10-19**: the B-wave — P0-NEW-3 (tensor index refusal), P0-NEW-1
  (path collapse), P0-NEW-2 (enum equality by per-variant field compare);
  then M2 residue part 1 (cast + nested-call inference), M2b (explicit type
  arguments `f::<T>(...)`), the named-function-as-a-value feature, M2c (the
  strict generic resolver), and the P4 error-code catalog.
- **2026-10-20**: the assessment pass — the 30-section roadmap, the
  benchmark harnesses, the binary-size regression found and fixed
  (`46a007e`), the performance gap measured by IR comparison, and this
  rewrite of `STATE.md`.

Commits of record (see `git log`): `1219665` M3 · `a6b89e6` M4 · `9553f8c`
M5 · `d65ed3f` M6 · `67edab9` P0-NEW-3 · `d1c58cf` P0-NEW-2 ·
`3d3d914` M2b · `fd8cf09` named-fn value · `922d03a` M2c · `4214082` P4
catalog · `46a007e` binary-size fix · `c69d421` assessment ·
`46b7c5c` the performance root cause.

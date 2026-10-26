# STATE — Flint compiler: continuation document

**This file is the entry point for any future session.** It is written so that
a reader who has *no* prior context can (a) verify the current state, (b)
understand what is unfinished and why, and (c) continue the work without
re-deriving anything or repeating a failed experiment.

Companion documents, and what each is for:

| file | purpose | read it when |
|---|---|---|
| `STATE.md` (this file) | the continuation document: state, recipes, open items, decisions, traps | **always, first** |
| `memory.md` | the project ledger: the 2026-09/10 audit (bug table B1–B9), milestone specs, per-session addenda, the full text of the long plans | looking up *why* a decision was made |
| `docs/INDEX.md` | the map: which document owns which fact (one fact, one home) | before adding or moving documentation |
| `docs/LANGUAGE-ROADMAP.md` | the 30-section assessment and roadmap (claims tagged [M]/[P]/[D]/…), the score table (§28) | planning work; do not edit the evidence in it |
| `COMPATIBILITY.md` | C++ vs self-host divergence log (D1–D5) | touching the self-host pipeline |
| `docs/PIPELINE-GENERICS.md` | the pipeline's generics/trait contract and known deviations | touching stage2/stage3 |
| `docs/errors-cpp.md`, `docs/errors.md` | the two diagnostic catalogues (C++ and self-hosted) | touching diagnostics |

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

## 0.1 CONTEXT WATCH (living block — update at every step)

**Purpose:** if a future session (or a compacted one) has no memory of this
work, this block says where the work stands, what is in flight, and what the
next action is. It is the first thing to read and the first thing to update.

| field | value |
|---|---|
| current phase | **Phase 2, steps 2A -> 2E** (the compiler-unification work) — 2A, 2B, 2C, 2D, and 2E are landed; **Phase 3 / P1 performance is the active work** (§4.3) |
| last completed | 2A.0 the type audit (`b75e4c7`); 2A.1 the length fact (`42b3a04`, `2b45516`); 2A one inference engine + three bug fixes (`f0441df`); 2C the loop-range analysis (`a7c6e2b`); 2D capture-less escaping closures (`57db1a4`); 2B the safety policy (`37d9b8b`); 2E measured + specified (`66f85ae`); **2E.1 the pipeline rename** (`16adde0`); **2E.2 the monomorphizer** (`48c12d5`); **the optimizer / LSP surface review** (§4.7.1); **2E.3/2E.4 the trait form and its resolution**; **2A the type-checking phase + call facts**; **2E.5 the C++ query mode + the pipeline overlay** (`--emit-signatures`, `E0284`–`E0289`); **2D.2 escaping capturing closures** (heap promotion at return / global store / call argument, `tests/test_closure_escape.sh`); **2E.6 explicit `::<T>` call syntax** (`E0295`–`E0298`, `tests/test_explicit_targs.sh`, differential 14/14); **the secure line-input API** (`flint_input*`, 1 MiB cap + status codes, `tests/test_input.sh` 8/8); **the closed-world bounded-range analysis** (`BoundedRangeAnalysis`, `tests/test_range_facts.sh` 19/19, `fib(45)` at C speed); **the middle-end TargetMachine** (TTI, `tests/test_vectorize.sh` 3/3, `--unsafe` at C parity); **the pipeline `-O2` consumers** (driver + gates, self-hosted emitter 2.37× faster, fixed point byte-identical) |
| in flight | **P1 performance**: the safe `sum_array` is 1.42× C (its accumulator check still blocks vectorization); `--unsafe` is at C parity. The pipeline's length fact/literal BCE are open (§4.3) |
| next action | **Implement the pipeline's length fact + literal-index BCE** (the remaining IR-quality gap). The safe-reduction path is settled against the max-abs guard (10.7 ms vs 11.0 ms safe: not worth its complexity); a wrapping operator remains the explicit future option. Phase 2's remaining gaps (typed HIR §4.2, NLL §4.4, capability types §4.5) come after |
| build | `bash build.sh` — ~3 min warm on this device (measured 2026-10-25); 12–15 min is the worst case under load. Poll at 60–120 s, never with a long sleep |
| gates | `bash tests/v1_gate_check.sh ./flintc --full` — 32 gates (23 fast + 9 slow), ~30–35 min; run it detached and poll |
| invariant | the tree must be GREEN and COMMITTED at every step; if a step cannot be finished safely, leave it documented rather than half-done |
| do not | hand-write a vectorizer (the roadmap rejects it); use `llvm.assume` for range facts instead |

**If you are reading this with no memory at all:** §2 has the build and gate
recipes, §4 has the open items with a "verify it is still open" command for
each, §6 has the findings that justify the plan, §8 has the traps (read §8
before running anything), and the git log has one commit per completed step.

## 1. Current position

**Flint is a 0.22.0 systems language with a safety model, a
batteries-included standard library, and a compiler that is also its
toolchain.** The C++ compiler (`src/main.cpp`, ~16K lines) is authoritative;
a self-hosted Flint pipeline (stage1/2/3) exists as the bootstrap ladder.

Measured on this device (Termux AArch64, 3.6 GB RAM, clang 21.1.8, rustc
1.99.1, Python 3.14.6). **The five-language comparison table lives in
[`docs/LANGUAGE-ROADMAP.md`](docs/LANGUAGE-ROADMAP.md) §0.1** (with the
controls and the per-workload method); this file does not repeat it.
Reproduce with §2.3.

**The v1 battery is green: 32 gates** (measured 2026-10-26; per-gate counts
in §2.2), and the standalone gates are green with it. The whole battery runs
as one command (`bash tests/v1_gate_check.sh ./flintc --full`: 32/32). The
checker's differential is 181/181 over every `.fl` in the tree, the type
audit reports **0 contract violations and 0 drift**, the two frontends are
compared by the differential gate (14/14), the query-mode gate is 14/14,
the explicit-type-argument gate is 6/6, the closure-escape gate is 7/7, the
input gate is 8/8, the bounds-safety gate is 5/5, the unsafe-flag gate is
6/6, the range-facts gate is 19/19, the vectorize gate is 3/3, and the
bootstrap's fixed point is byte-identical.

---

## 2. Environment, build, and verification recipes

### 2.1 Build (this device)

```bash
cd /data/data/com.termux/files/home/Flint

# fast syntax check (~3-8 min; the full compile is the slow part)
timeout 500 clang++ -std=c++17 -fsyntax-only -I$(llvm-config --includedir) src/main.cpp

# full build (~3 min warm; 12-15 min under load) — ALWAYS detached: a tool timeout kills the
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
bash tests/run.sh                  # smoke (60)
bash tests/test_lexdiff.sh         # token contract
bash tests/test_parse_gate.sh      # 149/149 — parser contract
bash tests/test_fixpoint.sh        # 149/149 — self-host byte-identical
bash tests/test_errors.sh          # 149/149 — stage3 emitter catalog
bash tests/test_errors_cpp.sh      # 137/137 — C++ fixed-pattern catalog
bash tests/test_differential.sh    # 14/14  — C++ vs self-host pipeline
bash tests/test_signatures.sh      # 14/14  — query mode + pipeline overlay
bash tests/test_explicit_targs.sh  # 6/6    — pipeline `::<T>` (2E)
bash tests/test_closure_escape.sh  # 7/7    — escaping closures (2D)
bash tests/test_input.sh           # 8/8    — bounded line input (flint_input)
bash tests/test_type_check.sh      # 181/181 — checker + differential (whole tree)
bash tests/test_type_audit.sh      # 156 checks, 16 skipped, 0 drift, 82 facts
bash tests/test_generics.sh        # 7
bash tests/test_no_hang.sh         # 88
bash tests/test_ai.sh tests/test_rust_stdlib.sh tests/test_rust_ffi.sh \
    tests/test_rust_oracle.sh tests/test_https.sh tests/test_python.sh \
    tests/test_lsp.sh tests/test_fmt.sh tests/test_opt_identity.sh \
    tests/test_cache.sh tests/test_fuzz_corpus.sh tests/test_aegis.sh
bash tools/docs_check.sh           # README counts vs tree
```

The pipeline/corpus gates use the AOT stage binaries in
`$TMPDIR/slipstream/self` (`lex.0`, `parse.0`, `emit.0`). Build them once
with `bash tests/test_selfhost.sh A`; `test_explicit_targs.sh` fails rather
than skipping when they are absent, and the corpus gates use them when
`SELF_DIR` points at that directory.

**The v1 battery (2026-10-26, 32 gates, all green):**
`bash tests/v1_gate_check.sh ./flintc --full` → **32 passed, 0 failed**.
Every gate prints its own count; the counts re-measured at the 2D/2E landing:

| gate | measured 2026-10-25 |
|---|---|
| type-check (in `--full`) | 181/181 — 12 fixtures + one differential pair per `.fl` in the whole tree |
| type-audit (widened, in `--full`) | 156 checks passed, 16 skipped (deliberate error demos), 0 failed, 82 call facts |
| parse-gate | 149/149 |
| fixpoint | 149/149 |
| differential | 14/14 (0 known divergences) |
| signatures | 14/14 — query-file content + determinism, the untyped fix, the no-file control, a partial overlay, a differential pair, E0284–E0289 |
| explicit-targs (new) | 6/6 — the fixture, the mangled names, E0295–E0298 |
| closure-escape (new) | 7/7 — five escape shapes, heap on escape, no heap when local |
| bounds-safety (gated 2026-10-26) | 5/5 — literal OOB traps, unsafe build stamped, policy/IR counts agree |
| unsafe-flag (gated 2026-10-26) | 6/6 — stamp, warning, fewer checks, ownership still enforced |
| range-facts (new 2026-10-26) | 19/19 — elision, overflow traps, branch/return/map bail cases, emit-mode retention |
| vectorize (new 2026-10-26) | 3/3 — a check-free loop carries `llvm.loop.isvectorized`, the safe build keeps `sadd.with.overflow`, the unsafe sum is exact |
| input (new) | 8/8 — values, CRLF, EOF, invalid/overflowing numbers, 1 MiB cap + drain, embedded NUL |
| driver | 7/7 |
| errors | 149/149 |
| errors-cpp | 137/137 fixed patterns / 145 codes |
| diagnostic-codes | 16/16 |
| docs | 64 links, 17 indexed documents (16 live), 0 duplicate paragraphs |

The gates this work did not touch (smoke, generics, no-hang, the
Rust/AI/LSP suites, …) keep the counts they print; the pass/fail line is the
contract. The two diagnostic catalogues are in sync (149 emitter codes +
145 C++ codes; 137 C++ fixed patterns), and the three bootstrap phases
(`tests/test_selfhost.sh` A/B/C) finish with the fixed point byte-identical.

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

**Read this before starting.** The first draft said "add a typed HIR/MIR".
A red-team pass found that vague in a way that would have wasted weeks:

- Flint had **no type checker**. Types were guessed in ~6 places
  (`exprFlintType`, `emitVarDecl`'s ladder, `inferGenericArgType`,
  `typeMangleTag`, the annotation checks). A "typed HIR" that merely
  recorded those guesses would be a typed tree full of lies. **The first
  deliverable is a real checker, not a data structure.**
- Checks are **code today**, emitted inline. They must become *data*
  before an analysis can remove them (2B).
- A full **SSA MIR is a backend rewrite** — the thing we must not do.
  A typed HIR plus a side CFG (for region inference only) is enough.

**The delivery order** (each step independently gated; the long design is
`docs/LANGUAGE-ROADMAP.md` §13/§27, the research §6.11):

| step | deliverable | status (2026-10-25) |
|---|---|---|
| **2A.0** | a **type checker** for the decidable subset (annotated declarations, function signatures, literal types), with `--type-audit` as its differential instrument | **landed**: `TypeChecker` runs as a phase, `E0290`–`E0294`, `--no-type-check`, 181/181 differential, 0 drift |
| **2A.1** | **types carry facts**: an array's exact length, the resolved trait for a call, the instantiated type arguments | **landed**: length (`42b3a04`, `2b45516`); `resolvedImpl` + `instantiatedTypeArgs`, 82 facts corpus-wide |
| **2B** | **checks become IR-level operations** (bounds/overflow as explicit ops) | landed (`37d9b8b`): `SafetyPolicy`, counted and gated |
| **2C** | **analyses**: loop-range facts, then escape analysis | loop-range landed (`a7c6e2b`); escape analysis landed as 2D.2 (intraprocedural, lazy heap promotion) |
| **2D** | **regions** (NLL-style from the CFG) → escaping closures | **landed for escaping closures**: capture-less and capturing values may escape; a capturing box is promoted at the escape site (`tests/test_closure_escape.sh`). Full NLL region inference is still open |
| **2E** | **the pipeline consumes the same semantics** (D5) | **landed through 2E.6**: rename, monomorphizer, traits, the C++ query mode + overlay, and explicit `::<T>` (`E0295`–`E0298`) |

**What "finish 2A" means** (the acceptance criteria, not a vibe):

1. **The checker runs as a compiler phase** — after parse + imports, before
   codegen. It rejects a *provable* type error with a structured diagnostic
   (span + code + `help`), and it never reaches LLVM verification.
2. **It never rejects a program the compiler accepts.** The oracle is the
   compiler on the corpus: the gate runs both paths (`--no-type-check` vs
   default) over every `.fl` in the tree and requires identical accept/reject
   decisions.
3. **The audit's drift is zero.** An annotated declaration is a contract; an
   inferred one is a guess. `--type-audit` reports both; the corpus must show
   **0 contract violations and 0 inferred drifts** before 2A is called done.
4. **The facts are inspectable.** `--type-audit` prints the instantiated type
   arguments and the resolved trait/impl for the calls that have them, and a
   gate checks the printed facts against the fixture's source.

**Landed evidence (the short version; detail is in `memory.md`):**

- **The checker phase** (2A landing commit): `TypeChecker` runs after
  imports + declaration generation and before any function body is emitted.
  A provable error stops the compile with a span, a code and a `help` line:
  `E0290` declaration/global initialiser, `E0291` return, `E0292`
  assignment, `E0293` call argument, `E0294` mixed `i64`/`f64` comparison —
  the five shapes where codegen used to retype silently or emit IR LLVM's
  verifier rejected. `--no-type-check` is the differential oracle, and the
  cache salt includes it. The checker is deliberately conservative: an
  expression shape it does not model is *unknown*, never an invented error.
- **Zero drift, zero contract violations** (2A landing commit; re-measured
  2026-10-25): the whole tree — 169 files, including
  stage1-3/driver/tools/agent-bench — audits clean; the 16 skipped files are
  the deliberate borrow/move/error demos.
  The last drifts were named function values (no parser signature table for
  `h = inc`), `-> ptr` missing from the declaration pre-pass, a void
  declaration's pre-pass consuming the next signature, and extern
  signatures. All four are fixed at the source, not waived.
- **The call facts** (2A.1, 2A landing commit): `CallExprAST` carries the
  instantiated type arguments and the resolved impl (name + receiver type,
  `area for Circle`), recorded by codegen and printed by `--type-audit`.
  The fixture pins `[i64]`, `[f64]` and the impl; explicit `::<T>` calls
  are covered by `tests_manual/t_explicit_targs.fl` (7 facts).
- **One inference engine** (`f0441df`): each builtin's Flint type is derived
  at registration (`gBuiltinTypes`), so the parser and the codegen cannot
  drift on a builtin; the parser learned user-call returns, field access,
  float arithmetic and a declaration pre-pass. Drift **178 → 21 → 0**
  (the last 21 were the tensor-literal rule, fixed in 2A's completion).
- **The length fact** (`42b3a04`, `2b45516`): the bounds check compares the
  index against the same value the loop uses. `sum_array` 10M **19.1 →
  12.0 ms** (1.89× → 1.44× of C), **0 bounds-check calls** in the final IR.
- **The range fact** (`a7c6e2b`): `i*i <= n` hands LLVM an assumption; the
  surviving checks in `primes` go **7 → 1**.
- **Three real bugs found by the audit** (`f0441df`): a forward reference's
  block types were discarded, an explicit `return` did not infer the return
  type, and a `return "hi"` left a void function returning `ptr`. All three
  were valid programs that failed with an internal LLVM error.

**What is NOT 2A (do not smuggle it in):** the typed HIR migration (§13.3),
regions (2D.3), capability types (4.5), and the pipeline's shared semantics
(2E). 2A delivers the checker and the facts; the HIR consumes them later.

### 4.3 P1 — Range analysis → check elimination (roadmap Phase 3/5)

**What**: the measured 13 % overhead comes from two checks per loop
iteration that LLVM cannot remove, and their presence also blocks
auto-vectorization (§6.1).

**Done — bounded range facts (2026-10-26)**: `BoundedRangeAnalysis` in
`src/main.cpp` evaluates the whole-program call graph of eligible functions
(one i64 parameter, i64 return, non-generic) at every constant call root,
memoized by argument. A `[lo,hi]` fact proves an `add`/`sub`/`mul` cannot
overflow, so codegen emits the plain operation and LLVM's own transforms
apply. The analysis is deliberately closed-world: it refuses loops, unknown
values, global stores, non-self calls, function values, and name-based
dispatch, and it runs only for whole-program executables — `.ll`, `.o`,
interface, `--link`, `--target`, and `--check-bce` keep every check. The
mode is part of the module-cache key, so an optimized executable module
cannot satisfy an emit-only request.

**Measured**: default `fib(45)` **11.6–12.6 s → 8.60–9.17 s**, against
`clang -O2` **8.54–8.76 s** in the same session (three alternating runs).
The check on the sum was not just an instruction: its branch blocked LLVM's
accumulator-recursion transform (§6.2). `tests/test_range_facts.sh`
(19 checks) pins the elision, the traps, the emit-mode retention, and every
bail case.

**Done — the middle end gets a TargetMachine (2026-10-26)**: the second
half of the same measurement was a bug, not a trade-off. `runLLVMOptimizations`
built `PassBuilder PB;` with no `TargetMachine`, so `TargetTransformInfo`
reported no vector registers and the loop vectorizer declined **every**
loop. `opt -O2` on the same module vectorized it. Passing a TM for the
module's triple (the pipeline already knows the triple and the object
emitter already creates the same TM) fixed it. Same-session `sum_array`
10M: `--unsafe` **10.1 ms → 7.49–7.88 ms**, against `clang -O2`
**7.60–7.91 ms** — C parity. The default build stays at 11.0–11.1 ms
because its accumulator check is still there; the vectorizer cannot
reassociate a checked reduction. `tests/test_vectorize.sh` (3 checks) pins
the vectorizer metadata, the safe check, and the exact sum. The cost is
compile time: the stage3 emitter `.o` went from ~3.0–3.5 s to ~3.9–4.3 s.

**Remaining**:
1. The safe `sum_array` is 1.42× C. Its bounds checks are gone (2A.1), but
   the accumulator's overflow check still blocks vectorization. A sound
   fix needs a bound on the sum: the measured max-abs guard (a fused
   `sum` + `max(|x|)` pass, then `maxAbs <= INT64_MAX / n`) runs at
   10.7 ms vs 11.0 ms safe and 7.6 ms C — only ~3 %, because the guard's
   own max/abs work eats the vectorization gain. A wrapping operator is
   the explicit alternative; the guard is not worth its complexity.
2. The pipeline's length fact + literal-index BCE. Its `-O2` consumers are
   done (§4.7.1): the self-hosted emitter is 2.37× faster and matches the
   C++-compiled emitter, and the fixed point is byte-identical.
3. Re-measure: the target is "safe within 1.2× of `--unsafe`" and "integer
   loops within 1.3× of C".

**Verify**: `./flintc --check-bce benchmarks/fib.fl` prints
`3 checks emitted, 1 survive O2 (2 elided)` and
`bounded-range facts prove 3 arithmetic overflow checks safe`; the linked
`fib` has no `flint_panic` branch.

**Do NOT** build a vectorizer: LLVM's own vectorizer will do the work once
the checks are gone (§6.1). If SIMD is wanted in the stdlib, follow Go
1.27's portable `simd` package (size-agnostic, emulated fallback) rather
than compiler intrinsics.

### 4.4 P1 — Regions, escape analysis, escaping closures (roadmap Phase 3)

**What**: a closure's value is an env box in the defining frame, so it
cannot escape. Before 2D the compiler refused to return or store it. The
restriction is a consequence of the representation, not a missing language
feature.

**Landed (2D.2, 2026-10-25)**: an intraprocedural escape analysis with
*lazy* heap promotion. A capturing closure created in an escaping position
(return value, global store, call argument) allocates its box with
`flint_malloc`; a closure bound earlier is promoted at the escape site, and
every alias of the box is repointed. A closure that stays inside its frame
keeps the stack box, so the non-escaping case allocates nothing.
`tests/test_closure_escape.sh` pins five escape shapes and checks the
emitted IR for the heap call (and its absence in the local case).

**Still open**:

- **full NLL region inference** from the CFG (the current analysis is
  intraprocedural and syntactic, not a region solver);
- **freeing an escaped box**: there is no closure destructor yet, so an
  escaped box is never freed. A loop that creates escaping closures grows
  the heap; this is a known limitation, not a hidden one;
- the per-thread arena placement the roadmap originally proposed.

**Done when** (met for the escape shapes): `let cb = |x| x + 1;
store_it(cb)` compiles, and the emitted IR shows no heap allocation for the
non-escaping case.

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

#### 4.7.1 The surface review (2026-10-21) — the optimizer and the LSP

The last item of the 2E brief ("look at optimizers and other surfaces")
was a review, so it is recorded here rather than left in a chat log.
Both halves were read out of the code; nothing below is a guess.

**A. The optimizer surface — the gap is NOT "the pipeline optimizes
worse", it is "the pipeline never optimizes, and its consumers do not
optimize either".**

| | C++ compiler | the pipeline |
|---|---|---|
| runs an optimizer | **yes** — LLVM O2 in-process (`src/main.cpp:766-782`, called at `:14517`) | **no** — it writes text IR |
| who optimizes the pipeline's IR | — | the consumer. Since 2026-10-26 every gate, the self-host bootstrap and `driver/flintc.fl` compile it with `clang -O2`; before that, plain `clang` = **-O0** (`tests/test_differential.sh:67`, `tests/test_selfhost.sh:97`, `tests/test_emit_stage3.sh:28`) |
| `--opt-level` | maps to `CodeGenOptLevel` only (`:13905-13915`); the IR pipeline is always O2 | none |
| `--fast` | an explicit **no-op** (`:13868-13872`) | none |
| the array length fact | yes (`:8120-8156`) → the bounds check compares against a *constant* | no → it compares against the length loaded from memory (`stage3/emitter.fl:2026-2029`) |
| literal-index BCE | yes (`:9049-9061`) | no |
| the loop-range `llvm.assume` | yes (`:8547-8561`) | no |
| cold / noreturn / `!prof` weights on the panic and the bounds check | yes (`:1125-1152`) | no (the pipeline's declares carry no attributes) |
| the identity elisions (`x+0`, `x*1`, …) | yes | **yes** — the same shape (`stage3/emitter.fl:5401-5500`) |
| div/mod-by-zero traps | yes | **yes** |

So the *IR quality* gap is: the length fact, the literal-index BCE, the
range analysis, and the cold-path attributes. The *performance* gap that
follows from them is bigger than it looks, because the pipeline's IR is
normally compiled at **-O0** — the same `.ll` at `-O2` is 50,944 B
linked against the C++ post-O2 IR's 62,432 B, i.e. **the pipeline's IR
optimizes BETTER once something optimizes it**.

**The cheapest, highest-value fix was not in the compiler at all — and it
is done (2026-10-26):** the differential gate, the self-host bootstrap,
the stage3 ladder and `driver/flintc.fl` now compile the pipeline's `.ll`
with `-O2` (the driver previously linked it at `-O0`). Measured on the
same self IR: the pipeline-compiled emitter runs **0.452 s → 0.191 s
(2.37×)**, matching the C++-compiled `emit.0` at 0.193 s, and emits
byte-identical IR; a pipeline-built 5M-iteration loop runs **0.096–0.127 s
→ 0.059–0.068 s** with the same output and a 65 KB → 52 KB binary. The
differential is 14/14, the ladder 21/21, the driver 6/6, and the
self-host fixed point is byte-identical under `-O2`. The gates compare
program output, not IR, so the comparison stays valid; the compile-time
cost is visible in the differential gate (30 s → 42 s), which is the
price of actually optimizing 28 small binaries.

**B. The LSP surface — a thin line scanner with one compiler-backed
feature.**

- `lsp/` is 2,006 lines of Rust (5 files). Diagnostics now ask the
  compiler for `--error-format json` (`lsp/src/diagnostics.rs`), so the
  editor gets the compiler's real code, message, `help` and primary span;
  it no longer runs the user's program. Completion, hover, definition,
  references and documentSymbol are still a Rust line scanner over the
  buffer (`lsp/src/analyzer.rs`).
- It never uses the self-hosted pipeline, and it does not link the
  compiler as a library.
- **Fixed since the review**: the JSON path carries `L:C` (the old text
  path lost it), and an unresolved import now produces `E9001` instead of
  a false all-clear (the diagnostic has no span, so it lands at 1:1).
- **Still open**: `::<T>` is unknown; `trait` is not in the scanner's
  keyword table (so it is not completable, and a trait contributes no
  symbol to the outline); generics are cosmetic (the name scan stops at
  `<`); fn values, enum variants and struct fields are not modelled;
  compiler-magic builtins are missing from completion — **`print`, `len`,
  `range` and `py_eval` are not completable** (the generator only scrapes
  `add("…")`, and these are special-cased by name).
- The drift gates are green (the 30 lexer keywords and the 586 builtins
  match the compiler exactly) — so the LSP is in sync with the compiler
  *as far as it models the language*, and blind exactly where the
  compiler grew features after the lexer froze.

**The ordering that follows from the two reviews** (the cheapest first):

1. **Done (2026-10-26): compile the pipeline's IR with `-O2`** in the
   gates and the driver, and measure. It removed the largest *measured*
   performance difference between the frontends (2.37× on the
   pipeline-compiled emitter; 1.7× on a pipeline-built loop).
2. **The pipeline's length fact + literal-index BCE** (the C++ compiler
   has both; the emitter has the tables to do it: `ENFCode`, the array
   length in `{ptr,i64}`).
3. **The LSP's `print`/`len`/`range` builtins** (nine missing items, one
   generator change) and **the codegen diagnostic position** (one
   regex).
4. **The LSP's import diagnostic** (a false all-clear is worse than no
   feature).
5. The trait form in stage2 + the trait resolution in stage3 (2E.3/2E.4).

**Not now, and why:** the LSP being "compiler-backed" (4.7's real fix)
needs the compiler's type checker to answer questions (4.2), and the
compiler-backed LSP would then be *slow* unless the checker is fast —
the TypeScript-7 precedent in 4.7 says: make the checker fast first.
So the LSP work waits for 4.2; the three cheap items (1, 3, 4) do not.

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

**What**: the self-host pipeline (stage2/stage3) still lacks enum
equality and named function values, while the C++ compiler has both.
Generics, traits, explicit `::<T>` and the C++ compiler's resolved
signatures are now shared (2E.2-2E.6). The differential gate is green
(14/14) but it only covers the programs both frontends can compile.

**Do**: after 4.2, make the pipeline emit the shared HIR rather than
re-implementing the semantics. Until then, D5 is the honest record.

**Verify**: `grep -c typeParams stage2/parser.fl` → 0 today.

#### 4.9.1 2E status: generics, traits, and the query mode (2E.1-2E.6)

**Landed** (2026-10-21/25): the rename (`16adde0`), the monomorphizer
(`48c12d5`, the pipeline produces the C++ compiler's own mangled instance
names), the trait form (2E.3) and its name+arity resolution (2E.4), the
query mode (2E.5: `flintc --emit-signatures` + the emitter's overlay), and
explicit `::<T>` call syntax (2E.6: `(call-t NAME (targs T*) ARGS...)`,
`E0295`–`E0298`). The differential gate is **14/14**, the query-mode gate
**14/14**, and the explicit-type-argument gate **6/6**.

**The design, the S-expression contract and the lessons learned are in
[`docs/PIPELINE-GENERICS.md`](docs/PIPELINE-GENERICS.md)** — that is their
single home; do not restate them here. The implementation history is in
`memory.md`.

**The deviations that remain (recorded, not hidden):**

| deviation | scope | fix |
|---|---|---|
| `bool` mangles as `.i64` | the pipeline has no bool type code; the C++ says `.bool`. Same behaviour, different symbol name | give the pipeline a bool code |
| arrays are `[i64]` only | an array type argument mangles `.arr`; the element type cannot vary (M7 is deferred) | M7 |
| one entry per method name in the function table | two implementations of one method name cannot coexist; the C++ keeps a multimap and filters on the receiver | make the table a multimap |
| the query file covers one file | a multi-file build passes the main file's signatures; imported functions keep their scanned signatures (safe, not invisible) | query each file, or merge the files in the driver |

**2E has no open syntax gap.** The query mode landed: the pipeline consumes
the C++ compiler's answers for the functions a file lists instead of
re-deriving their types (`PIPELINE-GENERICS.md` §5.1). The remaining
deviations in the table are representational (bool, arrays, multimap,
per-file query), not missing syntax.

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

**§11 is the plan** (transpiler, diagnostics, the four selling points,
positioning) and **§12 is the score plan**; their full text lives in
`memory.md`. **The domain design is §6.5** (a policy table, not a compiler
variant) — that is its single home.

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

**Result**: 8,048 B raw / 4,160 B stripped (re-measured 2026-10-25;
the fix-day measurement was 4,536 B — later code changes account for the
difference, not a different measurement method); `.bss` 24 B; `libc` only.

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

**Follow-up (2026-10-26): the recursive `fib` gap was the same lesson with
a twist.** LLVM already removes the two `n-1`/`n-2` checks, but the check
on `fib(n-1) + fib(n-2)` survived, and its branch is what stopped LLVM's
accumulator-recursion transform: the function stayed a tree of calls.
Patching that one check to a plain `add` in the IR restored the loop and
took `fib(45)` from ~11.6 s to ~8.6 s, matching C. A provably-safe check can
therefore cost far more than its own instructions. `BoundedRangeAnalysis`
(§4.3) proves the sum fits for a closed-world executable, so the check is
not emitted; every artifact that can be called from outside keeps it.

**Follow-up 2 (2026-10-26): "Flint cannot vectorize" was itself a bug.**
The table above says Flint had **0** vector operations. That was not the
safety policy: `runLLVMOptimizations` built a `PassBuilder` with no
`TargetMachine`, so TTI reported no vector registers and the loop
vectorizer declined every loop in every build. `opt -O2` on the identical
module vectorized it. After passing a TM for the module's triple,
`--unsafe sum_array` went from 10.1 ms to 7.49–7.88 ms (C: 7.60–7.91 ms,
same session). The default build still has the accumulator check, so it
stays at 11.0–11.1 ms — the check, not a missing vectorizer, is the
remaining cost. This is the second time the measurement caught a
tooling assumption instead of a language trade-off.

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
- "Write the LSP in Flint" — blocked by D5: 2E closed the
  generics/traits/`::<T>` syntax gap, but the pipeline covers the
  differential subset (14 fixtures), not the whole language, and its
  method table holds one entry per name. An LSP would need both fixed.

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

### 6.12 Three ideas from Zig, mapped onto Flint (2026-10-21)

Notes brought in from outside; each is mapped onto what Flint actually
does today, with the code that decides it. They are recorded here
because two of the three touch decisions this project has already made
(§4.4, §6.5, §6.11) — a note that contradicts a decision is worth
catching *before* it becomes a plan.

**1. Explicit allocation** (Zig removes automatic memory management;
every allocation needs an explicit allocator).

- Flint today: **implicit** allocation with no GC — `flint_vec_new`,
  `flint_map_new` and `flint_array_alloc` (`runtime/rust/src/array.rs:213`)
  are called by the runtime and by the emitter's generated code; the
  leaks are documented as "a separate story, not memory safety" (the
  sanitizers gate's own comment ✓).
- What explicit allocation would cost here: the runtime's C ABI, the
  Rust stdlib, every emitter-generated call site and the FFI boundary
  all change ✓ — and the language's implicit-ownership surface changes
  with them ✓.
- **Verdict: not now, and not as a replacement.** It is an ABI-level
  change, and §6.6's "leases + a runtime backstop" is the deliberate
  alternative ✓. The *additive* version — an explicit-allocator escape
  hatch (a runtime entry that takes an allocator) — is compatible with
  the current model ✓ and is the only form worth considering ✓.

**2. Alias tracking** (Zig tracks the *sequence* of data updates instead
of banning multiple references).

- Flint today: the Rust-like model — a move plus a borrow count
  (`Symbol::moved`, `Symbol::borrowCount`, `src/main.cpp:2463-2464` ✓),
  on top of the leases ✓.
- The project's own rule: **"no second ownership system"** (§4.4 ✓,
  §6.11 ✓) — Flint's leases are the user-visible model ✓, and adding a
  borrow checker on top of them would create two competing systems ✓.
- **Verdict: this is a *replacement* question, not an addition** — Zig's
  model is not a second system only if it *replaces* the lease model ✓.
  That is a whole-program analysis question, so it belongs with the
  region work (2D.3 ✓) and not with a feature list ✓. The honest
  statement: this is the one place where Flint's model is *weaker* than
  Zig's (Zig's programs are not "banned" from aliasing ✓, they are
  *checked* ✓) — and the price of Zig's model is an analysis that must
  be right ✓.

**3. Experimental dialects** (Zig: a variant of the language with
different rules).

- Flint today: the **domains** (§6.5 ✓) — a file-level `@domain(ai)`
  declaration, a *policy* table, explicitly **never semantics** ✓ and
  **diagnostics never change** ✓.
- The difference matters: Zig's dialects change the *semantics* ✓; a
  Flint domain changes which lints/errors are on ✓ — closer to a lint
  profile ✓ than to a dialect ✓.
- **Verdict: the idea is already here under a different name** ✓. If a
  semantics-changing dialect is wanted, §6.5's "never semantics" line is
  exactly the line to cross deliberately ✓ — and the decision log's
  "domains are a data table, not code paths" (§6.11 ✓) is the guard
  that keeps it from forking the compiler ✓.

**The one-line summary:** two of the three are already Flint's shape
(the domains, the implicit-ownership-with-a-runtime-backstop), and the
third (alias tracking) is a *replacement* decision that belongs to the
region work, not to a feature list ✓.



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
| `src/main.cpp` | the whole compiler (~16K lines). Key sites: `spawnLinker` (~293, the link line — §6.1), `emitCompareValues` / `createCallChecked` / `emitPrintCall` / `emitArray*Core` (the shared cores, P0-NEW-1), `emitEnumEqual` (P0-NEW-2), `inferGenericArgType` (M2c, strict), `namedFnThunk` / `namedFnValue` (named-fn-as-value), `llvmType` (array case = `{ptr,i64}`), `Type::fn_` |
| `tests/run.sh` | the smoke suite + `check_fail` cases (60 checks) |
| `tests/test_*.sh` | the standalone gates; the v1 battery is 28 (§2.2) |
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
   full build takes ~3 min warm (12–15 min under load); a "failing" test may be testing yesterday's
   compiler. Always check `ls -la flintc` against your edit time.
2. **A tool timeout kills the process group** — never run a >8 min command
   in the foreground. Detach and poll.
3. **The build can be OOM-killed silently** (3.6 GB RAM, a 16K-line
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

---

## 11. The plan from the 2026-10-21 research pass — POINTER

### 11.5a What landed on 2026-10-21/22 (the diagnostics overhaul, §11.4)

The first item of the plan is DONE for both compilers' *codes* and for
the C++ compiler's *structure*:

| piece | state |
|---|---|
| the Diagnostic structure (span, code, level, labels, help, notes) | **done** — one structure, two renderings ✓ |
| the rendered window (rustc's shape: the header, the `-->`, the gutter, the source line, the `^^^`/`---` underlines) | **done** ✓ |
| the JSON stream (`--error-format json`, one object per line) | **done** ✓ — rustc's `--error-format json` shape ✓ |
| the stable codes | **done** — 145 for the C++ compiler, 149 for the emitter, in ONE space (no two diagnostics in the project share a code) ✓ |
| the code tables | **done** — `docs/error-codes.csv`, `docs/error-codes-pipeline.csv` ✓ |
| the gate | **done** — `tests/test_diagnostic_codes.sh` (16 checks ✓) ✓ |
| the suggestions (Levenshtein) | **done** — "there is a name with a similar spelling: `counter`" ✓ (rustc's wording: a statement, not a question ✓) |
| the LSP reads the JSON | **done** — and it never fakes an all-clear ✓ |
| the error poisoning | **done for the catalogued shapes** — the parser stops at the first error and the codegen side stops after its first error (`tests/test_one_error.sh`, 5/5, also run by CI); broader semantic-error suppression is not claimed ✓ |
| the pipeline's SPANS | **not done** — the parser could have one ✓ (it knows the token's position) but the emitter cannot: the S-expression carries no positions ✓. Putting them in is a format change (both frontends, the goldens, the ladder) ✓ — the next step ✓ |
| the `help:` with a suggested FIX (a span + a replacement) | **not done** — the help line suggests a NAME ✓, not a patch ✓ |

**The cost is measured in `memory.md`** (compiler +22 KB, artifact
unchanged); the score effect is part of §12.3.

**The full text is in `memory.md`** ("The 2026-10-21 research pass +
peer-comparison plan", at the end of the file) ✓.

The four idea-clusters from the research, and what each needs from Flint:

| cluster | the plan in one line |
|---|---|
| **C/C++ → Flint transpiler** | Clang LibTooling for the front (never a hand-written C++ parser) → pointer classification → ownership inference → idiomatic Flint source, so LLVM's O2 does the "peak optimization" ✓. The shippable subset is **C, not C++**, and it must **refuse** what it cannot prove ✓. It needs the `[T]` containers as its safe target and an `unsafe {}` block ✓. |
| **Rust-like diagnostics** | the gap is exactly a single `line:col` and a free string ✓. The work: a Span on every AST node, a structured Diagnostic (code, severity, title, labels, suggestion), codes derived from the existing 273-message catalogue, a renderer, Levenshtein suggestions, error poisoning ✓. **Cheap** (the positions and the catalogue exist ✓) and **high-leverage** (the CLI, the LSP and the transpiler consume the same structure ✓). |
| **the four "Flint Twist" selling points** | instant ignition → the incremental story ✓; embedded/edge → the `[T]` containers, a freestanding build, the arena story in the core language ✓; data-streaming → async and the capability model BEFORE any stream syntax ✓; interop → native `.c` import and FFI contract checking ✓ (the lease model ends at the FFI boundary ✓). |
| **the two positioning questions** | *positioning*: a **systems language with an application-language workflow** ✓. *memory safety*: **leases + a runtime backstop** — not ARC (it would erode the 1.44× gap to C), not a GC (it would put a pause-latency and a footprint floor under the embedded story) ✓. |

**The order (§11.6):** diagnostics (spans + codes) → the incremental story
→ native `.c` import → the `[T]` containers → a freestanding build → the
renderer/suggestions/poisoning → `unsafe {}` + the C subset transpiler →
async + capabilities → the pointer/ownership passes.

**What is NOT in the plan** (and why): a vectorizer ✓ (LLVM does it), a
second ownership system ✓, a semantics-changing dialect framework ✓, a
hand-written C++ parser ✓.

---

## 12. The peer-comparison plan — POINTER

**The full text is in `memory.md`** (the same addendum) ✓.

Flint is **6.1/10** (the same 10 categories and weighting as §28): Rust
8.6, Swift 7.6, Go 7.3, Kotlin 6.9, C++ 6.6, Zig 6.1, TypeScript 6.1,
Python 6.0. Flint beats C++ on memory safety (78 vs 15) and binary size
(92 vs 70) and loses on runtime (70 vs 100), ecosystem (12 vs 90) and DX
(66 vs 55) — so it is **below** C++ overall, at the Zig/TS/Python level.

**The one sentence:** Flint's *language* is competitive; Flint's
*adoption* is not — and the two multipliers (ecosystem 10 %, tooling
6 %) are what hold the total at 6.1.

**The order, the arithmetic and the targets are memory.md §12.1/§12.2/§12.5**
(their single home). The short version: type system + capability model →
diagnostics + a compiler-backed LSP → package manager + registry; the
ecosystem line is the one that cannot be engineered directly.

---

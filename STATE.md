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

## 0.1 CONTEXT WATCH (living block — update at every step)

**Purpose:** if a future session (or a compacted one) has no memory of this
work, this block says where the work stands, what is in flight, and what the
next action is. It is the first thing to read and the first thing to update.

| field | value |
|---|---|
| current phase | **Phase 2, steps 2A -> 2E** (the compiler-unification work) |
| last completed | 2A.0 the type audit (`b75e4c7`); 2A.1 the length fact (`42b3a04`, `2b45516`); 2A one inference engine + three bug fixes (`f0441df`); 2C the loop-range analysis (`a7c6e2b`); 2D escaping capture-less closures (`57db1a4`); 2B the safety policy (`37d9b8b`); 2E measured + specified (`66f85ae`); **2E.1 the pipeline rename** (`16adde0`); **2E.2 the monomorphizer** (`48c12d5` — the pipeline now compiles generics, with the C++ compiler's own symbol names); **the optimizer / LSP surface review** (§4.7.1); **2E.3/2E.4 the trait form and its resolution** (the pipeline now compiles trait programs) |
| in flight | (none — see the git log for the newest commit) |
| next action | **§11.4 step 1+2: spans + structured diagnostics + error codes** (§12.3 puts this at +0.6 overall — the cheapest move that reaches C++'s score ✓) (the first item of the research plan ✓ — cheap ✓, unblocks the LSP ✓, and it is the interface the C→Flint transpiler needs ✓). Then the C++ query mode (the last 2E item: the pipeline should consume the C++ compiler's answers instead of re-deriving them ✓ — see `docs/PIPELINE-GENERICS.md` §5), and the three cheap items the optimizer / LSP review found (§4.7.1): compile the pipeline's IR with `-O2` in the gates and measure; give the LSP the nine missing builtins (`print` among them) and the codegen diagnostic position. The trait form and its resolution are DONE (2E.3/2E.4 ✓); the explicit `::<T>` call syntax is still a stage2 gap ✓. |
| then | the escape analysis for closures that DO capture (2D.2), and the region work (2D.3) |
| build | `bash build.sh` — ~2 min; poll at 60-120 s, never with a long sleep |
| gates | `bash tests/<name>.sh` — the whole set is ~4.5 min; run it detached and poll |
| invariant | the tree must be GREEN and COMMITTED at every step; if a step cannot be finished safely, leave it documented rather than half-done |
| do not | hand-write a vectorizer (the roadmap rejects it); use `llvm.assume` for range facts instead |

**If you are reading this with no memory at all:** §2 has the build and gate
recipes, §4 has the open items with a "verify it is still open" command for
each, §6 has the findings that justify the plan, §8 has the traps (read §8
before running anything), and the git log has one commit per completed step.

## 1. Current position

**Flint is a 0.22.0 systems language with a safety model, a
batteries-included standard library, and a compiler that is also its
toolchain.** The C++ compiler (`src/main.cpp`, ~13K lines) is authoritative;
a self-hosted Flint pipeline (stage1/2/3) exists as the bootstrap ladder.

Measured on this device (Termux AArch64, 3.6 GB RAM, clang 21.1.8, rustc
1.99.1, Python 3.14.6) — reproduce with §2.3:

| workload | Flint | C | C++ | Rust | Python |
|---|---|---|---|---|---|
| sum_array 10M | **12.0 ms** | 8.3 ms | 8.7 ms | 8.2 ms | 178.4 ms |
| primes 10M | **451 ms** | 191 ms | 184 ms | 193 ms | 2046 ms |
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
bash tests/run.sh                  # smoke (56)
bash tests/test_lexdiff.sh         # 133  — token contract
bash tests/test_parse_gate.sh      # 134  — parser contract
bash tests/test_fixpoint.sh        # 134  — self-host byte-identical
bash tests/test_errors.sh          # 139/139 — stage3 emitter catalog
bash tests/test_errors_cpp.sh      # 134/134 — C++ compiler catalog
bash tests/test_differential.sh    # 13/13  — C++ vs self-host pipeline
bash tests/test_generics.sh        # 7
bash tests/test_no_hang.sh         # 88
bash tests/test_ai.sh tests/test_rust_stdlib.sh tests/test_rust_ffi.sh \
    tests/test_rust_oracle.sh tests/test_https.sh tests/test_python.sh \
    tests/test_lsp.sh tests/test_fmt.sh tests/test_opt_identity.sh \
    tests/test_cache.sh tests/test_fuzz_corpus.sh tests/test_aegis.sh
bash tools/docs_check.sh           # README counts vs tree
```

**The whole suite, counted (2026-10-21, 36 gates, all green):**

| gate | count | | gate | count |
|---|---|---|---|---|
| smoke | 56 | | print-size | 11 |
| parse-golden | 2 | | one-error | 6 |
| parse-gate | 134 | | unsafe-flag | 6 |
| fixpoint | 134 | | bounds-safety | 5 |
| lexdiff | 133 | | type-audit | 125 files, 0 violations |
| differential | 13 | | sanitizers | 10 |
| generics | 7 | | merge | 6 |
| no-hang | 88 | | driver | 6 |
| ai-stdlib | 26 | | emit | 3 |
| rust-stdlib | 20 | | emit-stage3 (the ladder) | 21 |
| rust-ffi | 8 | | tutorial | 8 |
| rust-oracle | 2 | | opt-identity | 11 |
| aegis | 16 | | dwarf | 16 |
| https | 4 | | python | 4 |
| target | 9 | | lsp | 5 |

= **764 assertions** across the 36 gates, plus the two diagnostic
catalogs (139 emitter + 134 C++ messages, both in sync ✓), the
type-audit over 125 files ✓ and the three bootstrap phases
(`tests/test_selfhost.sh A/B/C` — Phase C byte-identical ✓). The v1
battery (`bash tests/v1_gate_check.sh ./flintc --full`) runs 19 of them
as one command ✓: 19 passed, 0 failed ✓.

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
| **2A.0** | a **type checker** for the subset that is decidable today: local variable declarations, function signatures, literal types | it must AGREE with the compiler's current behaviour on the whole corpus (a differential gate) — and the gate must also report where the current guesses are WRONG, which turns it into a bug-finder. **STARTED**: the `flintc --type-audit` half of it is done (see below) |
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
- **The builtin form was then wired to the fact (commit 2b45516) — and
  that is where the win came from.** The shared array cores take VALUES,
  so the fact (which lives on the binding) never reached them; the AST
  call site now looks the argument up and passes it along. Measured:

  | workload | before | after | gap to C |
  |---|---|---|---|
  | sum_array 10M | 19.1 ms | **12.0 ms** (-37 %) | 1.89x -> **1.44x** |
  | primes 10M | 495 ms | **451 ms** (-9 %) | see the note below |
  | bounds-check calls in sum_array's final IR | 3 | **0** | - |

  primes' relative gap looks worse only because the C baseline moved in
  that run (220 -> 191 ms); its absolute number improved.

- **What still does NOT work (measured, not guessed)**: primes improves
  much less, because its bound is not provable the same way — the loop is
  `while i * i <= limit`, and LLVM cannot derive `i <= limit + 1` from
  `i * i <= limit`. That is a **range-analysis** limit (step 2C), not a
  length-fact limit: the fact gives the compiler the length, but proving
  the INDEX is below it needs a range analysis over the loop.

  The measurement baseline was re-taken (the old one would have read the
  improvement as a regression).

**The vectorization question, measured (2026-10-20):** after the fact
reached the builtins, `sum_array`'s final IR has **0 bounds-check calls**
and **still 0 vector operations** (C's has 16). The loop is *unrolled 2x*
instead (the exit condition becomes `2 * n`), and the accumulator is a
clean phi. So the remaining gap is **not** the checks any more:

| build | sum_array 10M | vs C (8.3 ms) | vector ops |
|---|---|---|---|
| safe (default) | 12.0 ms | 1.44x | 0 |
| `--unsafe` | 9.9 ms | 1.19x | 0 |
| C -O2 | 8.3 ms | 1.00x | 16 |

`--unsafe` removes the overflow check and recovers 17 %, but the loop
still does not vectorize: the vectorizer bails and the loop is unrolled
instead. With the checks gone, the blocker is LLVM's cost model and the
pointer's provenance (the data pointer comes from a runtime allocation),
not anything Flint emits. **Do not chase this by hand-writing a
vectorizer** (the roadmap rejects that): the honest next step is to see
whether a small, targeted hint (or the loop's shape) makes LLVM's own
vectorizer take it, and otherwise to accept ~1.2x on this workload.

**Step 2A.0 — the type audit (landed 2026-10-20, commit b75e4c7).**
Flint infers types in TWO places: the parser's `inferType` and the
codegen's `exprFlintType` + declaration ladder. Nothing compared them.
`flintc --type-audit` now does, at every declaration:

- an **annotated** declaration is a CONTRACT (the program states the
  type) — a disagreement is a BUG;
- an **inferred** declaration is a GUESS — a disagreement is DRIFT, which
  is what step 2A must remove.

Measured over the corpus (123 files): **0 contract violations** and **135
inferred declarations where the two engines still drift**. The drift has
one shape: the parse-time engine answers **i64** (its default) where the
codegen engine knows **str** or **f64** — e.g. `benchmarks/pi.fl:9`
(`denom` parse=i64 codegen=f64), `examples/regex_demo.fl:9` (`replaced`
parse=i64 codegen=str). So the parse-time engine is the weak one, and it
is the one a future LSP or checker would inherit. Gate:
`tests/test_type_audit.sh` (fails on a contract violation, reports the
drift).

**The NEXT slice, designed (so it is not re-derived): a loop-range
analysis that feeds LLVM assumptions.**

Why this is the next one: it is the only remaining item that attacks a
MEASURED gap. `primes` improves 9 % while `sum_array` improves 37 %,
and the difference is entirely the bound:

- `sum_array`: `while i < n` and the array's length IS `n` -> the check
  compares the index against the loop's own bound -> LLVM folds it.
- `primes`: `while i * i <= limit` and the array's length is `limit + 1`
  -> LLVM cannot derive `i <= limit + 1` from `i * i <= limit`, because
  `i * i` may overflow in its reasoning. **The missing fact is
  `0 <= i <= limit`** (for `i >= 0`, `i <= i * i`), and it is a RANGE
  fact, not a length fact.

Design (bounded, and the soundness conditions are the whole point):

1. Recognise the loop shape: `while (COND) { BODY }` where COND is
   `i < n`, `i <= n`, or `i * c <= n` (`c` a positive CONSTANT), `i` a
   local integer variable, and `n` an expression.
2. Establish the loop's induction variable and its range by checking
   the PRECONDITION: `i` is initialised to a non-negative constant
   before the loop; in the body `i` is only ever updated by
   `i = i + k` with `k` a POSITIVE constant; and `i` is not captured
   by anything that could change it (no address-of, no closure capture,
   no other assignment).
3. Inside the body, the range `[0, n]` (or `[0, n/c]`) holds. Emit it
   as an LLVM assumption (`llvm.assume(i >= 0 && i <= n)`) so the
   OPTIMIZER can use it: the bounds check folds, and the vectorizer gets
   the trip-count range it needs.
4. If ANY of the preconditions cannot be established, emit nothing.
   **An unsound assumption is a miscompilation, which is worse than a slow
   program.** The range analysis is therefore opt-in by construction:
   it only fires on shapes it fully understands.
5. Gate it the way the other safety properties are gated: a program whose
   index is out of range must still trap (the assumption is about the
   LOOP's own range, not about the array's length — the array's length
   fact is separate and already tested).

**Explicitly NOT in this slice:** general range analysis, symbolic
execution, or anything that would need a fixpoint. The loop-range shape
above is the whole scope.

**Step 2A — ONE inference engine, and the three bugs it exposed
(landed 2026-10-20, commit f0441df).**

The two engines are now ONE by construction: the compiler derives each
builtin's Flint type at REGISTRATION (`flintTypeOfLlvm` → `gBuiltinTypes`)
and the parser's inference reads that map, so a builtin's type cannot
drift. The parser also learned the shapes it was missing (a call to a user
function, a field access, float arithmetic), and a declaration PRE-PASS
records the file's signatures so a forward reference resolves.

**Drift measured over the corpus (123 files): 178 → 21 declarations, 0
contract violations.** The 21 that remain are forward references to
functions whose return type is INFERRED (not annotated) — single-pass
parsing cannot know those, and guessing would be the silent wrongness this
work removes.

**Three real bugs, all found by the audit, all fixed** — each was a VALID
program that failed to compile with an internal LLVM error ("Function
return type does not match operand type of return inst!"):

1. `fn user() { s = later()\n s }` with `later` declared later — the
   body's variable types were DISCARDED by the block scope restore before
   the return type was inferred, so `user()` was inferred as returning
   i64. Fix: the block's types are kept as a snapshot (`lastBlockTypes`)
   when the scope restores.
2. The same with an explicit `return s`.
3. `fn f() { return "hi" }` — a return type inferred from a RETURN
   STATEMENT had no path at all: the type stayed void and the codegen
   emitted `ret ptr` from a void function. Fix: infer it from the
   body's return statement.

Fixture: `tests_manual/t_return_inference.fl` (four shapes).

**Step 2C — the loop-range analysis (landed 2026-10-20, commit a7c6e2b).**
A recognised loop hands the optimizer a RANGE fact about its induction
variable, as an LLVM assumption: `i < n` and `i <= n` give the condition
itself; `i * c <= n` (c a positive constant) gives the DERIVED `i <= n`
(LLVM cannot derive that itself — the multiply may overflow in its
reasoning — and the derivation is sound for negative i as well).

Measured: `primes` goes from **7 bounds-check calls in the final IR to 1**.

Soundness: the fact is emitted only when the condition shape is
recognised exactly, the induction variable is not assigned before the
access, its address is not taken, and the access reads the variable
itself. The gate `tests/test_bounds_safety.sh` pins the dangerous case
(a loop `while i <= limit` over an array of length `limit` — the fact does
NOT make the access in bounds, so the check must stay and the program must
trap: it does).

**Step 2D — capture-less function values may ESCAPE (landed 2026-10-20,
commit 57db1a4).** The rule now follows the box's lifetime instead of a
blanket ban: a closure that captures nothing has a module-level constant
box, a named function's value has one too, and a value whose box does not
depend on the frame may be returned, stored in a global, or handed to a
callee that keeps it. A closure that CAPTURES still cannot (its box is
frame-bound) — pinned by `tests_manual/t_capturing_escape.fl` ✓.

**Step 2B — the safety checks are ONE policy (landed 2026-10-20, commit
37d9b8b).** `Codegen::SafetyPolicy` decides what is inserted (bounds,
overflow, div-zero) and counts it exactly; the posture was previously two
switches that could disagree. `--unsafe` removes the two PERFORMANCE checks
only; the div-zero trap, the ownership rules, the escape rules and the
one-error contract stay. The gate checks that the policy's counts equal the
compiler's pre-optimization IR count ✓ (a check emitted without the policy
knowing is the failure mode this removes).

**Step 2E — the pipeline gap, MEASURED and SPECIFIED (commit 66f85ae).**
Measured on this device: the stage2 parser ALREADY accepts generic
functions (it emits `(fn idc (tparams T) ...)`) and the stage3 emitter
refuses them ("generics need G6"); the parser does not accept traits at
all (a method signature has no body, which the grammar has no form for).
The implementation is specified in `docs/PIPELINE-GENERICS.md` -- the
S-expression contract, the inference rule, the instantiation steps, the
mangled-name convention (the C++ compiler's, so the differential gate
compares the two frontends symbol for symbol), the trait-resolution rule
(the C++ compiler's M3 rule), and the order of work. It is NOT
implemented: a half-implemented monomorphizer in a 7.6k-line Flint emitter
would break the bootstrap fixed point, which is the ladder's own oracle.

**Dead ends found while doing it (do not retry):**
- The vestigial `IndexExprAST::unchecked` flag ("R2 BCE-lite: proven
  in-bounds") was never set anywhere — a dead field that claimed to
  implement bounds-check elimination. It is now actually used, but the
  flag alone was never the missing piece: what was missing was a PROOF.
- The `--check-bce` COUNT is confounded: LLVM clones loops, so the
  "surviving checks" number can exceed the "emitted" number (negative
  "elided"). Measure the real thing instead: the number of
  `call void @flint_bounds_check` in the final IR, and the runtime.
- The type audit was SILENT on a cache hit (the cached module skips
  codegen, so no audit data was collected) — the same trap as
  `--check-bce`. Both now bypass the cache: a diagnostic that depends on
  cache state is not a diagnostic.
- The **self-host artifacts in tmp are cleaned** by the system, which made
  the differential gate fail with "self binaries" twice for a reason that had
  nothing to do with the code. The gate now falls back to the STABLE
  artifacts in `~/flintwork/self` — but ONLY if they are NEWER than the
  pipeline sources: stale artifacts produce FALSE divergences (last month's
  pipeline rejects what today's accepts), which looks like a compiler bug
  and hides real ones.
- **Absolute timings on this device are not comparable across runs** —
  the same C binary measured 11.5 ms, 15.4 ms and 23.7 ms in three
  consecutive runs while the agent's own runtime used ~46 % of a core.
  Measure the RATIO to a control compiled the same way, interleaved, and
  report the check/IR counts for anything the optimizer decides. The
  measurement dashboard (Phase 0) must do this: a control + a ratio, not a
  raw number.
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
| who optimizes the pipeline's IR | — | the consumer: every gate compiles it with plain `clang` = **-O0** (`tests/test_differential.sh:67`, `tests/test_selfhost.sh:97`, `tests/test_emit_stage3.sh:28`) |
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

**The cheapest, highest-value fix is not in the compiler at all:** compile
the pipeline's `.ll` with `-O2` in the differential gate and the
self-host gate, and the two frontends become comparable *today*. That is
a measurement question first: does the pipeline's IR survive `opt` and
produce the same behaviour? (It must — the IR is the IR.) The gate
compares *program output*, not IR, so the comparison stays valid.

**B. The LSP surface — a thin line scanner with one compiler-backed
feature.**

- `lsp/` is 1,961 lines of Rust (5 files). Its **only** use of the
  compiler is `Command::new(flintc).arg(&path)` with **no flags**
  (`lsp/src/diagnostics.rs:142`) → that is the compiler's **run mode**,
  so **the editor executes the user's program on every debounced
  keystroke** (300 ms, `lsp/src/main.rs:30`). Everything else —
  completion, hover, definition, references, documentSymbol — is a Rust
  line scanner over the buffer (`lsp/src/analyzer.rs:229-282`).
- It never uses the self-hosted pipeline, and it does not link the
  compiler as a library.
- The concrete gaps: `::<T>` is unknown; `trait` is not even a keyword
  (so it is not completable, and a trait contributes no symbol to the
  outline); generics are cosmetic (the name scan stops at `<`); fn
  values, enum variants and struct fields are not modelled; nine
  compiler-magic builtins are missing from completion — **`print`
  itself is not completable** (the generator only scrapes `add("…")`,
  and `print`/`len`/`range`/`py_eval`/`fa_*` are special-cased by name);
  codegen diagnostics lose their `L:C` and land on line 0; **an import
  that cannot be resolved produces no diagnostic at all** (a false
  all-clear).
- The drift gates are green (the 30 lexer keywords and the 586 builtins
  match the compiler exactly) — so the LSP is in sync with the compiler
  *as far as it models the language*, and blind exactly where the
  compiler grew features after the lexer froze.

**The ordering that follows from the two reviews** (the cheapest first):

1. **Compile the pipeline's IR with `-O2` in the gates** (a one-line
   change per gate) and measure. It removes the largest *measured*
   performance difference between the frontends, and it costs nothing.
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

**What**: the self-host pipeline (stage2/stage3) has no traits, no
generics, no `::<`, no enum equality, while the C++ compiler has all four.
The differential gate is green (11/11) *because* it does not cover them.

**Do**: after 4.2, make the pipeline emit the shared HIR rather than
re-implementing the semantics. Until then, D5 is the honest record.

**Verify**: `grep -c typeParams stage2/parser.fl` → 0 today.

#### 4.9.1 The 2E design: monomorphization in stage3 (decided 2026-10-21)

The rename landed (`16adde0`). The next 2E slice is the monomorphizer —
the piece that makes `fn idc<T>(x: T) -> T` compile through the pipeline.
Everything below was read out of the code, not assumed.

**What the code already gives us (the reason this is tractable):**

| need | what exists | where |
|---|---|---|
| a signature table | `EFNMAP`/`EFNN`/`EFNPB`/`EFPTYPE`/`EFNRET`/`EFNVAR` | `stage3/emitter.fl:58-71` |
| balanced-group text reading | `e_skip_balanced_fn()` returns the group's inner text | `stage3/emitter.fl:6643` |
| a type code per expression | `ETYPE` (set by every `e_expr` arm) | `stage3/emitter.fl:30` |
| a type name per struct/enum | `ENTNameOff`/`ENTNameLen` (code − 100) | `stage3/emitter.fl:101-102` |
| **synthesize a function, register it, emit it later** | the lambda mechanism: `e_decl_lambda` registers the signature, stores the text, `e_drain_lambdas` emits it | `stage3/emitter.fl:285`, `:628` |

The lambda mechanism is the *precedent*: the monomorphizer is the same
shape, with a type environment instead of capture slots.

**The design (four pieces):**

1. **Template registry (new).** A function whose `tparams` group is
   non-empty is a TEMPLATE: it is registered in a new `ETPLMAP` (not
   `EFNMAP`), with its type-parameter names, its parameters' *type words*
   (text, not codes — a type parameter has no code), its return type word,
   and the text spans of its parameter group and body.
2. **Type environment (new).** During an instance's emission, each type
   parameter is bound to a concrete type code and name. Two hooks
   consult it: `e_ann_code()` (the type word → code) and the struct-name
   lookups (`e_struct_lit`, `e_ann_code`'s enum branch). **No textual
   substitution** — the earlier draft in `docs/PIPELINE-GENERICS.md` §3
   said "textual over balanced groups"; that is wrong, because a textual
   pass over the body would also rewrite a *string literal* or a
   *variable* that happens to be spelled `T`. The environment is the
   structural version of the same idea, and the emitter already resolves
   types through exactly these two functions.
3. **Inference at the call site (by-parameter rule, as the C++ compiler
   does).** For each type parameter, find the FIRST parameter whose
   declared type word is that type parameter, and take the type of the
   argument in that position. Un-inferrable → the C++ compiler's
   diagnostic (`cannot infer type parameter 'T' for 'f'`), never a guess.
4. **Instantiate, mangle, register, drain.** The instance's S-expression
   is `(fn <mangled> (tparams) (params) ret body)`; the mangled name is
   the C++ compiler's (`idc.i64`, `idc.s:Inner`, `idc.arr` — the
   injective `typeMangleTag` scheme, `src/main.cpp:9722`). The instance
   is registered in `EFNMAP` *before* its body is emitted (the C++
   compiler's recursive-generic bug, `src/main.cpp:9805-9811`), and the
   queued instances are emitted after the main sweep (`e_drain_lambdas`).

**The two frontends must agree on:** the inference rule, the mangle, the
diagnostics, and the type-parameter-to-argument mapping. That is what the
differential gate will check — new cases in `tests/differential/` that
use generics, with the C++ compiler as the oracle.

**The known deviations (to be recorded, not hidden):**

- `bool` is `i64` in this codegen, so the pipeline mangles a bool
  instantiation as `.i64` where the C++ compiler says `.bool`. Same
  behaviour (the C++ `llvmType` maps bool to i64 as well), different
  symbol name.
- the pipeline's array type is `[i64]` only (M7 is deferred), so an
  array type argument is `.arr` and the element type cannot vary.

**Order of work inside 2E:** (1) monomorphization (above); (2) stage2:
the trait form and method signatures; (3) stage3: trait resolution by
name + arity; (4) the C++ query mode. The monomorphizer comes first
because it is the piece the other two depend on (a trait method can be
generic).

#### 4.9.2 The monomorphizer, as implemented (2026-10-21)

**Landed.** The pipeline now compiles generic functions, and it produces
the *same symbol names* as the C++ compiler — the strongest parity
statement available:

| program | C++ compiler | pipeline |
|---|---|---|
| `idc(5)`, `idc(2.5)`, `idc("hi")`, `idc(Inner{a:7})` | `idc.i64`, `idc.f64`, `idc.str`, `@"idc.s:Inner"` | the same four |
| `pair(3,9)`, `pair(2.5,"x")` | `pair.i64.i64`, `pair.f64.str` | the same two |
| recursion, generic→generic, typed local, default arg | `countdown.i64`, `twice.i64`, `local.i64`, `withdef.i64` | the same four |

Both print `5 3 0 9 11 2` for `tests/differential/t_generic.fl` ✓ (the
case is in the differential manifest ✓, so the gate checks it from now on).

**Four bugs the implementation exposed (all fixed in the same change):**

1. **The template spans were read from the wrong text.** The instance
   drain swaps `ESRC` for the instance text, so a template's parameter
   and body spans (which point into the *program* text) became garbage
   → "emit failed" with no diagnostic. Fix: a dedicated `ESRC0` holds
   the original program text ✓.
2. **The argument scratch was global, so a nested generic call
   clobbered the outer call's arguments** (`idc(idc(x))` → "too many
   args calling"). Fix: every call works on its own window of the
   scratch, starting at `argbase` ✓ (the same base-index discipline the
   lambda captures already use ✓).
3. **`main` ending with a void expression emitted `ret i64 0` in an
   `i32` function** → invalid IR → the program does not link. This is
   **pre-existing** (the old emitter does it too ✓ — the differential
   corpus dodged it by ending every `main` with `0` ✓). Fix: `main`
   returns `i32 0` ✓.
4. **A struct-typed fall-through return emitted `ret {i64} 0`** →
   invalid IR (the zero of a struct is `zeroinitializer`). Also
   pre-existing ✓. Fix: `e_llvm_zero` handles the named types ✓.

**One deliberate restriction (to match the oracle):** a generic call
must pass every argument — the C++ compiler does not fill a generic's
default arguments ("'f' expects 2 argument(s), got 1") ✓, so the
pipeline refuses it too ✓ rather than being more permissive than the
reference implementation.

**The deviations that remain (recorded, not hidden):** `bool` mangles as
`.i64` (the pipeline has no bool code) ✓; the explicit `::<T>` call
syntax is still a stage2 gap ✓ (the *declaration* form works ✓).

#### 4.9.3 The trait form and its resolution (2E.3 + 2E.4)

**Landed.** The pipeline now compiles a trait program, with the same
output as the C++ compiler (`tests/differential/t_trait.fl` ✓ — the
differential gate is 13/13 ✓).

**The trait, as the C++ compiler defines it (M3):** a trait carries
SIGNATURES only; an implementation is an ordinary function with the same
name; the call site resolves by NAME + ARITY — exactly one → call it;
none → "no type in scope implements 'm'"; several → "ambiguous". The
parameter and return types of a signature are parsed and then DROPPED
(the C++ does the same: `if (match(TokenType::COLON)) parseType();` —
signatures only).

**stage2 (the parser).** `trait` is a contextual keyword (an identifier
to the lexer ✓ — the same as the C++ ✓). The new form is

```
(trait NAME ( (method1 arity) (method2 arity) ))
```

The parser reads the members as `fn NAME ( params ) [-> Type]` ✓,
counts the parameters ✓, and drops the types ✓.

**stage3 (the emitter).** A trait emits no IR ✓ — it only records the
method names (`ETRMETH` ✓) so that a call to an unimplemented one can
say so ✓. A method call on a user type (`(method BASE NAME args…)` ✓)
resolves by name + arity ✓: the implementation's first parameter is the
receiver ✓, so the implementation must take exactly one more parameter
than the call has arguments ✓, and the receiver is converted to that
parameter's type ✓.

**The one limitation (recorded, not hidden):** the pipeline's function
table holds ONE entry per name ✓, so two implementations of the same
method name cannot coexist ✓ — the C++ compiler keeps a multimap and
filters the candidates on the receiver's type ✓. A program with two
implementations of one method name compiles in the C++ and is refused by
the pipeline ✓ (the pipeline's diagnostic is "no type in scope
implements" ✓, the C++'s is the ambiguity error ✓). Making the
pipeline's table a multimap is the fix ✓ — it is a table change, not an
algorithm change ✓.

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

**§12 is the score plan** (the peer comparison: Flint 6.1/10 today,
and the arithmetic that says which category is worth the most effort ✓)
and **§11 is the work plan** (the C→Flint transpiler, the diagnostics
overhaul, the four selling points, and the positioning/memory-safety
answers ✓).

**§11 adds the plan that came out of the 2026-10-21 research pass** (the
C→Flint transpiler, the Rust-like diagnostics, the four "Flint Twist"
selling points, and the positioning/memory-safety answers). Its first
item — spans + structured diagnostics + error codes — is cheap, unblocks
the LSP, and is the interface the transpiler and the LSP both need.

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

---

## 11. The plan from the 2026-10-21 research pass — POINTER

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

**The arithmetic that decides the order** (§12.1): a point of ecosystem
score is worth **+4.8**, the capability model **+3.3**, the type system
**+3.0**, diagnostics + LSP **+3.8**, the regions **+2.1**, the range
analysis **+2.3**. The biggest line is the one that **cannot** be worked
on directly — the ecosystem is the *output* of what other people do with
the language ✓.

**The adoption ladder** (§12.2): type system + capability model →
diagnostics + a compiler-backed LSP → package manager + registry → the
ecosystem score moves ✓. It is the §28 weighting's own logic: a package
manager without a sound compiler and a fast compiler just distributes
bugs ✓.

**The targets** (§12.5): **6.6** (the diagnostics + the LSP's cheap
items) = C++ today ✓; **7.3** (the type system + the capability model) =
Go today ✓; **8.0** (the compiler-backed LSP + the runtime work + the
cross-platform runs) ✓; **8.6+** (the package manager, the registry) =
Rust today ✓ — and that last row is not engineerable by us alone ✓.

---

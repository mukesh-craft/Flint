# Flint — Research & Improvement Plan

**Date:** 2026-10-03 · **Device:** Termux AArch64, LLVM 21.1.8, 3.6 GB RAM
**Method:** every number below was measured or read in this tree. Web claims are cited.

---

## Part 1 — Verified state

### 1.1 The single largest perf finding: opaque bounds checks

`emitLoopBoundsCheck` (`src/main.cpp:5271`) gated its inline `icmp`+`br` behind
`--loop-opt`, which defaults **off**. So the array *builtins*
(`flint_array_get`/`flint_array_set`) emitted an opaque `bl flint_bounds_check`
on every iteration, while the `a[i]` *syntax* (`emitIndexAccess`) always inlined.
An opaque call blocks inlining, unrolling and loop vectorization.

Same algorithm, same compiler, two spellings (`benchmarks/sum_array.fl`, 10M):

| Form | Before | After |
|---|---|---|
| `flint_array_get/set` builtin | 65.4 ms | **18.7 ms** |
| `arr[i]` index syntax | 18.5 ms | 18.5 ms (already inline) |
| C `-O0` | 41.3 ms | — |
| C `-O2` | 8.2 ms | — |

**3.5× on the flagship benchmark.** Flint is now **2.2× faster than clang `-O0`**
and 2.3× short of `-O2`. Output verified identical to C (`99999990000000`);
bounds traps still fire with byte-identical messages, negatives included
(the `icmp ult` unsigned wrap catches them).

Four call sites now share one helper (`Codegen::emitBoundsCheckInline`):
the AST `a[i]` path, both array builtins, and the two single-pass
emit-mode paths (`emitIndexEmit` / `emitIndexStoreEmit`).

`primes.fl` uses the same builtin pattern and benefits identically.

### 1.2 Checked arithmetic is what blocks vectorization (not a codegen bug)

memory.md concluded "checks are NOT the bottleneck; loop codegen quality is."
That is wrong, and here is the control experiment that shows it.

The same loop written in C, compiled by clang -O2:

| C loop | Vectorized? |
|---|---|
| `for(i) s += a[i];` | **Yes** — 2-wide, two partial accumulators, `llvm.vector.reduce.add.v2i64` |
| same + `__builtin_add_overflow` | **No** — zero `vector.*` |

Identical IR shape to Flint's. So `sadd.with.overflow` + branch *structurally*
blocks integer-reduction vectorization — in clang too. It is not something a
better backend can route around.

This explains the shape of the existing benchmark table exactly:

| Benchmark | vs C | Why |
|---|---|---|
| `pi` (f64) | 1.04× | FP reductions *are* reorderable → `pi.fl` **does** vectorize (7 vector ops in its IR) |
| `sum`/`primes` (i64) | 2.2–2.4× | integer reductions are not reorderable under a checked op → no vectorization |

**Implication.** The fix is *not* to weaken the default (that is Flint's whole
pitch, and Rust's own answer is to check in debug and wrap in release — RFC 560).
The fix is to give hot numeric code a type whose operation is associative *by
definition*: wrapping integers `u8/u16/u32/u64`. `u32 + u32` wraps, so it
vectorizes; checked `i64` stays the default. Additive, no safety loss, and it is
the only real route to `-O2` parity on integer reductions.

### 1.3 Soundness gaps found in the tree

1. **`verifyModule` is disabled** — `src/main.cpp:4374` reads
   `if (0 && llvm::verifyFunction(...))`. Invalid IR is emitted silently. This
   is the root of the `--fast` segfault that memory.md has carried as an open
   P0: without a verification gate, a malformed-PHI bug only shows up as a
   mystery crash under ORC.

2. **`AegisChecker` no longer exists.** memory.md:296 documents a static pass
   "after `BorrowChecker`" and lists `aegis_uaf` / `aegis_double_free` /
   `aegis_branch_uaf` as *compile-time rejects*. Run today, all three compile
   cleanly and only `PANIC` at runtime. It was lost in the 2026-09-14 wipe
   (`git checkout -- src/main.cpp`). The "hybrid static + runtime" claim is
   currently runtime-only.

3. **The borrow checker covers little.** `BorrowChecker::checkStmt`
   (`src/main.cpp:8049`) has no case for `Match`, `Lambda`, `StructLiteral`,
   `FieldAccess`, `Index`, `Ref`, `Deref`, `Slice`, `MapLiteral`, `Unwrap`,
   `Destructure` — all fall to `default: checkExpr`. There is **no loop
   fixpoint**: a `while` body is walked once, so borrow counts never restore
   across iterations and a move inside a loop is not tracked. And
   `Type::isCopyType()` returns true for `Struct`, `Enum`, `Str` and `Map`, so
   moving a non-Copy field out of a struct is never checked — the exact hole
   memory.md's own audit flags ("heap str/map/chan are Copy = UAF").

4. **No LICENSE file.** Publish blocker for a public repo.

---

## Part 2 — Research

### 2.1 Memory safety: what to do beyond a borrow checker

**Polonius is the cheap win.** Polonius Alpha shipped on Rust nightly in
August 2026. The important detail: the 2023 re-formulation was designed to need
only *minimal rearchitecture of the existing NLL code*, which is what finally
made it shippable. It fixes the "NLL problem cases" — safe code NLL rejects
because it tracks lifetimes lexically rather than by dataflow. For Flint this
is the single most importable idea: it is a dataflow upgrade to an existing
checker, not a rewrite.
([blog.rust-lang.org](https://blog.rust-lang.org/2026/08/04/enabling-polonius-alpha-on-nightly/),
[status](https://rust-lang.github.io/polonius/current_status.html))

**Aegis is already the right design.** Flint's lease = `slot<<32 | generation`
is essentially Vale's generational reference, independently arrived at. Vale's
canonical write-up describes the same slot-table scheme; the difference is that
Vale ships **Catalyst**, an optimizer pass that eliminates generation checks for
objects that can't be mutated. Flint has no equivalent — every checked lease op
pays the generation compare forever.
([verdagon.dev](https://verdagon.dev/blog/generational-references))

**The gap that matters most:** Aegis only tracks leases. `str`, `map` and
`chan` are plain heap pointers with no lease, so a use-after-free through any of
them is invisible to the runtime backstop *and* unchecked statically (because
`isCopyType()` says they're Copy). Extending leases to those three types is
worth more than any new checker feature.

**Production evidence for the pitch.** Google's Android numbers: Rust code
shows ~1000× lower memory-safety vulnerability density than the C/C++ baseline,
and memory-safety bugs fell from >75% to <20% of Android's total over four
years, with ~20% fewer revisions.
([blog.google](https://blog.google/security/rust-in-android-move-fast-fix-things/),
[Google Security Blog](https://security.googleblog.com/2024/09/eliminating-memory-safety-vulnerabilities-Android.html))
NSA and CISA published a joint advisory in June 2025 recommending memory-safe
languages for critical infrastructure
([CISA](https://www.cisa.gov/resources-tools/resources/memory-safe-languages-reducing-vulnerabilities-modern-software-development)).

Honest counterpoint: no comparable published CVE statistics exist for
Microsoft's C# migration, and their overall patch volume has not declined.
The Android numbers are the strong evidence; the C# story is not.

### 2.2 Performance: what actually works

**Checked-arithmetic cost is structural** (see 1.2). The productive directions:

- **Wrapping integer types** — the only route to integer-reduction vectorization.
- **Inline every check that can be inlined.** The 3.5× in 1.1 came from exactly
  this. The remaining opaque calls on hot paths are the string builtins
  (`str` is bare `char*`, so every `.len()` is an opaque `strlen` call).
- **Bounds-check elimination.** Go's SSA `prove` pass is the reference
  implementation, and it exposes a *diagnostic*:
  `go build -gcflags=-d=ssa/check_bce/debug=1` prints every surviving check.
  Flint already has the equivalent (`--check-bce`). No authoritative
  "percentage eliminated" figure is published for any language — measure your
  own, per site.
  ([cmd/compile README](https://go.dev/src/cmd/compile/README))
- **Compiler self-speed.** LLVM's own CI uses PGO + ThinLTO + BOLT and reports
  30–40% gains building LLVM; a published recipe combining those plus jemalloc
  reports 1.75× vs a stock Clang.
  ([LLVM Discourse](https://discourse.llvm.org/t/clang-pgo-mystery-am-i-holding-this-wrong/77673),
  [BOLT+PGO+LTO recipe](https://discourse.llvm.org/t/optimized-clang-to-compile-clang-itself-bolt-pgo-lto-jemalloc/90887))
  PGO alone is the cheap 80% of that and is a `build.sh` change.

### 2.3 Project scale: the "too small" premise is wrong

The 500k-lines-for-Zig figure is stale. Zig was **~197k lines when it
self-hosted** (April 2022) and grew to ~700–850k by 2025. Flint is **~33k
lines total** (~9.3k C++ compiler, ~7k self-hosted emitter, ~3.8k self-hosted
parser, ~1.5k runtime) — already roughly 20× under the number in the question,
and under 197k by ~6×.

More to the point, LOC does not predict adoption. SQLite is ~150k lines of C
and ships in every browser, phone, and aircraft. What predicts adoption is
ecosystem, a niche, and sustained maintenance.

---

## Part 3 — Plan, by leverage

### Tier 1 — correctness first (these gate every other claim)

| # | Task | Why | Size |
|---|---|---|---|
| 1 | **Enable the `verifyModule` gate** | Root of the `--fast` crash class; catches malformed PHI/invalid IR before it ships. May surface latent bugs — that's the point. | ~5 lines |
| 2 | **Restore `AegisChecker`** | Restores the documented "hybrid static+runtime" design; 3 tests become compile-time rejects again | ~300 lines |
| 3 | **Add a LICENSE** | Publish blocker | trivial |
| 4 | **BorrowChecker: missing node kinds + loop fixpoint** | `Match`/`Lambda`/`StructLiteral`/`FieldAccess`/`Index`/`Ref`/`Deref` currently unchecked; loops unsound | ~200 lines |

Item 1 is one line and should land first regardless of anything else here.

### Tier 2 — performance

| # | Task | Expected | Size |
|---|---|---|---|
| 5 | ✅ **Inline bounds checks for array builtins** | **3.5× measured** | done |
| 6 | **Inline `flint_str_length`/hot string builtins** | Removes remaining opaque calls in string loops | ~100 lines |
| 7 | **PGO for `flintc` releases** | 20–40% compiler speed; `build.sh` only | ~40 lines |
| 8 | **Wrapping integer types `u8..u64`** | The only route to `-O2` parity on integer reductions | ~600 lines, CPP-only, feature-gated |
| 9 | **Extend Aegis leases to `str`/`map`/`chan`** | Closes the Copy-type UAF hole | ~400 lines |

### Tier 3 — capability

- `verifyModule` + a `--check-bce` default-on report → a *measured* safety/perf
  story rather than an asserted one.
- DWARF line tables via `DIBuilder` → real `gdb`/`lldb` debugging (currently the
  default is `--no-strip`, so nothing is debuggable).
- Stage-2-backed LSP (hover/goto-def/diagnostics) instead of regexes.
- WASI `_start` so `--target wasm32` actually *executes*, not just emits objects.

### Explicitly not recommended

- **Removing checks by default.** It is the product. `--unsafe` already exists
  for people who want it.
- **Re-litigating subprocess backends.** The QBE experiment already answered
  this: 3× *slower* than LLVM -O0 because of process-spawn overhead and IL
  bloat. Cranelift-as-library has the same shape of result (20–35% at similar
  code quality, CGO'24).
- **Fine-grained incremental compilation.** rustc's own dev guide notes
  fingerprinting is costly enough that incremental can be *slower* than clean.
  Flint's content-hash file-level cache is the right rung.

---

## Standing constraints (unchanged)

Gates run **solo** — never in parallel (shared `$TMPD` paths cross-contaminate
and 3.6 GB RAM thrashes). After a killed run, `rm -rf ~/.cache/flintc`.
Never trust `$?` after a pipe. Small diffs, re-green end-to-end.

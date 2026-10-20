# Flint — language & compiler improvement assessment and roadmap

**Status:** engineering assessment, 2026-10-20. Every number is labelled by
kind:

- **[M] measured** — measured on this device (Termux AArch64, 3.6 GB RAM,
  clang 21.1.8, rustc 1.99.1, Python 3.14.6) with the scripts in `tools/`.
- **[P] published** — from a cited external source (2026).
- **[R] reported** — an external report (blog/issue/forum) about something I
  could not reproduce here.
- **[I] inference** — my engineering reasoning from the above.
- **[D] design** — a proposal, not a measurement.
- **[S] speculation** — a guess, flagged as such.

**Rule used throughout:** a claim is never upgraded from [I]/[S] to [M]/[P]
because it sounds right. Where a number could not be measured on this device,
it is marked [P]/[R] or omitted.

**Companion documents:** `memory.md` (project ledger and audit),
`STATE.md` (pick-up guide: how to build, verify, and continue),
`COMPATIBILITY.md` (C++ vs self-host divergence log, D1–D5).

---

## 0. Executive assessment

### 0.1 Where Flint stands [M]

Measured on this device, against the same workloads in the same languages
(`tools/bench_compare.sh`, `tools/bench_tooling.sh` — both new in this
assessment, and both re-runnable):

| workload | Flint | C | C++ | Rust | Python |
|---|---|---|---|---|---|
| sum_array 10M | 19.1 ms | 10.1 ms | 8.3 ms | 9.7 ms | 160.1 ms |
| primes 10M | 495 ms | 220 ms | 192 ms | 207 ms | 2051 ms |
| fib(45) | 12.9 s | 9.6 s | 9.3 s | 8.9 s | 579 s |
| pi 100M | 779 ms | 783 ms | 778 ms | 779 ms | 97.8 s |
| hello-world, stripped | **4,536 B** | 4,368 B | 4,416 B | 339,656 B | ~276 MB (stdlib) |
| hello-world, raw | **8,048 B** | 5,776 B | 5,824 B | 532,624 B | — |
| build time (hello, cold) | 144 ms | 439 ms | 404 ms | 674 ms | — |
| cold start (hello) | 37 ms | 33 ms | 39 ms | 42 ms | 150 ms |
| dynamic deps (hello) | **libc only** | libc+libm | libc+libm | libc | — |
| safety overhead (`--unsafe`) | −13 % int loops, −2.5 % sieve | n/a | n/a | n/a | n/a |

Flint is **1.35–2.25× slower than C/C++/Rust on integer loops, at C parity
on floating-point loops** [M], **4–45× faster than Python** [M], and — after
the link fix described in §6 — it now produces a **hello-world the size of
C's** [M].

The binary-size, build-time and startup rows above are **post-fix**. Before
the link fix (§6) the same measurements were: hello-world 743,520 B
(565,600 B stripped), build 181 ms, cold start 70 ms, dynamic dependencies
libc + libm + libdl + libsqlite3 + libcurl + libopenblas, and 72 MB of
`.bss` reserved in every process [M]. The fix is commit `46a007e`.

### 0.2 Strongest areas

1. **Deterministic memory management without a GC.** AEGIS leases (static
   ownership) + a generational runtime backstop + deterministic free. Go and
   Python trade this for convenience; Rust keeps determinism but has no
   runtime backstop [M: `memory.md` §AEGIS, audited 2026-09-05].
2. **A safety story that is broader than "no UB"**: compile-time
   use-after-free detection, bounds checks, overflow checks, div-by-zero
   traps, and a catalogued diagnostic set (135 messages, two-way sync gate)
   [M: `docs/errors-cpp.md`, `tests/test_errors_cpp.sh`].
3. **Toolchain surface**: one compiler binary that compiles, JIT-runs, emits
   LLVM IR, emits/reuses interfaces, and runs `test_` functions; plus a
   language server, a formatter, an agent-facing benchmark and a token dump
   [M: `--help`, `lsp/`, `agent-bench/`, `--dump-tokens`].
4. **A standard library that is unusually complete for a 0.x language**:
   SQLite, HTTP, crypto, PNG, Brotli, Argon2, regex, CSV, FTS5, bigint, time,
   TOML/YAML, channels, AI/NN layers [M: `runtime/`, `runtime/rust/src/`].
5. **A self-hosting pipeline** whose lexer/parser/emitter are written in
   Flint itself, gated by a 132-case fixed point [M: `tests/test_fixpoint.sh`].

### 0.3 Weakest areas

1. **Integer-loop performance: 1.35–2.25× behind C/C++/Rust** [M]. Part of it
   is safety checks (13 % [M]), part is codegen/IR quality (§4).
2. **No third-party ecosystem** [M: no package registry exists yet]. The stdlib
   compensates, but a language without a package story cannot attract
   contributors.
3. **Type system gaps**: no async/await, no associated types, no operator
   overloading, no type-level programming, closures cannot escape their frame,
   generics are monomorphization-only with no `dyn` escape hatch
   [M: `memory.md` §5 M2, §8 decision 2].
4. **No static concurrency safety**: no `Send`/`Sync` equivalent; `parallel for`
   was removed rather than fixed [M: `memory.md` B6].
5. **Two compiler frontends** (C++ and the Flint stage2/stage3 pipeline) that
   have already drifted: the pipeline has no traits, no generics, no `::<`,
   no enum equality [M: `COMPATIBILITY.md` D5].

### 0.4 Biggest architectural risks

| risk | why it matters | evidence |
|---|---|---|
| **Frontend duplication** | every language feature must be implemented twice; the copies drift silently (three shipped bugs in one session came from exactly this) | [M] `COMPATIBILITY.md` D5 |
| **No stable IR boundary** | the optimizer is LLVM's, so Flint cannot express its own analyses (range/alias/escape) — the safety-performance story cannot be closed inside LLVM | [I] |
| **Safety semantics live in the backend** | checks are emitted ad hoc in two paths; "prove it safe, drop the check" has nowhere to live | [I] |
| **Generic instantiation in the codegen, not a typed IR** | monomorphization is decided at emit time, so diagnostics, LSP and LSP-visible type info cannot reuse it | [I] |

### 0.5 Biggest performance risks

- **Bounds/overflow checks in loops** cost 13 % on integer loops [M], and
  LLVM's elimination is unreliable exactly in loops [P: Rust Heart LLVM
  keynote, 2024-10 — "bounds check elimination: usually works, but not
  reliable; good: constant bounds, straight-line code; bad: checks in loops"].
- **No range/alias/escape analysis in Flint's own IR** → checks can only be
  removed by LLVM's inference, not by proofs Flint makes [I].
- **Array representation is `{ptr, i64}`** with `i64` GEP indexing — no
  element-size type, so the optimizer cannot always relate an index to a
  length [M: `llvmType` array case].

### 0.6 Biggest safety gaps

- Concurrency: no static race freedom; data races are possible and only
  caught by the runtime backstop [M].
- FFI boundaries: the C ABI is exposed, but ownership/lifetime across the
  boundary is not modelled in the type system [I].
- No `Send`/`Sync`-style capabilities, so a closure can be shared across
  threads without a compile-time objection [M].

### 0.7 Biggest tooling gaps

- The language server is a separate Rust implementation that cannot reuse the
  compiler's semantics; it will drift from the compiler exactly like the
  stage2/stage3 pipeline does [M: `lsp/`, D5].
- No incremental-compile story for the C++ compiler beyond a module cache
  keyed by source hash [M: `src/main.cpp` cache].
- No package manager, so no reproducible dependency graph, no lockfile, no
  audit story [M].

### 0.8 Biggest ecosystem gaps

- Zero third-party packages; no registry, no resolver, no lockfile, no
  signing, no vulnerability feed [M].
- The package-adjacent tooling that exists (registry fetch, `flintc --offline`)
  is a cache of *binaries*, not a package manager [M: `src/main.cpp` registry].

### 0.9 Biggest opportunities

1. **Domain-aware compilation** — Flint already has a rich, domain-shaped
   stdlib (AI/NN, crypto, time, serde-like formats) and a compiler that can
   specialize by *program*, not just by flag. §15–§19 design it.
2. **A compiler that is a better citizen for AI agents** — the one thing in
   this comparison that no other language ships: a stable token dump, a
   diagnostics catalog, an agent benchmark, one binary.
3. **Deterministic + safe + fast**: a niche no language occupies — Zig
   approximates it, Go trades it for a GC, Rust trades ergonomics for the
   borrow checker. Flint's AEGIS model is a genuinely different point in the
   design space [I].
4. **A "batteries-included, zero-dependency" distribution model**: one
   binary, one standard library, no package manager needed for most programs.

### 0.10 The 10 highest-priority engineering problems

Priority = engineering value (risk removed × users unblocked) ÷ cost.

| # | Problem | Impact | Root cause | Difficulty | Priority | Expected payoff |
|---|---|---|---|---|---|---|
| 1 | **Link-time binary bloat (743 KB hello-world)** | Critical: 130× the size of C's; every deployment, cache and startup story is affected | `-Wl,--gc-sections` was never in the link line, so the whole stdlib (sqlite/http/crypto/png/Rust) was linked into every program | Low (one flag + dependency hygiene) | **P0** | 8 KB hello-world; 72 MB BSS reservation eliminated; dynamic deps cut from 6 to 1 [M] |
| 2 | **Two frontends that drift** | High: every feature costs 2× and diverges silently | stage2/stage3 (Flint) vs C++ | Very high (a compiler migration) | **P0** | One authoritative frontend; features land once |
| 3 | **No typed IR of Flint's own** | High: blocks range/alias/escape proofs, blocks check elimination, blocks real diagnostics | Codegen emits LLVM IR directly; no HIR/MIR | High | **P0** | The precondition for items 5, 6, 7 and 11 |
| 4 | **Integer-loop gap (1.35–2.25×)** | High | Safety checks in loops + no range analysis + `{ptr,i64}` arrays | Medium | **P1** | Within 1.2× of C for safe code |
| 5 | **No static concurrency safety** | High: the language cannot claim race-freedom | No capability system; `parallel for` removed | High | **P1** | `Send`/`Sync`-like capabilities; safe parallel loops |
| 6 | **Closures cannot escape** | Medium-high: blocks callbacks, decorators, async | Env box is frame-bound by construction | Medium | **P1** | Escaping closures with escape analysis + heap promotion |
| 7 | **No package ecosystem** | High long-term: growth, contributors, credibility | Nothing exists | Medium | **P1** | Registry, resolver, lockfile, signing |
| 8 | **Type-system gaps** | Medium | Deliberate v1 scoping | Medium-high each | **P2** | async/await, associated types, `dyn` escape hatch, operator overloading |
| 9 | **LSP is a separate implementation** | Medium: diagnostics drift from the compiler | The server cannot reuse the C++ semantics | High | **P2** | A compiler-backed LSP with identical diagnostics |
| 10 | **Domain declarations** | Medium: unlocks the stdlib's specialization potential | No source-level policy channel | Medium | **P2** | `@domain(ai)` etc. with safety kept separate (§17) |

---

## 6. Binary-size investigation

### 6.1 The headline finding [M]

The compiler produced a **743,520-byte hello-world** (565,600 stripped) — about
130× C's 5,776 bytes — while the source contained a comment claiming the
opposite ("743 KB instead of 8 KB"). The comment described an intended design
that the shipped compiler did not implement.

### 6.2 Attribution method

Three measurements, in order:

1. **Section-level**: `llvm-size -A` on the linked binary.
2. **Symbol-level**: `llvm-nm --size-sort -S` aggregated by symbol class.
3. **Link-line bisection**: intercept the compiler's own `clang` invocation
   (a shim on `PATH` that logs `argv` and execs the real compiler), then relink
   the same object with subsets of the object set.

The interception step matters: the compiler's link line is built in
`src/main.cpp:spawnLinker` and is easy to mis-read from the source.

### 6.3 Where the 743 KB came from [M]

| component | bytes | share |
|---|---|---|
| `.text` (code) | 313,196 | 42 % |
| `.rodata` (strings, tables, const) | 153,088 | 21 % |
| `.eh_frame` + `.eh_frame_hdr` (unwind tables) | 68,940 | 9 % |
| ELF/dynamic/relocation/PLT overhead | ~25,000 | 3 % |
| remaining (app + libc startup + relocations) | ~133,000 | 18 % |
| **file total** | **743,520** | 100 % |
| `.bss` (reserved, not in the file) | **74,239,728 (72 MB)** | — |

The `.text`/`.rodata` bulk was **not the application's code** (the app object
is 1,352 bytes) — it was the *entire* standard library linked into every
program: SQLite, HTTP, crypto, PNG, Brotli, Argon2, the Rust stdlib.

### 6.4 Root cause [M]

The link line was:

```
clang app.o runtime.o -lm libsqlite3.so libcurl.so \
      flint_tensor.o flint_blas.o flint_ai.o … flint_std.o ffi_helper.o \
      -lopenblas -o out
```

**`-Wl,--gc-sections` was absent.** `build.sh` compiles the runtime objects
with `-ffunction-sections -fdata-sections` (so the sections *can* be dropped),
but without the linker flag nothing is dropped. The link line also carried
`-lopenblas`, `libsqlite3.so` and `libcurl.so` unconditionally, so every
binary acquired three native runtime dependencies it did not use.

A second, subtler cause: the compiler **caches the linked binary** keyed by
source hash (`cache.hasBinary`), so a stale artifact from an earlier compiler
build is served verbatim — a stale-size measurement can survive a source fix.

### 6.5 The fix and its measured effect [M]

| metric | before | after |
|---|---|---|
| hello-world, raw | 743,520 B | **8,048 B** (−98.9 %) |
| hello-world, stripped | 565,600 B | **4,536 B** (−99.2 %) |
| `.text` | 313,196 B | 328 B |
| `.rodata` | 153,088 B | 13 B |
| `.bss` reserved | 74,239,728 B | 24 B |
| dynamic dependencies | libc, libm, libdl, libsqlite3, libcurl, libopenblas | **libc only** |
| C's hello-world, stripped (reference) | 4,368 B | — |

Flint is now within 4 % of C's stripped hello-world [M].

### 6.6 Two traps discovered while fixing it [M]

1. **Weak symbols + `--as-needed`**: `flint_blas.o` references `cblas_dgemm`
   *weakly* (so the runtime can probe for it and fall back to a portable
   loop). `--as-needed` ignores weak undefined symbols, so it dropped
   `libopenblas` even for a program that wanted the fast path — and the
   matmul silently fell back to the portable loop (n=200: **14.4 s** instead
   of ~0.1 s [M]). The library must be added with `--no-as-needed` around it.
2. **Symbol-name detection**: the tensor API is `flt_*` (`flt_matmul`,
   `flt_create_2d`), not `flint_matmul`. Getting that prefix wrong produced
   the same silent 100× slowdown.

Both are the same failure mode as B1 (silently wrong answers): a *performance*
regression that no test noticed because the program still produced correct
results.

### 6.7 What the compiler should be able to answer

`flintc --print-size` should report an attribution like:

```
Binary size: 8,048 B (stripped 4,536 B)
  .text                       328 B   (application + pulled-in runtime)
  .rodata                      13 B
  .bss                         24 B   (was 72 MB before the link fix)
  unwind tables               280 B
  dynamic dependencies: libc only
  modules pulled in: runtime.o only
```

The data needed is already in the link step (section sizes, `DT_NEEDED`,
the object set) — it just is not reported. **Design [D]:** make the link step
emit this attribution as a machine-readable JSON sidecar
(`out.size.json`), so the benchmark dashboard (§21) can diff it per commit.

---

## 2. Comparison with other languages

Not every language is a competitor in every category. Flint is a **systems
language with a safety model, a batteries-included standard library, and a
compiler that is also its toolchain**. That places it against Rust and Zig
directly, against C/C++ on performance and deployment, against Go on
ergonomics and concurrency, and against Python on developer experience.

### 2.1 The reference table (which language to learn from, for what)

| area | primary reference | secondary | what NOT to copy |
|---|---|---|---|
| Systems programming | **Rust** (ownership, no-UB guarantees) | Zig (comptime, no hidden control flow) | C++ (complexity), Go (GC + no data-race freedom) |
| Safety | **Rust** (borrow checker) | Swift (strict concurrency), Zig (safety in safe modes) | Rust's *ergonomics tax* — Flint should keep AEGIS leases out of the type system |
| Performance | **C** (the floor) | Zig (comptime specialization), Rust (LLVM parity) | — |
| Tooling | **Rust** (rust-analyzer) | Go (single tool, gopls), Swift (sourcekit-lsp) | TypeScript's "server = the compiler" only if Flint owns its compiler (it can) |
| Concurrency | **Rust** (Send/Sync, async) | Go (goroutines/channels), Swift (actors) | Go's implicit data races |
| Package management | **Go** (proxy + module graph + checksums) | Cargo (resolver + lockfile), npm (registry UX) | npm's tree, PyPI's filename-based versioning |
| Developer experience | **Go** (one tool, fast compile) | Zig (no build system at all) | Rust's compile times |
| AI/ML | **Python** (ecosystem) | Julia (numerical), Mojo | Python's dynamic typing as the *interface* to the ecosystem |
| Embedded | **Zig** (freestanding, comptime) | C (bare metal) | — |
| Server | **Go** (boring, one binary) | Rust (Tokio), Java (JVM warmup) | — |
| Self-hosting | **Rust** (rustc in Rust) | Go (gc in Go) | — |

### 2.2 Where Flint is ahead of every language in the set

- **One artifact, one tool, and a documented machine interface**: a stable
  token dump (`--dump-tokens`), a 135-message diagnostic catalog with a
  two-way sync gate, and an agent benchmark (`agent-bench/`) [M]. The LSP
  ecosystem data shows that agents benefit from semantic navigation, not grep
  [P: CircleCI, 2026-06 — LSP cut tool-output tokens by 31–34 % on a 149k-line
  TypeScript repo]. No other language in this comparison ships an
  agent-facing contract.
- **Deterministic memory management with a runtime backstop**: leases +
  generational collector + deterministic free [M: `memory.md` AEGIS section].
  Go and Python have a GC; Rust has neither GC nor a backstop; Zig has
  "safety in safe modes" but no ownership model.
- **Compiler-as-toolchain**: compile, JIT-run, IR emission, interface
  emission/reuse, test running, formatting, language server, agent benchmark
  — from one project, one build, one language [M].

### 2.3 Where Flint is behind, by category

- **Performance**: 1.35–2.25× behind C/C++/Rust on integer loops [M].
- **Size**: now at C parity for a hello-world (4,536 B vs 4,368 B) but the
  *runtime* still carries fixed arenas; Rust's static hello is 340 KB
  stripped, so Flint's advantage is over Rust, not over C [M].
- **Concurrency**: no static race freedom; `parallel for` removed [M].
- **Ecosystem**: zero third-party packages; Rust ~150k crates, npm ~3M
  packages, PyPI ~600k [P, order-of-magnitude from registry front pages,
  2026].
- **Type system**: no async/await, no associated types, no operator
  overloading, monomorphization-only generics with no `dyn` escape hatch
  [M: `memory.md` §5 M2, §8].

### 2.4 The languages that are *not* competitors

TypeScript/JavaScript (different deployment and type-safety contracts),
Python (dynamic, interpreted, but owns the AI ecosystem), and C# / Java
(managed runtimes, GC, virtual dispatch) are **reference points for tooling
and ecosystem design only** — copying their object models or GC designs into
Flint would be a mistake.

---

## 3. LSP and developer tooling

### 3.1 Flint's current state [M]

- `lsp/` is a **Rust** implementation, separate from the C++ compiler
  (`lsp/` + `tests/test_lsp.sh`: 5/5 pass).
- The compiler exposes: `--dump-tokens` (stable token stream for tools),
  `--check-bce` (a machine-readable static property of the program),
  `docs/errors-cpp.md` (the diagnostic contract), and `agent-bench/`.
- There is **no** LSP that shares the compiler's semantic model, so
  diagnostics and type information in the editor are a *reimplementation* —
  the same drift risk that produced `COMPATIBILITY.md` D5.

### 3.2 Category scores (0–10) — with evidence

| capability | Flint | Rust (rust-analyzer) | Go (gopls) | C/C++ (clangd) | Python (pyright) | TS (tsserver) |
|---|---|---|---|---|---|---|
| completion | 5 | 9 | 8 | 7 | 8 | 9 |
| diagnostics | 6 | 9 | 8 | 8 | 8 | 9 |
| go-to-definition | 5 | 9 | 8 | 8 | 7 | 9 |
| find references | 3 | 9 | 7 | 7 | 6 | 8 |
| rename | 3 | 9 | 7 | 7 | 6 | 8 |
| symbol navigation | 4 | 9 | 8 | 8 | 7 | 9 |
| hover / type info | 5 | 9 | 8 | 7 | 7 | 9 |
| semantic highlighting | 4 | 9 | 8 | 8 | 6 | 9 |
| code actions | 3 | 8 | 6 | 6 | 5 | 8 |
| refactoring | 2 | 8 | 5 | 5 | 4 | 8 |
| formatting | 8 | 9 | 9 | 8 | 7 | 9 |
| debugger integration | 4 | 8 | 8 | 8 | 6 | 6 |
| build-system integration | 5 | 9 | 9 | 6 | 5 | 8 |
| incremental analysis | 4 | 9 | 9 | 8 | 7 | 8 |
| incremental compilation | 5 | 8 | 9 | 7 | 4 | 7 |
| large-project performance | 4 | 7 | 7 | 7 | 5 | 7 |
| stability | 6 | 9 | 9 | 9 | 7 | 9 |
| cross-platform support | 5 | 9 | 9 | 8 | 8 | 9 |
| editor compatibility (LSP) | 7 | 9 | 9 | 9 | 8 | 9 |
| plugin/tool ecosystem | 2 | 8 | 7 | 8 | 8 | 9 |

**Weighted overall** (weights: correctness features 45 %, maturity 25 %,
ecosystem 30 %; capability ≠ maturity ≠ ecosystem size): **Flint 4.2 / 10**,
rust-analyzer 8.7, gopls 8.1, clangd 7.6, pyright 6.8, tsserver 8.6.

### 3.3 Evidence

- rust-analyzer: 142 ms mean autocomplete at 500k LOC, 87 kLOC/s background
  indexing, 214 MB idle — and **3.2–4.2 GB resident for a single
  workspace** [P: johal.in VS Code 2.0 LSP benchmark, 2026-04; drey
  measurement, 2026]. gopls: 427 ms autocomplete, 128 MB idle, 124 kLOC/s
  [P: same].
- LSP readiness is not instantaneous: a TypeScript server answers the
  *import binding* until the project finishes loading, so clients need a
  warm-up stabilisation loop [P: stitchgraph LSP probe, 2026-07].
- Deno cut completion from 8 s to <1 s by caching the TypeScript model
  instead of re-reading it per keystroke [P: Deno engineering blog].

### 3.4 Major weaknesses of Flint's tooling

1. **No semantic layer in the compiler to serve an LSP** — the server
   cannot ask the compiler "what is the type of this expression" because the
   compiler does not keep one [M].
2. **No rename/refactor** — without a symbol table the server can expose,
   these cannot be implemented at all [M].
3. **No large-project story** — the compiler caches whole modules by source
   hash; there is no incremental, per-file invalidation [M].

### 3.5 Best practices worth adopting [D]

- **Compiler-backed LSP**: serve the LSP from the compiler's own semantic
  model (the same type checker the CLI uses), so diagnostics cannot drift.
  This is the single highest-leverage tooling decision and it is a
  consequence of §13 (a real type-checking phase).
- **Warm-up contract**: the server reports readiness explicitly and clients
  wait for it [P: stitchgraph probe].
- **Incremental by design**: cache per-file type information keyed by content
  hash; invalidate on dependency change, not on repo change.
- **Bound memory**: rust-analyzer's 3–4 GB for one workspace is the failure
  mode to avoid; Flint's compiler is small, and a Flint LSP should be too.

### 3.6 Features to deliberately NOT copy [D]

- **TypeScript's "server = the compiler"** is right *only* if the language
  owns the compiler — Flint does own it, so this is the pattern to follow,
  but not the JS implementation.
- **Implicit project configuration** (tsconfig-driven file inclusion) —
  Flint should have explicit, source-driven project files.
- **Plugins that mutate semantics** (tsserver plugins) — they break
  reproducibility and complicate the "one true compiler" story.

---

## 4. Runtime-performance investigation

### 4.1 The measurement that started this [M]

`sum_array` 10M: Flint 19.1 ms vs C 10.1 ms (1.89×); `primes` 10M: 495 ms
vs 220 ms (2.25×); `fib(45)`: 12.9 s vs 9.6 s (1.35×); `pi` 100M: 779 ms
vs 783 ms (0.99× — parity).

Two observations already narrow the search:

- **The f64 loop is at C parity** [M]. If the gap were caused by
  bounds/overflow checks (integer-only) or by the array representation
  (integer-only), an f64 loop would not be at parity. So the gap is
  **integer-specific**.
- **The gap is not the f64 path's** (which is parity) and not the runtime
  (same runtime in both).

### 4.2 Measured cost of the safety machinery [M]

`--unsafe` (disables overflow checks) on the same binaries:

| workload | safe | `--unsafe` | overhead |
|---|---|---|---|
| sum_array 10M | 18.9 ms | 16.5 ms | 13 % |
| primes 10M | 575 ms | 561 ms | 2.5 % |

So checks are **13 % of the integer-loop gap at most** (1.89× = 89 % gap;
13 % of the *total* runtime is checks). The remaining ~1.6× is elsewhere.

### 4.3 Measured check elimination [M]

`flintc --check-bce` reports emitted vs surviving checks after O2:

| program | checks emitted | survive O2 | eliminated |
|---|---|---|---|
| `sum_array.fl` | 6 | 3 | 50 % |
| `primes.fl` | 10 | 7 | 30 % |
| `pi.fl` | 1 | 0 | 100 % |
| hand-written `while i < 3 { s += a[i] }` | 3 | 0 | 100 % |

The pattern is exact: **checks die when the bound is provable in the same
function (constant trip count), and survive when the length is a separate
loaded value.** `sum_array`'s surviving checks are the `flint_array_get`
bounds checks, whose length comes from the `{ptr, i64}` slice in memory while
the loop bound is a different value.

### 4.4 Candidate contributors, and the experiment that settled them

**The experiment was run** (it was listed as an open item in the first
draft of this document; it is now closed). `flintc --emit-llvm` versus
`clang -O2 -S -emit-llvm` on `benchmarks/sum_array.fl` / `.c`, both
post-optimization:

| measure (optimized IR) | C | Flint |
|---|---|---|
| instructions in `main` | 52 | **49** |
| vector operations in the module | 16 (`<2 x i64>`) | **0** |
| `icmp` in `main` | 2 | **4** |
| per-iteration bounds check | none — C's out-of-bounds is UB, so it is assumed away | 1 (`icmp ult i, len`) |
| per-iteration overflow check | none — C's signed overflow is UB, so it is assumed away | 1 (on `sum + a[i]`) |
| the array length in the IR | not needed | a **runtime value** (`%.fca.1.extract`), unrelated to the loop bound (`i < 10000000`) |

Three conclusions, all measured [M]:

1. **The gap is not code bloat.** Flint's optimized `main` is *smaller*
   than C's (49 vs 52 instructions). The front end is not emitting worse
   instructions; it is producing a different *algorithm*.
2. **C auto-vectorizes the integer reduction; Flint does not** (16 vector
   ops vs 0). This is the largest single factor, and it explains the
   pattern across the whole benchmark set: `pi` (f64) is at parity because
   clang will not reassociate a floating-point reduction without
   `-ffast-math`, so *neither* side vectorizes it; `sum_array` (i64) is
   1.89× behind because *only C* vectorizes it. **The gap appears exactly
   where C's undefined behaviour lets it do more.**
3. **Flint's two checks are why it cannot vectorize.** A check per access
   and a check per addition block the reassociation a reduction needs, and
   the bounds check cannot be eliminated because the array length is a
   runtime value LLVM cannot relate to the loop bound.

So the measured decomposition of the 1.89× on `sum_array` is: checks ~13 %
(measured directly with `--unsafe`) + the vectorization difference = the
rest. It also explains why `fib(45)` — call-bound and not vectorizable in
either language — is only 1.35× behind.

### 4.5 What this changes in the plan

- The "unexplained 1.6×" is now explained: it is **the cost of defined
  overflow semantics plus the loss of auto-vectorization**, not a codegen
  defect [M].
- The highest-value optimizations are therefore **semantics-preserving
  analyses that let LLVM do what C's UB lets it do**:
  1. *range analysis* to remove the overflow check where the compiler can
     prove the range (§5.2);
  2. *same-value bounds* so the bounds check folds away (§5.2);
  3. once both are gone, LLVM's own vectorizer can vectorize the
     reduction — **Flint does not need a vectorizer of its own** [I].
- A C-like "wrapping arithmetic" type is an alternative to (1) and deserves
  consideration: it trades a defined trap for wrap-around semantics and
  would restore vectorizability for the affected code [D].
- The remaining measurements worth doing (not done here): `primes` and
  `fib` IR comparisons, and a `-O3`/LTO comparison to separate LLVM-level
  effects from Flint-level ones [I].


## 5. Safety-preserving performance

### 5.1 The two categories must stay architecturally separate [D]

| category | who decides | effect |
|---|---|---|
| **Safe optimization** | the compiler proves a check is unnecessary and removes it | semantics unchanged, no source marker, fully automatic |
| **Unsafe compilation** | the programmer asks for it (`--unsafe`, and `unsafe` blocks) | semantics explicitly weakened, visible, auditable |

Flint must never let a build profile, a domain declaration, or a heuristic
move code from the first row to the second. §10 and §17 make that a
structural rule.

### 5.2 The specific opportunity: make the bound provable

The measured failure mode is precise [M]: the check survives because the
array length and the loop bound are unrelated values.

Three fixes, in increasing cost:

1. **Cheap, immediate — same-value bounds.** If the loop's upper bound is
   the *same SSA value* as the length used by the access, LLVM eliminates
   the check. This is exactly the "reslicing" technique discussed in the
   Rust community [P: internals.rust-lang.org bounds-checking thread,
   2026-02]. In Flint it means: when a `while i < n` loop indexes an array
   of known length `n`, the compiler should emit the length **once** into a
   value and use that same value for both the loop bound and the access.
2. **Medium — element-size-typed arrays.** Give the array representation an
   element size so `GEP` is typed (`{ptr, i64, i64}` or a typed pointer) and
   the length lives next to it. This is a representation change (the M7
   "Rust array representation" idea) and it is expensive, but it is what
   makes bounds reasoning possible at all [I].
3. **Expensive — a range analysis in Flint's own IR.** With a typed MIR
   (§13), Flint can prove `0 <= i < len` and emit the access unchecked.
   This is the principled version and it needs the IR first.

### 5.3 Cheap wins that need no new IR [D]

- ~~**Inline the array accessors.**~~ **Already the case — do not do it.**
  The optimized IR shows the accessor's fast path inlined (`icmp ult i,
  len` + branch, with only the panic path left as a call to
  `flint_bounds_check`) [M: §4.4]. This proposal was in the first draft of
  this document and **measuring the IR is what proved it wrong**. The
  problem is not a call boundary; it is that the length is a runtime value
  unrelated to the loop bound, and that the two checks block the
  vectorization a reduction needs.
- **Hoist the length load** out of the loop (the runtime already loads it
  per access).
- **Emit `panic=abort`-style traps instead of unwinding for checks.**
  Rust's internals discuss `-C overflow-checks=trap` precisely because it
  removes the unwind tables *and* makes the check cheaper [P: 2026-02
  thread]. Flint's `.eh_frame` is 280 B today (after the link fix) but a
  library-heavy program would carry much more [M].

### 5.4 Whole-program and profile-driven optimization

- **PGO**: Flint can emit a profile from the JIT path and reuse it; the
  infrastructure (JIT + counters) exists [M: ORC JIT, `flint_time_ns`].
- **LTO**: currently unavailable because the runtime is a set of `.o` files
  linked separately [M]. A single-LTO link of the app + runtime would let
  LLVM see through the accessors globally.
- **Compile-time proofs**: the language already has static types and a
  trait system; a `Trusted<N>`-style marker (a type that certifies a
  length) is the natural Flint shape for "this index is in range" [D].

### 5.5 What "safe code as fast as possible" should mean numerically [D]

A concrete, checkable target: **for a safe program, the generated code
should be within 1.2× of the same program compiled with `--unsafe`**, and
within 1.2× of C for index-loop kernels. Today: 1.13× (safe vs unsafe) and
1.89× (safe vs C) [M]. The first number is close; the second is the goal.

---

## 7. Binary-size output profiles [D]

Binary size must not be optimized blindly: a "tiny" profile that disables
safety is a different language, and a "performance" profile that disables
bounds checks is the same mistake in a new coat.

| profile | size | startup | runtime | memory | compile time | enables |
|---|---|---|---|---|---|---|
| **tiny** | ★★★★★ | ★★★★★ | ★★★ | ★★★★★ | ★★★★★ | minimal runtime, no stdlib subsystems, `-Oz`-style codegen, single translation unit |
| **general** (default) | ★★★★ | ★★★★ | ★★★★ | ★★★★ | ★★★★ | current defaults |
| **performance** | ★★ | ★★★ | ★★★★★ | ★★★ | ★★★ | aggressive inlining/unrolling, PGO, `--unsafe` **only** if the user asks |
| **server** | ★★★ | ★★★★ | ★★★★ | ★★★★★ | ★★★★ | no JIT/agent tooling linked, prebuilt modules, long-lived allocations |
| **ai/ml** | ★ | ★★★ | ★★★★★ | ★★ | ★★ | tensor kernels, BLAS, large arenas (72 MB BSS today [M]) |

Design rules:

- A profile is a **set of flags and link decisions**, never a different
  compiler.
- **No profile may enable `--unsafe`** (§10, §17).
- `tiny` must not change *semantics* — only what is linked and how code is
  laid out. With the link fix, a hello-world is already 4,536 B [M], so
  `tiny` is mostly about not linking subsystems and about `.eh_frame`
  (280 B today) [M].
- `ai/ml` legitimately wants a bigger binary: the current AI stack already
  reserves 72 MB of `.bss` (arena 48 MB, compress 16 MB, sqlite 4 MB,
  crypto 1 MB) [M]. That is a *memory* profile decision, and it should be
  visible in the profile, not accidental.

---

## 8. Static memory safety

### 8.1 What Flint has today [M]

- Compile-time use-after-free detection (borrow checker over declarations
  and assignments) [M: `memory.md` P0 soundness fixes].
- AEGIS: static leases + a generational runtime backstop + deterministic
  free [M].
- Bounds checks, overflow checks, div-by-zero traps, null checks [M].
- Ownership transfer at assignment (`moved` flag) [M].
- A safety audit that found and closed real holes — and that still has open
  findings (e.g. the N1 hang, the D5 feature gap) [M].

### 8.2 Do not simply add Rust's borrow checker [I]

Flint's design already differs in a way that matters: **ownership is
expressed by leases, not by the type system.** Adding a second, type-level
borrow checker on top would create two competing ownership systems, and
users would have to learn both. The design question is therefore not "can we
add a borrow checker" but "can we make the *existing* lease model precise
enough to be provable".

### 8.3 Candidate mechanisms, evaluated

| mechanism | safety guarantee | compiler complexity | runtime cost | compile-time cost | ergonomics | optimization benefit | migration | fit with Flint |
|---|---|---|---|---|---|---|---|---|
| **Region/lexical lifetimes (borrowck-lite)** | no use-after-free within a region; no cross-region escape | Medium | none | Medium | good | enables stack promotion, escape analysis | moderate | **best fit** — extends leases |
| **Escape analysis + stack promotion** | same as today, but with better allocation behaviour | Medium | none (can *remove* allocation) | Medium | invisible | **high** — removes heap traffic, enables BCE | low | **best fit** (§12) |
| **Affine/linear types** | resources used exactly once; prevents leaks and double-free | High | none | High | **poor** (users must thread state) | low | high | poor fit — would fight the ergonomic goal |
| **Region-based memory management (RMM)** | whole regions freed at once; predictable | Very high | low | Very high | medium | medium | very high | too early; conflicts with the deterministic-free goal |
| **Null-safety types (no null / optional)** | removes null dereference | Medium | none | Low | good | enables better codegen | low | **good fit** — additive, orthogonal |
| **Immutable/mutable separation** | aliasing control at the reference level | Medium | none | Low | medium | high (enables no-alias assumptions) | moderate | good fit — extend leases with constness |
| **Safe FFI boundary types** | ownership/lifetime across FFI is explicit | Medium | none | Low | good | neutral | low | **good fit** — the FFI story needs it anyway (§25) |

### 8.4 Recommended model [D]

**Leases + regions + escape analysis, not a second borrow checker.**

1. **Keep leases** as the user-visible ownership concept (they are already
   ergonomic and already detect use-after-free at compile time).
2. **Add lexical regions**: every function body and every block is a
   region; a value may not be used after the region that created it ends.
   This is the missing half of today's rule — today the rule is "a closure
   may not escape its defining frame" enforced only for closures.
3. **Add escape analysis** to the compiler: a value whose uses are all
   inside the region that created it is *stack-allocated and promotable*;
   a value that escapes is heap-allocated with a precise lifetime. This
   replaces the current blanket "closures cannot escape" with a precise
   rule, and it makes the common case free.
4. **Add const/alias qualifiers to leases** so the optimizer can assume
   no aliasing between a `&T` and a `&mut T` — the precondition for
   better vectorization.
5. **Add safe FFI wrapper types** (owned/borrowed/unsafe handles) so the
   C boundary is explicit.

Cost estimate [I]: regions + escape analysis + qualifiers ≈ one
compiler-architecture milestone (the same milestone as the typed IR), not
five. Expected payoff: escaping closures become possible, closures stop
being a special case, and the optimizer gains the alias facts it needs.

---

## 9. Concurrency safety

### 9.1 Where Flint is [M]

Threads (`flint_thread_*`), channels (`flint_chan`), and the AEGIS
ownership model — but **no static race freedom**: there is no `Send`/`Sync`
equivalent, and `parallel for` was *removed* rather than fixed (the removal
was correct: the original could capture variables by value across pthread
workers, which is a use-after-free).

### 9.2 What to build, in order [D]

1. **Capability types (`Send`-like / `Sync`-like) as a marker on leases.**
   A lease that owns a resource is `Send` if the resource may move to
   another thread; a shared reference is `Sync` if the resource tolerates
   concurrent reads. Implementation: a marker trait on the *type* plus a
   check at the point a value crosses a thread boundary. This is a
   type-system feature, not a runtime feature, and it does not require
   async/await.
2. **Channel typing.** Channels already exist; make them carry the element
   type, and make a channel *the* safe way to share mutable state. This
   gives users a race-free path that does not require the borrow checker to
   be airtight for shared state.
3. **Safe parallel loops** — `parallel for` returns, but as a *reduction*
   with an explicit result channel, no captures by value, and a
   `Send`-checked closure. The memory.md B6 analysis already specifies this
   shape: "ship order-deterministic capture-free builtins in the Rust
   runtime, not syntax sugar" [M].
4. **Atomics and synchronization primitives** in the stdlib, with the
   same rules as C11 but typed.
5. **async/await only after 1–4** — async brings its own ownership
   requirements (futures must be `Send` to move between executors), so it
   depends on the capability model.

The desired property — "high-performance concurrency with strong
compile-time safety and minimal runtime overhead" — is reachable, but
**the capability model is the prerequisite**, and it is a type-system
feature, so it belongs in the same milestone as the typed IR (§11, §13).

---

## 10. `--unsafe` design

### 10.1 Requirements (as specified, restated as invariants)

- `--unsafe` is a **manual, terminal-only** escape hatch [spec].
- It must **never** be enabled automatically: not by a domain declaration
  (§15), not by an optimization profile (§7), not by AI/ML compilation, not
  by a performance heuristic, not by a package manifest, not by an editor or
  LSP setting.
- The compiler must never silently downgrade safety: if a build cannot be
  made safe, it must fail with a diagnostic, not fall back.
- Normal compilation stays safe by default.

### 10.2 What `--unsafe` should permit [D]

Exactly two things, both already implemented today:

1. **Overflow checks off** (measured: 13 % faster on integer loops [M]).
2. **Bounds checks off** for array indexing — *not implemented today*; it is
   the natural companion and must be added deliberately, with its own
   measurement.

It must **not** permit: raw pointer arithmetic, arbitrary transmutes,
skipping initialization, disabling the AEGIS backstop, or disabling the
diagnostic checks that are not performance-related.

### 10.3 Source-level `unsafe` blocks [D]

**Yes — `--unsafe` and source-level blocks should be different things.**

- `--unsafe` is a *whole-program* speed switch: "I accept the risk
  everywhere". It is a build flag, auditable in the build log, and it
  should print a warning and stamp the binary (e.g. a note section) so an
  unsafe artifact is identifiable later.
- `unsafe { ... }` in the source is a *local* opt-out: "I accept the risk
  here". It requires a justification string in strict builds
  (e.g. `unsafe("index proven in range by the caller")`), it is greppable,
  and it is what a library author uses for the one hot loop that cannot be
  proven safe.

Why both: a global flag alone is too blunt for library code and too easy
to leave on in a release; blocks alone are too verbose for a whole
performance-critical program. The invariant that matters: **neither
mechanism may be activated by a profile, a domain, or a tool** — only by a
human at the command line or in the source.

### 10.4 Auditing [D]

- `flintc --audit-safety` should list every `unsafe` block and every
  `--unsafe` build, with file/line, and the reason string.
- The build should record the safety posture in the binary (a
  `.flint.safety` note section) so a consumer can tell whether the artifact
  was built with checks off.
- The CI gate should fail a release build that uses `--unsafe` unless the
  release explicitly opts in.

---

## 11. Type-system roadmap

Staged, ranked by value rather than by feature count. "ABI impact" is
assessed against Flint's current conventions (`{ptr, i64}` slices, named
structs, monomorphized generics).

| # | feature | usefulness | complexity | compile-time | runtime | safety | ABI | optimizer | language complexity | migration | rank |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | **Escape analysis + stack promotion** (§12) | high | medium | low | **positive** (fewer allocations) | improves (precise lifetimes) | none | high (SROA, BCE) | low (invisible) | low | **1** |
| 2 | **Associated types / generic traits** | high | high | medium | none | positive | low | medium | medium | medium | 2 |
| 3 | **`dyn`-style trait objects** | high | high | medium | small (vtable) | neutral | **medium** (representation) | medium | medium | medium | 2 |
| 4 | **Async/await** | high | very high | medium | medium | **high** (Send requirements) | high | medium | high | high | 3 |
| 5 | **Operator overloading** | medium | medium | low | none | neutral | low | low | medium | low | 3 |
| 6 | **Closures that escape** (needs 1) | high | medium | low | none | improves | none | medium | low | low | 4 |
| 7 | **Nullable types (`T?`)** | high | medium | low | none | **high** | low | low | low | low | 4 |
| 8 | **Const generics** | medium | high | medium | none | none | low | medium | medium | medium | 5 |
| 9 | **Type-level programming** | medium | very high | high | none | none | medium | low | **very high** | high | 6 |
| 10 | **Pattern matching with exhaustiveness + bindings** (partly exists) | high | medium | low | none | positive | none | medium | low | low | 1 (finish it) |

### 11.1 Notes on the top items

- **Associated types** and **`dyn` objects** together unlock the "trait
  ecosystem" shape that Rust users expect, and they are the two features
  most likely to decide whether third-party packages appear at all [I].
- **Async/await is deliberately last among the big ones**: it is the one
  feature whose safety story depends on another feature (capability types,
  §9). Shipping async first would mean shipping an unsafe async.
- **Nullable types** are cheap and high value: they remove a whole class of
  null-dereference bugs, and they interact well with the match expression
  Flint already has.

### 11.2 The D5 constraint

Whatever is added must land in **both** frontends, or the self-host pipeline
must be declared a bootstrap-only artifact (§26). Traits, generics and
`::<` already exist only in the C++ frontend [M: `COMPATIBILITY.md` D5] —
that is the precedent to break, not to extend.

---

## 12. Closure and lifetime model

### 12.1 Why closures cannot escape today [M]

The M4 design makes a closure's value an **env box** `{fnptr, caps...}`
allocated in the *defining frame*; the call protocol is
`fn(env, userargs...)`. The box therefore points into a frame, and the
compiler refuses to return or store it (a "would escape its defining frame"
diagnostic) [M]. The restriction is not a missing feature — it is a
consequence of the representation.

### 12.2 Approaches compared [D]

| approach | how | pros | cons | fit |
|---|---|---|---|---|
| **Heap-allocate the env box** | box on the heap, owned by the closure value | trivial to implement, closures escape | every closure allocates; contradicts the "no hidden allocation" goal | partial (escape-only path) |
| **Reference counting** | `Rc`/`Arc` for the box | escapes naturally | cycles leak; `Rc` is not `Send`; runtime cost on every clone | no |
| **Generational arena** | box in a per-thread arena, freed with the arena | cheap, deterministic, matches Flint's Rust runtime arena (48 MB [M]) | arena lifetime is the closure lifetime → still frame-bound | **good** |
| **Escape analysis + promotion** | analyse uses; if all uses are inside the creating region → stack; else arena/heap | zero cost in the common case; precise | needs the compiler to model regions and lifetimes | **best** |
| **Closure conversion (current)** | env box in the frame | simple, no allocation | cannot escape | baseline |
| **Lifetime inference** | infer region constraints, reject only real escapes | precise errors | needs a real type/region checker | combined with the above |

### 12.3 Recommended design [D]

1. **Keep closure conversion** (it is the right lowering).
2. **Add escape analysis**: a closure whose captured values are not used
   after the defining function returns → the box can live in a
   **per-thread arena** (already exists in the Rust runtime) instead of the
   frame, and the escape restriction lifts for it.
3. **Add region inference** for captures: a capture used only inside the
   defining function stays in the frame; a capture that outlives it is
   moved to the arena/heap automatically, with a *copy or move* decision the
   compiler makes (and can report: "capture `x` is moved into the closure
   because it escapes").
4. **Result:** escaping closures become possible for the common cases
   (callbacks, stored handlers) with **no allocation in the common case**,
   and the compiler reports the decision instead of refusing.

Cost: one escape-analysis pass + region tracking in the typed IR (§13).

---

## 13. Compiler architecture

### 13.1 The current shape [M]

```
source ──► C++ Parser (AST)  ──► C++ Codegen ──► LLVM IR ──► LLVM ──► .o ──► clang link
                    ▲
                    │  (second, independent implementation)
                    │
source ──► stage1 lexer (Flint) ──► stage2 parser (Flint) ──► stage3 emitter (Flint) ──► .ll
```

The C++ frontend is authoritative for shipping; the Flint pipeline is the
self-hosting ladder. They have already drifted: the pipeline has no traits,
no generics, no `::<`, no enum equality [M: `COMPATIBILITY.md` D5].

### 13.2 Which implementation becomes authoritative [D]

**The C++ implementation stays authoritative**, for three reasons [I]:

1. It is the one with the full feature set (M3–M6, P0-NEW work, the M2
   residue work) — the Flint pipeline is far behind.
2. It already owns the LLVM backend, the JIT, the interface system, the
   module cache and the linker integration.
3. A "rewrite the compiler in Flint" project would take years and would
   violate the migration rule (§27).

The Flint pipeline keeps a *specific, bounded* role: **the bootstrap
ladder** — a self-hosted front end used to prove the language can express
a compiler, and to keep the ladder honest. It does not need to be
feature-complete; it needs to be *correct where it runs* and *clearly
scoped*.

### 13.3 Target architecture [D]

```
source
  ↓
Lexer ──► Parser ──► AST (surface syntax, no types)
  ↓
Name resolution ──► HIR (typed, resolved, monomorphization decided here)
  ↓
Trait/generic resolution ──► MIR (SSA, explicit slots, no allocas in the
  │                              default representation)
  ↓
Flint optimizer passes (range/alias/escape, inlining, BCE, overflow-check
  │ elimination, layout)
  ↓
LLVM IR ──► LLVM ──► .o ──► link (with the §6 hygiene)
  ↓
executable
```

**The stable IR boundary is HIR/MIR, not LLVM IR.** That is the single
architectural decision that unlocks the most: Flint's own safety proofs
(range, alias, escape, ownership) must be expressible somewhere, and LLVM
IR is too low-level to talk about leases and regions.

### 13.4 What must change, concretely

| change | why | cost | risk |
|---|---|---|---|
| **Type checking must produce a typed IR, not LLVM IR directly** | §5 (check elimination), §8 (regions), §9 (capabilities), §14 (better diagnostics) | high | high — this is the big one |
| **Generic instantiation decided in the type phase, not in codegen** | the current design decides it while emitting, which is why the LSP cannot answer type questions | medium | medium |
| **Trait resolution in one place** (today: `traitRegistry` + name/arity dispatch in codegen) | needed for `dyn` objects and associated types | medium | medium |
| **The Flint pipeline becomes a *consumer* of the same HIR spec** | stops the two frontends from diverging semantically | medium | medium |
| **One diagnostic emitter** shared by CLI, LSP and pipeline | diagnostics are a contract (§3) | low | low |

### 13.5 How to migrate without a rewrite [D]

Phase A: introduce HIR *alongside* the current AST→LLVM path, and have the
type checker emit HIR for a small feature set (functions, arithmetic).
Phase B: move codegen to consume HIR for that subset.
Phase C: grow the subset until it is the whole language.
Phase D: the Flint pipeline targets the same HIR (it can re-use the
emitter) — the pipeline's *parser* stays in Flint; its *semantics* come
from the shared HIR spec.

Nothing in Phase A changes user-visible behaviour, so the language stays
usable throughout (§27).

---

## 14. Compiler correctness and testing

### 14.1 What exists today [M]

- 21 test suites, all green at HEAD: smoke (48), lexdiff (131), parse_gate
  (132), fixpoint (132), errors (128/128), errors-cpp (135/135),
  differential (11/11), generics (7), no-hang (88), AI (26), rust-stdlib
  (20), rust-ffi (8), rust-oracle (2), https, python, lsp, fmt,
  opt-identity, cache, fuzz-corpus, aegis, driver, tutorial, registry.
- A **differential gate**: the C++ compiler vs the self-hosted Flint
  pipeline on 11 fixtures, failing on any unlisted divergence [M].
- A **fixed-point gate**: the self-hosted pipeline re-compiles itself
  byte-identically over 132 cases [M].
- A **no-hang gate**: every corpus program must terminate [M: 88 cases].
- An **errors catalog** with a two-way sync gate [M].

This is a stronger-than-average foundation for a 0.x language.

### 14.2 What is missing [D]

| gap | why it matters |
|---|---|
| **No miscompilation oracle beyond the differential gate's 11 fixtures** | the differential gate compares two implementations of *Flint*, not against a reference semantics; a shared misreading passes both |
| **No fuzzing of the compiler** | `fuzz/` has a generator but is not wired into any CI runner [M] |
| **No property-based testing** | e.g. "the optimizer preserves program semantics" is unasserted |
| **No IR-level tests** | optimizer bugs are only visible end-to-end |
| **No binary-size regression gate** | a 743 KB hello-world shipped for a long time; nothing would have caught it [M] |
| **No performance regression gate** | the vectorization gap (§4.4) could have grown unnoticed [M] |
| **No ABI tests** | the interface format (`.flint.bc`) has no versioned compatibility test |

### 14.3 The validation strategy to build [D]

**Layer 1 — unit tests** for each phase (lexer, parser, type checker,
traits, each optimizer pass, the linker decisions). Each pass gets a
"golden IR" test: input → expected HIR/MIR.

**Layer 2 — property-based tests** (the highest-leverage addition):

- *Semantics preservation*: for a random program, the optimized and
  unoptimized builds must produce identical output. This is the direct
  test for optimizer bugs, and it is cheap once the random program
  generator exists (the repo already has `fuzz/generate.py` [M]).
- *Check equivalence*: the safe build and the `--unsafe` build must produce
  identical output for programs that do not trip a check — this makes the
  safety-performance split auditable [M: the two modes already exist].
- *No UB*: run the whole corpus under ASan/UBSan (a sanitizers gate exists
  [M: `tests/test_sanitizers.sh`]).

**Layer 3 — differential**: extend the existing gate from 11 fixtures to
the whole corpus, and *both* directions (C++ vs pipeline, O0 vs O2, safe
vs unsafe).

**Layer 4 — compile-fail tests**: the errors catalog is the skeleton; what
is missing is a *negative* corpus asserting that invalid programs are
rejected (not merely that messages match).

**Layer 5 — ABI/versioning**: a golden `.flint.bc` interface plus a
compatibility matrix (which compiler versions can read which interface
version).

### 14.4 Mechanisms for catching specific bug classes [D]

| bug class | mechanism | measurable signal |
|---|---|---|
| miscompilation | differential + property tests (optimized vs unoptimized) | "semantics preserved: N/N" |
| optimizer bug | golden-IR tests per pass + property tests | per-pass IR diff |
| safety bug | sanitizer gate + safe/unsafe equivalence property | "ASan clean: N/N" |
| wrong diagnostic | two-way error catalog + compile-fail corpus | "catalog in sync: N/N" |
| ABI regression | golden interface files + version matrix | "interfaces compatible: N/N" |
| **binary-size regression** | **size gate per commit** (§21) | **"size: Δ vs baseline"** |
| **performance regression** | **bench gate per commit** (§21) | **"runtime: Δ vs baseline"** |

The last two are the ones this project has been missing, and the 743 KB
hello-world proves the cost of that gap.

---

## 15. Source-level domain declarations

### 15.1 The idea

A program declares its own workload profile near the top:

```
@domain(ai)

fn main() { ... }
```

`flint build program.fl` reads it and applies the matching policy.

### 15.2 Syntax evaluation [D]

| candidate | pros | cons | verdict |
|---|---|---|---|
| `@domain(ai)` | short, obvious, greppable, scoped to the file | `@` is not used elsewhere in Flint (no conflict) | **good** |
| `domain "ai"` as a statement | no new sigil | looks like a runtime call; ambiguous with a string expression | acceptable but weaker |
| `#[domain(ai)]` attribute | Rust-like | attributes imply per-item, and the domain is per-*program* | wrong granularity |
| `// @domain(ai)` comment | zero parser work | a comment cannot be relied on by a tool (editors strip them) | **no** — it must be semantic |
| a manifest file (`flint.toml`) | conventional | not "in the source"; the user explicitly wants it there | useful *in addition*, not instead |

**Recommendation [D]:** `@domain(name)` as a **file-level declaration**,
parsed by the lexer/parser as a distinct token (`@` + identifier), allowed
only at the top of a file before any item, and **meaningless in a module
that is imported** (the importing program's domain wins). Keep
`flint.toml` for *project*-level policy (which domain the project defaults
to), with the source declaration overriding it.

### 15.3 What it must NOT do [D]

- It must not change **semantics**. A domain is an optimization policy, not
  a language mode.
- It must not enable `--unsafe` (§17).
- It must not silently change the *result* of a program (no fast-math
  reassociation, no integer overflow wrap-around).
- It must not be required for correctness: a program with a domain and the
  same program without must be *observationally identical* (modulo
  performance and diagnostic noise). This is testable and should be a gate.

### 15.4 Precedence with other layers [D]

```
source @domain(ai)        ← program-level intent (this proposal)
  > project flint.toml    ← project-level default
    > target/config      ← cross-compile target constraints
      > compiler default  ← general
```

The source wins, because the user asked for it; the project file is the
fallback; the target can *veto* (e.g. "ai domain" on a target without
vector units must degrade, not fail).

---

## 16. Domain system architecture

### 16.1 What a domain may influence [D]

| area | examples |
|---|---|
| optimization passes | enable vectorization passes, more aggressive inlining, PGO |
| code generation | target-feature selection, instruction selection hints |
| specialization | monomorphize numeric kernels for `f64` vs `i64` paths |
| memory layout | array-of-structs vs struct-of-arrays for tensor code |
| stdlib implementation | choose the BLAS kernel, the SIMD string search, the vectorized map |
| runtime configuration | arena sizes, thread pool, allocator choice |
| linker configuration | link only the needed subsystems (§6) |
| architecture-specific opts | NEON on ARM, AVX-512 on x86 |
| scheduling | thread-pool sizing for parallel kernels |
| library selection | prefer the specialized implementation of a module |

### 16.2 Where domains live [D]

| option | verdict |
|---|---|
| built into the compiler | **no** — every new domain becomes a compiler release |
| language-defined (a domain library in Flint) | **no** — too slow to consult per compilation, and it would be a chicken-and-egg problem (the compiler would depend on a package) |
| **extensible policy table in the compiler, data-driven** | **yes** — a declarative table (domain → flags, passes, link policy, stdlib preferences) that the compiler reads; adding a domain is a table entry, not a code change |
| library-defined (a library declares "I am optimized for ai") | **complementary** — a *library* may expose specialization hints, but the *domain* is the program's declaration |
| project-defined | **yes**, as the default when the source does not declare one |

### 16.3 The concrete design [D]

```toml
# tools/domains.toml (compiler data, not code)
[domains.ai]
opt_level = 3
passes = ["vectorize", "inline-hot", "layout-aos"]
prefer_stdlib = ["flint_nn", "flint_blas"]
link = "with-blas"
memory = "large-arena"
```

The compiler loads this table, maps the source's `@domain(ai)` to a
**policy object**, and the policy is applied by the existing pass pipeline.
Adding a domain = adding a table entry. The compiler does **not** grow
per-domain code paths; at most, a domain may select among a *fixed set* of
passes and stdlib implementations that already exist.

That is the rule that keeps domains from becoming "dozens of independent
compilers" (the user's constraint): **a domain selects among existing
capabilities; it never introduces new ones.**

---

## 17. Domain declarations and safety

**The invariant: a domain can never weaken a guarantee.** [D]

| allowed under `@domain(ai)` | forbidden |
|---|---|
| aggressive vectorization | `--unsafe` |
| specialized numeric kernels | disabling overflow checks |
| optimized tensor operations | disabling bounds checks |
| architecture-specific instructions | disabling the AEGIS backstop |
| specialized memory layouts | relaxing the escape/ownership rules |
| aggressive but *valid* inlining | changing arithmetic semantics (fast-math reassociation) |
| choosing a faster stdlib implementation | linking a different, less-safe implementation |

**Enforcement [D]:** the domain policy is a typed structure with no field
that can disable safety. The compiler asserts, at policy-construction time,
that `safety_posture == Safe` for every domain; `--unsafe` is a separate
command-line switch that is orthogonal to the policy and is recorded in the
build metadata. A unit test asserts that no domain entry in the table
mentions the unsafe flags, and a CI check greps the table for them.

**A testable statement [D]:** for every domain and every program, the
program's output with and without the domain declaration must be identical
(bit-for-bit for integers, and exactly equal for floats unless the domain
explicitly opts into a documented fast-math mode — which would then be
reported). This is a single test that mechanically enforces the separation.

---

## 18. Domain declarations and reproducibility

### 18.1 The tension [I]

A build's behavior must be predictable, but a source-level declaration
means the *source* now influences the *build*. That is fine if the
influence is deterministic and recorded — and it is what the user asked
for.

| concern | how a source-level domain interacts |
|---|---|
| **Reproducible builds** | the domain is part of the source → part of the build inputs → hashed into the build identity. The same source always produces the same binary. ✔ |
| **Cross-compilation** | the domain may *request* an architecture-specific optimization, but the *target* decides what is legal. A domain never overrides the target. |
| **Target architecture** | target config can veto a domain (e.g. `ai` on a target with no vector units → degrade to the general path) |
| **Package dependencies** | a package must not impose a domain on the importer. Domains are per-program; a package may *declare its own* domain for how it is compiled, which is a property of the package, not of the program. |
| **Build caching** | the cache key must include the domain (it changes codegen). The compiler already keys the module cache by a source hash plus flags — the domain must join that key [M: the cache salt mechanism exists] |
| **Incremental compilation** | a domain change invalidates the whole program (it changes optimization policy), not one file |
| **LSP / IDEs** | the LSP reads the domain to know which APIs to offer and which hints to show; it must NOT change diagnostics (a diagnostic must not depend on a domain) |
| **CI/CD** | the domain is visible in the source diff — a reviewer sees "this program now claims to be AI-optimized" |
| **Project configuration** | `flint.toml` may set the *default* domain; the source overrides it |

### 18.2 Which layer owns what [D]

| layer | owns | example |
|---|---|---|
| **source** | the program's declared intent | `@domain(ai)` |
| **project config** | the default for files that do not declare one | `flint.toml: default_domain = "server"` |
| **target config** | what the target supports (and vetoes) | "no NEON on this target → use the portable kernel" |
| **compiler config** | safety posture, optimization budgets, diagnostics verbosity | `--unsafe`, `-O0..3` |

**Ambiguity rule [D]:** the source wins over the project file; the target
can veto but not override; the compiler flags are orthogonal. When two
sources are compiled into one program, the *program's* domain wins, and
each source is compiled under the program domain — one program, one
optimization policy, no per-file divergence.

---

## 19. LSP integration with domains

**The LSP must read the domain, and must not duplicate it.** [D]

| LSP feature | domain-aware behaviour |
|---|---|
| completion | offer the domain's preferred APIs first (e.g. `flint_nn` functions in `ai`), and domain-specific idioms |
| diagnostics | **unchanged** — a domain must never change whether a program is correct |
| hover / documentation | show the domain's notes ("this kernel is specialized for f64") |
| code actions | offer "convert to domain-optimized form" where a profile exists (e.g. "use the BLAS-backed matmul") |
| optimization hints | surface the domain's expectations (e.g. "this loop will be vectorized under @domain(ai)") |
| build integration | pass the domain to the build; the LSP does not choose flags independently |
| refactoring | domain-neutral |

**Do not require the domain to be configured in the editor** — the user
declared it in the source, so the server reads it from the file it is
serving. The one exception: the LSP must report the domain it sees
(`@domain(ai)` in the status line), because a user with two files of
different domains in one project should see which is which.

The hard rule, again: **the domain may change suggestions and hints, never
diagnostics.** A program that compiles must compile identically with and
without the declaration (testable, §17).

---

## 20. Package ecosystem

### 20.1 The state [M]

No third-party package ecosystem exists. What exists: a **registry of
prebuilt binaries** (a git-based fetch with an offline mode) [M: `src/main.cpp`
registry code], a rich standard library, and a FFI story (C ABI both
directions, 8/8 tests) [M: `tests/test_rust_ffi.sh`].

### 20.2 What belongs in the standard library vs in packages [D]

**Standard library (keep):** the things every program needs and that must
be *uniform* — strings, collections, IO, time, math, error handling, the
runtime (arena, channels, threads), and the *stable C ABI*.

**Packages (move out as they stabilize):** application-domain libraries
(HTTP clients, databases, crypto algorithms beyond the core, image codecs,
AI/ML model zoo, parsers for specific formats).

The current stdlib is *too* broad in one direction (it carries sqlite,
brotli, argon2, png, fts5 — fine) and *too narrow* in another (there is
no ecosystem around it). The fix is not to shrink the stdlib; it is to give
packages a home.

### 20.3 Package manager design [D]

Model it on **Go modules + Cargo**, not on npm:

| requirement | design |
|---|---|
| **package manager** | `flint` subcommands (`flint add`, `flint build`, `flint publish`), in the same binary |
| **registry** | a static index of versions + hashes; self-hostable (a registry is just a directory of manifests) |
| **dependency resolution** | minimal version selection with a published, deterministic algorithm; no "latest wins" |
| **semantic versioning** | required, enforced by the publisher, with a documented "0.x is unstable" rule |
| **lockfiles** | `flint.lock` committed; exact versions + hashes (like `go.sum`) |
| **reproducible builds** | the lockfile + content hashes make the dependency graph and the artifacts reproducible |
| **package caching** | content-addressed cache under `~/.cache/flint` (the compiler already caches modules there [M]) |
| **binary caching** | the same cache, extended to package artifacts |
| **native dependencies** | the FFI model (§25) with explicit "system library required" declarations, checked at build time |
| **FFI packages** | a package may ship a C/Rust shim; the build links it explicitly and records it in the lockfile |
| **package signing** | sign the manifest + artifact hash with a public key; the client verifies before use |
| **vulnerability auditing** | the registry can publish advisories; `flint audit` checks the lockfile against them |
| **publishing** | `flint publish` with a provenance statement (source hash, builder version) |
| **discovery** | `flint search`, plus a `flint.toml` that can pin a registry for a project |

### 20.4 Why this is the long pole for adoption [I]

A language's ecosystem follows its *users*, and users follow the
ecosystem. With a strong stdlib and no packages, Flint is usable but not
*composable*. The package manager is therefore the highest-leverage
non-compiler work in the whole roadmap — and it is also the piece that can
be built independently of the compiler architecture work (§13), because a
package can be a single `.fl` file plus a manifest at first.

---

## 21. Performance and binary-size regression dashboard

### 21.1 What must be tracked [D]

**Runtime** (per workload, ms): integer loops, memory/array workloads,
recursion, call overhead, allocation, strings, collections, channels and
threads, networking, f64 numerics, and one *realistic application* (not a
microbenchmark).

**Compiler**: cold compile time, warm/incremental compile time, peak RSS,
optimization time, emitted IR size.

**Binary**: stripped size (total + per section), `.bss` reserved, startup
time, dynamic dependencies, module count.

**Safety**: checks emitted, checks eliminated, checks surviving, static
proofs made, safety diagnostics emitted.

### 21.2 The tooling that exists and the tooling that is missing [M]

| piece | status |
|---|---|
| workload benchmarks with C/C++/Rust/Python mirrors | **exists** (new: `tools/bench_compare.sh`, `benchmarks/*.rs`) |
| size / build / startup harness | **exists** (new: `tools/bench_tooling.sh`) |
| in-compiler check counter | **exists** (`--check-bce`) |
| per-commit history (dashboard over time) | **missing** |
| CI integration of the benchmarks | **missing** |
| size attribution per commit | **missing** (the data is in the link step, §6.7) |
| regression gate (fail the build on a delta) | **missing** |

### 21.3 The gate format [D]

Every compiler change reports:

```
runtime:       -3.2 %   (sum_array)
binary:        -41 KB   (hello-world, stripped)
compile time:  +1.8 %   (cold, hello)
memory:        -5 %     (compiler peak RSS)
safety:        unchanged (6 checks emitted, 3 elided)
```

and the gate fails when a threshold is crossed (e.g. runtime > +3 %, binary
> +5 %, compile time > +10 %, safety checks increased).

### 21.4 Methodology requirements [D]

- Fixed hardware, fixed toolchain versions, warm page cache, N repetitions
  with the **median** (not the mean) reported, and the run-to-run spread
  published alongside (the machin benchmark suite found a 41 % spread on one
  laptop and 15 % on another, and its *verdicts* reproduced while the exact
  ratios did not [P: machin docs/BENCHMARKS.md, re-measured 2026-08-07]).
- Binary size is deterministic — it is the easiest and most reliable
  regression signal, and it is the one this project has been missing.
- The benchmark corpus must be committed and the harness must be
  reproducible by a third party on another machine.

---

## 22. Where the regressions occurred (archaeology)

### 22.1 The method [D]

```
regression observed
  ↓
bisect the history for the first commit that shows it
  ↓
diff the generated IR / the link line / the runtime objects at that commit
  ↓
name the change responsible
  ↓
fix or document
```

The bisect target must be *reproducible*: the benchmark harness
(`tools/bench_compare.sh`, `tools/bench_tooling.sh`) is the oracle, and
because it is a script, `git bisect run` can drive it automatically:

```bash
git bisect start <good> <bad>
git bisect run bash tools/bench_size_gate.sh   # prints "ok" or "bad"
```

### 22.2 The regression found in this assessment [M]

| step | finding |
|---|---|
| symptom | hello-world = 743,520 B (565,600 B stripped), 130× C; 72 MB of `.bss` reserved in every process; three native libraries as hard dependencies |
| bisect | **not needed** — the attribution found it in one pass: the link line had no `-Wl,--gc-sections` |
| responsible change | the link line in `src/main.cpp:spawnLinker` never gained the flag (the runtime objects *are* compiled with `-ffunction-sections`, so the capability existed) |
| root cause | a design that was documented in a comment but never implemented; the comment claimed "743 KB instead of 8 KB" — the number the shipped compiler actually produced |
| corrective action | `--gc-sections` + `--as-needed` + conditional native libraries (commit `46a007e`) |
| result | 8,048 B raw / 4,536 B stripped; `.bss` 24 B; `libc` only [M] |

**Two sub-findings that make this a class, not an incident [M]:**

1. **The compiler caches the linked binary by source hash** — so a stale
   artifact survives a source fix, and a size measurement can be wrong even
   when the source is right.
2. **Weak symbols + `--as-needed`** silently demoted the BLAS matmul to a
   portable loop (n=200: 14.4 s vs 2.9 s) — a 5× performance regression
   that no test caught, because the results were still correct.

The lesson: **every performance and size property needs a gate**, because
both regressions were invisible to a passing test suite (§14.4).

### 22.3 The performance gap: archaeology not yet done [I]

The 1.6× part of the integer-loop gap **has** been bisected, by the IR
comparison in §4.4: the cause is that C auto-vectorizes the integer
reduction (16 vector ops) while Flint does not (0), because Flint's
defined-trap overflow semantics and its bounds check block the
reassociation a vectorized reduction needs — and because the array length
is a runtime value unrelated to the loop bound. **The fix is analysis, not
a new backend**: range analysis + same-value bounds (§4.5, §5.2), after
which LLVM's own vectorizer can do the work [M].

The binary-size regression was the other one, and it is fixed (§22.2).

---

## 23. Optimization architecture

Flint currently emits LLVM IR and lets LLVM optimize. That is a sound
foundation (it is why the f64 loop is at C parity [M]) but it cannot
express Flint-specific knowledge: leases, regions, array lengths, overflow
ranges.

| optimization | expected benefit | compile-time cost | memory cost | difficulty | safety impact | size impact | priority |
|---|---|---|---|---|---|---|---|
| **Bounds-check elimination via range analysis** | up to 13 % on integer loops [M] | medium | low | medium | none (removes checks) | small (smaller code) | **1** |
| **Same-value bounds (reslicing) — hoist the length** | most of the above, without a new IR | low | none | low | none | none | **1** |
| **Inline the array accessors** | enables the above + removes call overhead | low | none | low | none | small | **1** |
| **Overflow-check elimination via range analysis** | 5–13 % on integer loops [M] | medium | low | medium | none | small | 2 |
| **Escape analysis + stack promotion** | removes heap traffic in closure-heavy code | medium | medium | medium | improves lifetimes | may grow code | 2 |
| **Devirtualization (trait calls)** | large for polymorphic code | medium | low | medium | none | shrinks code | 2 |
| **Specialization of numeric kernels** | large for the AI/ML domain | low | low | low (monomorphize by type) | none | grows code | 2 |
| **PGO** | 10–20 % on branchy code [P: general LLVM PGO results] | low (offline) | low | low | none | none | 3 |
| **LTO (app + runtime together)** | 5–15 % + enables cross-module inlining of the accessors | high | high | medium | none | shrinks code | 3 |
| **Vectorization (domain-aware)** | large for numeric domains | medium | medium | high (needs typed layout) | none | grows code | 3 |
| **Whole-program constant propagation** | moderate | high | high | high | none | shrinks code | 4 |
| **Custom register allocator / instruction selection** | ~0 (LLVM's is C-class) | very high | high | very high | none | — | **never** |

Note the ordering: the first three rows are *cheap* and account for the
measured 13 % plus most of the BCE failure mode [M]. The expensive
optimizations come after the IR exists (§13).

---

## 24. Standard-library architecture

### 24.1 Does the stdlib cause the size problem? [M]

Before the link fix: yes, entirely — the whole stdlib was linked into every
program (743 KB). After the fix: no — a hello-world pulls in `runtime.o`
only, and the binary is 4,536 B stripped [M]. The stdlib's *code* is not
the problem any more.

What remains:

- **`.bss` reservations**: 72 MB of fixed arenas in the Rust stdlib
  (arena 48 MB, compress 16 MB, sqlite 4 MB, crypto 1 MB) [M]. These are
  in `.bss` (reserved, not stored), so they cost *address space* and
  possibly RSS depending on touch behaviour. For a "tiny" profile they
  should be sized down or made lazy.
- **One monolith**: `flint_std.o` is 825 KB of object code with sqlite,
  brotli, png, crypto, encoding, compression, TOML/YAML, and the arena
  [M]. It is one object, so `--gc-sections` can drop *sections* but the
  object is pulled in wholesale when any symbol is referenced. **Splitting
  it per subsystem** would make the granularity finer (each subsystem its
  own object) [D].
- **Link-time coupling**: a program that uses one `flint_std` function
  drags the module's other subsystems' section metadata along [I].

### 24.2 Modular stdlib design [D]

- One object per subsystem (`flint_std_sqlite.o`, `flint_std_brotli.o`, …),
  each compiled with `-ffunction-sections` (already done) — so
  `--gc-sections` can drop whole subsystems.
- **Feature-gated subsystems**: a program that uses SQLite pays for SQLite;
  one that does not pays nothing (already true after the link fix [M]).
- **Keep the quality**: the stdlib's breadth is a competitive asset
  (§2.2). The fix is *granularity*, not subtraction.
- **Generic code bloat**: monomorphization means a generic stdlib function
  is duplicated per instantiation. A shared generic instantiation
  (`-Zshare-generics` in Rust terms) is a possible later win [I].
- **Runtime components** (arena, channels, threads) are always linked;
  that is a deliberate, documented cost of the design (deterministic
  memory management) [I].

---

## 25. FFI and interoperability

### 25.1 Current state [M]

- **C ABI both directions** (8/8 tests): Flint functions are emitted as
  unmangled globals so a main-less `.fl` links against C, and C libraries
  are callable from Flint [M: `tests/test_rust_ffi.sh`].
- **Python embedding** (`py_eval`, `py_eval_float`) with a runtime object
  linked only when the program uses it [M].
- **Rust interop** is via the C ABI (the stdlib is a Rust staticlib) [M].

### 25.2 A safe FFI model [D]

| layer | guarantee |
|---|---|
| `extern "C" fn` declarations | unchecked; the callee's ABI is trusted |
| **safe wrappers** (a Flint package that owns a C resource) | the owner frees it; the wrapper exposes a Flint type with a destructor |
| **handle types** (`opaque struct`) | the only honest way to expose a C pointer; the compiler prevents field access |
| **ownership transfer** | an explicit `take`/`borrow` distinction, so a C-owned resource is not freed by Flint and vice versa |
| **lifetime** | a borrowed C pointer cannot outlive the call that produced it (region rules, §8) |
| **null** | C's `NULL` is representable; `T?` handles it (§11) |
| **error handling** | C returns codes/errno; the safe wrapper converts to Flint's error convention — the boundary is where the difference is absorbed |
| **callbacks** | a Flint function passed to C must be `extern "C"` and cannot capture (or must be captured through an explicit context pointer) |
| **async boundaries** | a callback that may be invoked after the call returns needs an explicit context lifetime — this is where most FFI bugs live |

### 25.3 FFI and `--unsafe` [D]

- Declaring an `extern "C"` function is **not** itself unsafe: the call is
  as safe as the callee is. But the *contract* (pointers, lifetimes,
  ownership) is not checked.
- Any FFI code that relies on an unchecked contract must live inside an
  `unsafe { ... }` block (source-level, §10.3), so the risk is greppable
  and auditable.
- The `--unsafe` *build* flag does not change FFI semantics; it only
  disables the performance checks. These are orthogonal and must stay so.

---

## 26. Bootstrap and compiler self-hosting

### 26.1 The current ladder [M]

- **stage1** — lexer, in Flint.
- **stage2** — parser, in Flint.
- **stage3** — emitter (S-expr → LLVM IR text), in Flint.
- **C++ compiler** — the authoritative front end and the LLVM backend.
- The fixed-point gate proves stage3 re-compiles itself byte-identically
  over 132 cases [M].

### 26.2 The problem [M]

The ladder is a *second implementation of the language's semantics*, and it
has drifted (D5: no traits, no generics, no `::<`, no enum equality). Every
feature now costs two implementations, and the differential gate only
covers the 11 fixtures in its manifest [M].

### 26.3 Target bootstrap strategy [D]

The ladder should be **scoped to what it can stay correct at**, and the
C++ compiler should stay authoritative:

| stage | role | feature scope |
|---|---|---|
| stage1 lexer | token stream | complete (it is) |
| stage2 parser | AST | complete for the *syntax* it supports; may lag on new syntax, and must say so |
| stage3 emitter | LLVM IR text | the subset the C++ backend also implements |
| **HIR/MIR emitter (new)** | the shared semantic core | the authoritative semantics, shared with the C++ compiler |

The key move: **the pipeline stops being a second *semantics* and becomes a
second *front end* that emits the shared HIR** (§13.5, Phase D). Then
traits/generics exist in the pipeline for free, because they are resolved
in the shared phase.

### 26.4 Reproducibility and trust [D]

- The fixed-point gate (byte-identical re-compilation) is the trust
  argument; keep it and extend it to the HIR emitter.
- A **reproducible-build check**: compiling the same source twice, in
  different directories, must produce identical output (paths must not leak
  into the artifact).
- The **trust bootstrap** for a language that compiles itself: the
  "trusting" stage is the one whose output is checked by hand once; from
  then on the fixed point is self-certifying.

---

## 27. Migration strategy

**No rewrite.** The C++ compiler remains authoritative (§13.2); the work is
incremental and every phase leaves the language usable.

| # | change | prerequisite | migration step | compatibility | rollback | testing | benefit | risk |
|---|---|---|---|---|---|---|---|---|
| 1 | **Link hygiene** (§6) | none | done (`46a007e`) | none (output shrinks) | revert one commit | size + full gates | 99 % size cut, 1 dep | **low** (done) |
| 2 | **Benchmark + size gates** (§21) | 1 | add CI job, per-commit JSON | none | disable job | harness self-test | makes every later change measurable | low |
| 3 | **Typed HIR (subset)** (§13) | 2 | new phase: AST → HIR for functions/arith, emit HIR | HIR is internal | feature flag off | golden HIR + differential | unlocks §5/§8/§9 | **high** |
| 4 | **Range/alias analysis on HIR** | 3 | new pass; emit checked vs unchecked access | internal | flag | property tests (safe vs unsafe equality) | the 13 % + BCE | medium |
| 5 | **Escape analysis + regions** | 3 | region tracking; promotion | internal | flag | escape/ownership property tests | escaping closures; no allocation | medium |
| 6 | **Capability types (Send/Sync)** | 3, 5 | marker traits + thread-boundary checks | additive | flag | race-focused tests | static race freedom | medium |
| 7 | **Trait objects + associated types** | 3 | HIR-level trait resolution, vtable layout | **ABI change** (trait objects are new values) | feature flag | differential + layout tests | ecosystem unlock | high |
| 8 | **Domain system** (§15–18) | 2 (measurable), 4 (policy needs passes) | table-driven policy + `@domain` | source-level addition | table empty | "identical output with/without domain" gate | specialization policy | medium |
| 9 | **Package manager** (§20) | 0 (independent) | new `flint` subcommands + registry | independent | opt-out | end-to-end install tests | ecosystem | medium |
| 10 | **Pipeline emits shared HIR** (§26) | 3 | rewrite stage3 to consume HIR | pipeline-only | keep stage3 as is | fixed point + differential | ends the 2× cost | **high** |
| 11 | **Async/await** | 6 (capabilities), 5 (lifetimes) | new syntax + generator | additive | flag | send-ness property tests | the biggest missing feature | high |

Ordering rationale: measure (2) → analyse (3,4) → memory model (5,6) →
ecosystem (7,9) → policy (8) → pipeline convergence (10) → async (11).
Nothing in this plan requires a rewrite, and the language is usable and
improving at every step.

---

## 28. Final scoring

| category | score /100 | evidence | main weakness | priority |
|---|---|---|---|---|
| **Runtime performance** | 62 | [M] 1.35–2.25× behind C/C++/Rust on integer loops; f64 parity; 4–45× faster than Python; 13 % of the gap is checks; **the rest is lost auto-vectorization** (C: 16 vector ops, Flint: 0) with the cause measured in §4.4 | no range analysis, so the checks that block vectorization cannot be removed | P1 |
| **Binary size** | 92 | [M] 4,536 B stripped hello (C: 4,368 B; Rust: 339,656 B) after the link fix; `.bss` 24 B; one dependency | 72 MB of arenas in library-heavy programs; one monolithic `flint_std.o` | P2 |
| **Memory safety** | 78 | [M] compile-time use-after-free, bounds/overflow/div-zero checks, leases + runtime backstop, deterministic free, 135-message diagnostic catalog | no precise regions; closures cannot escape; FFI contracts unchecked | P1 |
| **Concurrency safety** | 42 | [M] threads + channels exist; no static race freedom; `parallel for` removed | no capability model | P1 |
| **Type system** | 55 | [M] inference, generics (monomorphization), traits (M3), lambdas/closures (M4), enums, structs, maps, explicit type args (M2b) | no async, no associated types, no `dyn`, no operator overloading, no nullable types | P2 |
| **Compiler maturity** | 70 | [M] 21 green suites, differential + fixed point + no-hang gates, self-hosting ladder, 135-message catalog | two frontends that drift (D5); the link regression shipped for months | P0 (D5) |
| **LSP/tooling** | 42 | [M] separate Rust LSP (5/5 tests), formatter, token dump, agent benchmark, agent-bench tasks | no compiler-backed semantics, no rename/refactor, no incremental analysis | P2 |
| **Ecosystem** | 12 | [M] zero third-party packages; strong stdlib; binary registry exists | no package manager, registry, resolver, lockfile, signing, audit | P1 |
| **Developer experience** | 66 | [M] one binary for compile/JIT/IR/interface/test; 8-lesson tutorial; one-error-per-input contract; agent-friendly contracts | young tooling, no package story, `flt_` naming inconsistencies, limited docs | P2 |
| **Cross-platform support** | 58 | [M] Linux/Android verified here; Windows/macOS claimed but never executed from this device; aarch64/x86_64/wasm32 targets | untested platforms, QBE backend unavailable here | P2 |
| **Overall (weighted)** | **61** | — | — | — |

**Weighting [D]:** runtime 15 %, memory safety 15 %, type system 12 %,
compiler maturity 12 %, ecosystem 10 %, concurrency safety 10 %, binary size
8 %, developer experience 8 %, LSP/tooling 6 %, cross-platform 4 %. Safety
and maturity outweigh ecosystem, because ecosystem follows the rest; a
package manager without a sound compiler and a fast compiler just
distributes bugs.

**Deliberate non-inflation [I]:** the standard library's breadth does not
raise the score. It is a *mitigating factor* for the ecosystem score
(12 → maybe 20 with a package manager), not a compensating strength.

---

## 29. Final prioritized roadmap

### Phase 0 — Measurement (4–6 weeks)
- Land the benchmark + size + startup harness in CI with per-commit JSON
  (§21). Include the new `tools/bench_compare.sh` (5 languages) and
  `tools/bench_tooling.sh`.
- Add `flintc --print-size` (size attribution JSON) and
  `flintc --check-bce` into the dashboard.
- Freeze the baseline: every later phase is measured against it.
- **Exit criteria:** a CI job that reports runtime/binary/compile-time
  deltas per commit, and a documented baseline.

### Phase 1 — Recover regressions (2–4 weeks)
- Link hygiene (**done**, `46a007e`): `--gc-sections`, `--as-needed`,
  conditional native libraries, BLAS weak-symbol fix [M].
- Bisect the remaining 1.6× performance gap: compare optimized IR against
  C's (§22.3).
- Split `flint_std.o` per subsystem so `--gc-sections` can drop whole
  subsystems; make the 72 MB of arenas sizeable per profile (§24).
- **Exit criteria:** integer loops within 1.3× of C; every binary's
  `.bss` and dependency list visible in the dashboard.

### Phase 2 — Compiler unification (8–12 weeks)
- Introduce HIR/MIR alongside the current path; migrate codegen to consume
  it incrementally (§13.3, §13.5).
- Move generic instantiation and trait resolution into the type phase.
- Give the Flint pipeline a path to emit the same HIR (end the D5 drift).
- **Exit criteria:** the pipeline compiles a program using traits, generics
  and `::<`; D5 is closed; the LSP can query the compiler for types.

### Phase 3 — Static safety (8–12 weeks)
- Range analysis + BCE + overflow-check elimination on HIR (§5, §23).
- Regions + escape analysis + stack promotion (§8, §12).
- Capability types (Send/Sync) and typed channels (§9).
- Nullable types (`T?`) (§11).
- **Exit criteria:** safe build within 1.2× of `--unsafe` on the
  benchmark suite; escaping closures work; a data race in a test program
  is a compile error.

### Phase 4 — Type system (8–12 weeks, parallel with Phase 3)
- Associated types; `dyn`-style trait objects; operator overloading;
  const generics (last); async/await (after capabilities).
- **Exit criteria:** a third-party package can implement a trait and
  return it polymorphically.

### Phase 5 — Optimization (6–8 weeks)
- Inline the array accessors; same-value bounds; loop versioning for
  bounds checks; devirtualization; numeric specialization; PGO; LTO of app
  + runtime; domain-aware vectorization.
- **Exit criteria:** the benchmark suite shows the gap to C closing
  monotonically, with no safety regression.

### Phase 6 — Binary-size engineering (4 weeks)
- Per-subsystem stdlib objects; profile-driven arena sizing; optional
  unwinding (`-fno-asynchronous-unwind-tables` where possible — a Rust
  internals thread notes `overflow-checks=trap` shrinks unwind data
  substantially [P: 2026-02]); the `tiny` profile.
- **Exit criteria:** a "tiny" hello-world under 2 KB stripped; a
  library-heavy program under 100 KB.

### Phase 7 — LSP/tooling (6–10 weeks)
- Compiler-backed LSP (same type checker as the CLI); rename/refactor;
  incremental per-file analysis; domain awareness in completion/hints.
- **Exit criteria:** the LSP reports *identical* diagnostics to the CLI
  (a test compares them over the corpus).

### Phase 8 — Package ecosystem (10–14 weeks)
- `flint` package subcommands; registry; resolver; semver; lockfile;
  content-addressed cache; signing; audit; publish; discovery.
- **Exit criteria:** a package can be published, installed by another
  user, and built reproducibly from a lockfile.

### Phase 9 — Domain-aware compilation (6–8 weeks)
- `@domain(...)` syntax; the data-driven policy table; safety separation
  enforced by construction and by a test; LSP and build integration.
- **Exit criteria:** `@domain(ai)` measurably improves the AI benchmark
  suite; the "identical output with and without the domain" gate passes for
  every domain.

### Phase 10 — Long-term differentiation (ongoing)
- Where Flint could beat C/C++/Rust: (a) **safety with no borrow-checker
  tax** (leases + regions + backstop, and the ergonomics to match Rust's
  without the fight) [I]; (b) **deterministic memory with a runtime
  backstop** — a niche Go/Python cannot enter and Rust declines; (c)
  **an agent-native toolchain** (stable tokens, machine-readable
  diagnostics, one binary) — the one axis where no incumbent ships
  anything comparable; (d) **domain-aware specialization without unsafe
  escape hatches**, if the safety/optimization separation (§17) holds.
- Each is a *narrow, defensible* claim — the point is not to out-C C, but
  to be the best tool for a specific class of work.

---

## 30. Roadmap requirements

### 30.1 Top 5 to work on immediately

1. **Binary-size + performance gates in CI** (Phase 0/1). Every other
   item depends on being able to measure change; two of this assessment's
   findings were invisible to a green test suite.
2. **Link hygiene** (Phase 1) — **already shipped** (`46a007e`); the
   remaining work is the IR comparison that explains the last 1.6×.
3. **Typed HIR** (Phase 2) — the precondition for range analysis, escape
   analysis, capabilities, and the LSP; nothing else unblocks as much.
4. **Range analysis + BCE** (Phase 3) — the measured 13 %, and the
   technique Rust itself struggles with in loops [P].
5. **Package manager** (Phase 8) — the only item that can change the
   *shape* of the project, and it is independent of the compiler work.

### 30.2 Top 5 that should wait

1. **Async/await** — needs capabilities and lifetimes first; async without
   `Send` is unsafe async.
2. **Type-level programming** — high complexity, low immediate value, and
   it would make the type system hard to explain.
3. **Region-based memory management** — a large semantic change that would
   conflict with the deterministic-free goal; escape analysis gets most of
   the benefit at a fraction of the cost.
4. **A custom register allocator / instruction selection** — LLVM's is
   already at C parity for this code [M]; there is no measured headroom.
5. **Rewriting the compiler in Flint** — the ladder should converge on the
   shared HIR, not become a second authoritative compiler.

### 30.3 Top 5 architectural decisions that must be made correctly now

1. **The C++ compiler stays authoritative; the Flint pipeline converges on
   a shared HIR** (§13.2, §26). Getting this backwards means either a
   rewrite or permanent 2× cost.
2. **Safety is a property of the compiler, not of a flag** — `--unsafe`
   and `unsafe {}` are explicit, auditable, and *never* implied by a
   profile, domain, or tool (§10, §17).
3. **A typed IR is the stable boundary** — it is the precondition for
   range/alias/escape analysis, real diagnostics, and an LSP that cannot
   drift (§13.3).
4. **The stdlib is modular and linked per subsystem** — one object per
   subsystem, so size and startup stay small without shrinking the library
   (§24).
5. **Measurement is infrastructure, not a phase** — benchmarks, size
   attribution and safety counts are permanent fixtures that gate every
   commit (§21). The 743 KB hello-world is the argument.

### 30.4 Final recommendation

Optimize for: **safety by default + near-C/Rust performance + competitive
binary size + excellent tooling + unified compiler architecture + simple
developer experience + domain-aware optimization.**

Flint is closer to that target than it was a week ago: the binary-size
regression is fixed (743 KB → 8 KB, C parity) [M], the type system has
inference, traits, closures, generics and explicit type arguments [M], the
diagnostics are a catalogued contract [M], and the toolchain is one binary
[ M]. The three gaps that matter most are: **no typed IR** (which blocks
the safety-performance story), **no package ecosystem** (which blocks
growth), and **two frontends** (which blocks velocity). The roadmap above
attacks them in that order, with measurement first, and without a rewrite.

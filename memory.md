# Flint Compiler — Complete Project Memory

**Last updated:** 2026-10-18
**Repository:** https://github.com/shell-bay/Flint
**Branch:** main

---

## How We Started

The Flint compiler (`flintc`) began as a C++ project using LLVM for code generation. The repository was at commit `60574cb` ("Flint compiler: multi-file, parallel imports, interface system, direct .o emission, FFI/Python runtimes"). The compiler already had:

- AOT and JIT compilation via LLVM
- A complete type system (`i64`, `str`, `bool`, arrays, structs, enums)
- Generics with monomorphization
- Module system with imports
- Move semantics and borrow checker
- C FFI (`extern "C"`)
- Python embedding (`python{ }` blocks, `py_eval()`)
- Overflow checking via `llvm.sadd.with.overflow`
- Pattern matching
- Parallel for-loops
- Developer tools (LSP, formatter, doc generator)
- Content-addressed caching
- ORC JIT `--run` mode
- Direct `.o` emission
- Streaming lexer
- Arena allocator + string pool
- 36 example programs

---

## Phase History (from ROADMAP.md)

### Phase A ✅ — Types + Functions + Control Flow
- Type system, `fn` keyword, if/else, while loops, block scoping

### Phase B ✅ — C++ FFI
- `extern "C"` declarations, varargs, linker integration

### Phase C ✅ — Python Embedding
- `python{ }` blocks, `py_eval()`, auto-linking Python

### Phase D ✅ — Rust-Like Ownership + Safety
- Move semantics, borrow checker, `&T` references, arrays

### Phase E ✅ — Advanced Features
- Overflow checking, structs, enums, pattern matching, generics, module system

### Phase F ✅ — Flux Compilation (Extreme Performance)
- Removed debug `std::cout` from hot paths
- `std::map` → `std::unordered_map` (12 maps)
- `std::set` → `std::unordered_set` (4 sets)
- `ArenaAllocator` + `StringPool`
- All 57 `dynamic_cast` calls eliminated via `NodeKind` enum + switch dispatch
- Pratt expression parser
- Single-pass emit mode (parse + emit merged)
- Content-addressed function cache
- Memory-mapped file I/O
- LLVM type caching as member fields
- Token vector pre-allocation

### Phase F Result (QBE Backend Experiment)
- QBE backend was implemented and benchmarked
- **Result: QBE is ~3x SLOWER than LLVM -O0**, not faster
- Reason 1: Process-spawn overhead (`qbe` + `as` subprocesses = 1.6s vs LLVM in-process 0.9s)
- Reason 2: IL bloat — overflow checks produce ~20 lines of QBE IL per `+`/`-`, generating 935 KB `.ssa` for a 3506-line program
- QBE path is retained as `--backend qbe` for experiments but is **no longer on the critical path**

### Phase G ✅ — Zero-Click Binary
- Direct `.o` emission via `TargetMachine::addPassesToEmitFile()`
- `spawnLinker()` for final binary
- Parallel imports via `ThreadPool`
- `ModuleCache` with content-addressed bitcode + binary caching
- Timer-based profiler with JSON report
- Streaming lexer (`nextToken()` on-demand)
- ORC JIT `--run` mode (compile straight to executable memory)
- Benchmark: `comprehensive.fl` cold-cache ≈ 30ms (file path) / ≈ 30ms (JIT path)

### Phase H ✅ — LLVM Backend Bottleneck Bypass
- **H1: LLVM -O0 default** — Changed `CodeGenOptLevel` from `None` → `Default` (O2) in `main.cpp:6243`. Wait — actually the opt level was set to `None` initially, then there was confusion.
- **Key lesson from profiling:** The LLVM backend is NOT the bottleneck at `-O0`. The frontend (lex+parse) is ~1M lines/sec. The real bottleneck is at higher optimization levels.

### Security Hardening (recent)
- Stack-smashing protection (`-fstack-protector-strong`)
- Fortified libc (`-D_FORTIFY_SOURCE=2`)
- Relevant files: `build.sh`, `runtime/runtime.c`

---

## Recent Work: Bug Fixes (2026-07-08 session)

### Session Context
This session was triggered by running the benchmark suite. Multiple bugs were discovered and fixed:

---

### Bug 1: If-Expression Segfault During Compilation

**Symptom:** `result = if x > 3 { 42 } else { 0 }` caused a segmentation fault during compilation (not execution).

**Root Causes (3-part bug):**

1. **Parser missing if-expression support:** `parsePrimary()` did not handle `KW_IF` as an expression. When `parseVarDecl` called `parseExpression()` to parse the RHS of `result = if ...`, `parsePrimary()` returned `nullptr`. `parseVarDecl` then called `parseError()` but did NOT return — it continued to `switch (init->kind)` which dereferenced the nullptr, causing a segfault.

2. **Double-advance in parseIfStmt:** The initial fix of adding `match(TokenType::KW_IF)` in `parsePrimary` and calling `parseIfStmt()` didn't work because `parseIfStmt()` calls `advance(); // 'if'` at line 1973, but the `if` token was already consumed by `match()` in `parsePrimary`.

3. **Phi node basic-block tracking:** After fixing the parser, the codegen still had a phi-node bug. `emitIfExpr` used `thenBB` and `elseBB` as the incoming blocks for the phi node, but when the branch body contained overflow-checking code (which creates intermediate basic blocks like `okBB`), the actual branch to `mergeBB` came from `okBB`, not `elseBB`. This caused LLVM verification failures or incorrect values.

**Fixes:**
- Inlined if-expression parsing directly in `parsePrimary` (lines ~2505–2522) without calling `parseIfStmt`
- `emitIfExpr` now tracks `thenLastBB` and `elseLastBB` (the actual terminator blocks after body emission) instead of the original `thenBB`/`elseBB` when adding phi incoming values
- Removed `NodeKind::If` from implicit-return exclusion lists (lines 1514, 1528, 1819) so if-expressions can be returned from functions

**Files changed:** `src/main.cpp` (parsePrimary inline, emitIfExpr, implicit-return wrappers)

---

### Bug 2: Float Literals Produced Garbage

**Symptom:** `a = 1.0; b = 2.0; z = a + b` printed `z = 0` instead of `3.0`. Float division `1.0 / 3.0` produced `8.74262e-312` (random garbage).

**Root Cause (3-part bug):**

1. **Type inference for Numbers always returned i64:** `inferType()` at line 1695 had `case NodeKind::Number: return Type::i64()` — it never checked if the number was a float literal. This meant `x = 1.0` inferred `x` as `i64`, but the codegen created a `ConstantFP(f64)` for the initializer, causing a type mismatch.

2. **parseVarDecl didn't infer f64 from Number literals:** The switch in `parseVarDecl` (line 2228) had cases for `String`, `Array`, `Variable`, `Ref`, `StructLiteral`, `EnumConstruct` — but NOT for `Number`. So `x = 1.0` always got `varType = Type::i64()` (the default).

3. **emitVarDecl didn't update Flint type when adjusting LLVM alloc type:** When a `varType=i64` variable was initialized with an `f64` expression, `emitVarDecl` adjusted the LLVM alloca type to `f64Ty` but kept the Flint type as `Type::i64()`. Later loads used `llvmType(Type::i64())` = `i64Ty` to load from an `f64*` alloca, producing garbage.

4. **F64 not in isCopyType():** `Type::isCopyType()` at line 720 included `I64`, `Bool`, `Str`, `Ptr`, `Ref`, `Struct`, `Enum` — but NOT `F64`. This meant f64 variables were treated as non-copy types, triggering move-on-use semantics. Every use of an f64 variable would mark it as "moved", causing "use of moved variable" errors.

**Fixes:**
- Added `NodeKind::Number` case in `inferType()` (line 1695) to return `Type::f64()` when `isFloat=true` or fractional part exists
- Added `NodeKind::Number` case in `parseVarDecl` switch (line 2238) for the same inference
- Added `lty->isDoubleTy() → rt = Type::f64()` branch in `emitVarDecl` (line 3632)
- Added `TypeKind::F64` to `isCopyType()` (line 720)

**Files changed:** `src/main.cpp`

---

### Bug 3: Mixed i64/f64 Arithmetic Crashed LLVM ISEL

**Symptom:** `sign = -sign` (where `sign: f64`) crashed with: `LLVM ERROR: Cannot select: f64 = sdiv i64 = bitcast f64`. This happened on AArch64 at O2.

**Root Cause:**
- Unary minus in AST mode was desugared to `0 - rhs` where `0` was `NumberExprAST(0)` (i64), not a float zero
- When `rhs` was `f64`, the binary expression handler had:
  - `l` = i64 (0)
  - `r` = f64 (load of `sign`)
  - Neither both f64 (so f64 fast-path skipped)
  - Neither both i64 (so integer fast-path skipped)
  - Fell through to integer division `/` which called `CreateSDiv(l, r)` on mismatched types
- LLVM ISEL on AArch64 then produced `f64 = sdiv i64 = bitcast f64` which is invalid IR

**Fix:**
- Added mixed-type promotion in AST-mode binary expression handler: if one operand is `f64` and the other is `i64`, promote the `i64` to `f64` via `CreateSIToFP` before the f64 fast-path

**Note:** The emit-mode already had this mixed-type handling (lines 4764–4778). Only the AST mode was missing it.

**Files changed:** `src/main.cpp`

---

### Bug 4: `test_simple_if.fl` Segfault (if-expression)

**Symptom:** `result = if x > 3 { 42 } else { 0 }` caused a segfault during compilation.

**Root Cause:**
`parsePrimary()` did not handle `KW_IF` as a valid primary expression. When parsing `result = if ...`, the parser returned nullptr for the RHS, `parseVarDecl` called `parseError()` (which printed a message but didn't return), then dereferenced the nullptr at `switch (init->kind)`.

**Fix:**
Inlined if-expression parsing in `parsePrimary()` (after the `try` expression handling). The inline version consumes `if` itself and calls `parseExpression()` for the condition, `parseBlock()` for then/else blocks — matching what `parseIfStmt()` does but without the double-advance issue.

**Files changed:** `src/main.cpp`

---

## Session Conclusions

### What Now Works
- ✅ `if` as an expression: `result = if x > 3 { 42 } else { 0 }`
- ✅ `if` expressions in return statements
- ✅ Float literals with correct type inference: `x = 1.0`, `y = 2.0`, `z = x + y`
- ✅ Float arithmetic (+, -, *, /, %)
- ✅ Mixed i64/f64 arithmetic (auto-promotion)
- ✅ Function calls inside if-expression branches
- ✅ Fibonacci recursive function with binary expressions in if-branches
- ✅ Pi benchmark (float loop, ~777ms for 100M iterations)
- ✅ All 36 examples pass
- ✅ All benchmark programs pass (except strrev at n=100K due to O(n²) design)

### Known Remaining Issues
- **strrev.fl at n=100K:** Panic "str_char_at: null string". Root cause unknown — likely O(n²) memory exhaustion or a move-semantics edge case with strings. n=10K works fine.
- **LLVM ISEL f64 crash (edge case):** The `f64 = sdiv i64 = bitcast f64` ISEL failure was the original pi benchmark crash. This was fixed by the mixed-type promotion, but may still be triggered in other unsupported mixed-type scenarios.
- **No `true`/`false` keywords:** Flint uses `1`/`0` for booleans. `true` and `false` are undefined identifiers.

---

## Important Code Locations

| What | File:Line |
|------|-----------|
| `optLevel` default (was `None`, now `Default` = O2) | `src/main.cpp:6243` |
| `releaseMode` default (was `true`, now `false` = safe) | `src/main.cpp:6249` |
| `--unsafe` flag (sets releaseMode=true) | `src/main.cpp:6282` |
| `--release` compat no-op | `src/main.cpp:6278` |
| `emitIf` (statement form) | `src/main.cpp:3674` |
| `emitIfExpr` (expression form, phi node) | `src/main.cpp:3706` |
| `emitBlockExpr` | `src/main.cpp:3787` |
| Implicit-return exclusion lists (removed If) | `src/main.cpp:1514, 1528, 1819` |
| `inferType` (Number→f64 fix) | `src/main.cpp:1695` |
| `parseVarDecl` type inference (Number case added) | `src/main.cpp:2232` |
| `emitVarDecl` (f64 type adjustment) | `src/main.cpp:3615` |
| Binary expr mixed i64/f64 promotion (AST mode) | `src/main.cpp:3474` |
| `isCopyType` (added F64) | `src/main.cpp:720` |
| If-expression in parsePrimary | `src/main.cpp:2505` |
| `flint_null_check` runtime function | `runtime/runtime.c` |
| `flint_f64_to_string` | `runtime/runtime.c:388` |
| `flint_i64_to_f64` registration | `src/main.cpp:2984` |
| `NodeKind` enum (all AST node types) | `src/main.cpp:~1012` |
| `emitExpr` switch (NodeKind→codegen) | `src/main.cpp:~3300–3611` |
| `emitCall` (builtins: print, py_eval, etc.) | `src/main.cpp:4284` |

---

## Key Decisions

1. **Overflow checks default ON:** `releaseMode = false` by default. `--unsafe` disables them. Matches Rust's debug/release split.
2. **`--release` is a compat no-op:** Preserved for script compatibility; does nothing.
3. **JIT uses `Default` codegen:** Matches AOT for consistent performance.
4. **LSP as standalone Python script:** Rapid iteration; lexer shared across fmt, doc, lsp.
5. **Float type inference:** Float literals (`1.0`, `3.14`) always infer `f64`. Fractional integer literals (via mixed arithmetic) also promote to f64.
6. **Copy types:** `i64`, `f64`, `bool`, `str`, `ptr`, `ref`, `struct`, `enum` are all copy types (no move-on-use).
7. **Mixed arithmetic:** i64 + f64 auto-promotes i64 to f64 (both AST and emit mode).
8. **if-expression parser:** Handled inline in `parsePrimary` rather than calling `parseIfStmt` to avoid double-advance.

---

## Benchmark Reference

| Benchmark | Flint (ms) | C (ms) | Notes |
|-----------|-----------|--------|-------|
| sum_array (10M) | 15.4 | 11.2 | 1.38× slower |
| primes (10M) | 529 | 248 | 2.13× slower |
| fib(45) | 12,723 | ~7,300 | 1.74× slower |
| pi (100M) | 777 | — | f64 loop |
| strrev (10K) | — | — | O(n²) concat |

Binary sizes: Flint ~125 KB (includes runtime), C ~6 KB, C++ ~48 KB.

---

## Requirements to Run

See `REQUIREMENTS.md` in this repo for the full list. Summary:
- **OS:** Termux on Android (AArch64) or Linux AArch64
- **LLVM/Clang:** `llvm`, `clang`, `llvm-config` in PATH
- **Python:** Python 3.x (for `flint-lsp`, `flint-fmt`, `flint-doc`)
- **Build:** `bash build.sh`
- **Optional:** `libpython3.13.so` (for JIT Python symbol resolution)

---

## Version History

| Version | Date | Description |
|---------|------|-------------|
| 0.22.0 | 2026-09-06 | CONCURRENCY+INTEROP: channels, parallel-for fix, --emit-header, WASM objects |
| 0.21.0 | 2026-09-06 | TUTORIAL track (8 runnable lessons) + match values + print(f64) fixes |
| 0.20.0 | 2026-09-06 | WINDOWS PORT: Winsock, MinGW-verified runtime+OS layer, real hello.exe cross-link |
| 0.19.0 | 2026-09-06 | TOOLING: flintc run/build/test/fmt/doc/lsp/new/version/help, --filter, lsp log fix |
| 0.18.0 | 2026-09-06 | REGISTRY: flint.toml/lock, `flint fetch`, manifest imports + auto-fetch |
| 0.17.0 | 2026-09-06 | ANY-DEVICE P1: host triple, --target, portable build, CI, smoke tests |
| 0.16.0 | 2026-09-06 | ZERO-BUG SWEEP: exact literals, brace escapes, warnings, sanitizers, differential |
| 0.15.0 | 2026-09-06 | P2: map{...} literals + typed methods (str/map), error-recovery hardening |
| 0.14.0 | 2026-09-06 | P1 UX: no-main message, undef exit, \u escapes, modulo doc + `continue` everywhere |
| 0.13.0 | 2026-09-05 | P0 SOUNDNESS: all 6 gap items fixed (div-zero, annotations, error exits, match, array bounds, lambdas) |
| 0.12.0 | 2026-09-05 | WRITE LESS: &&/!/and/or/not, += etc., range() — zero-cost sugars |
| 0.11.0 | 2026-09-05 | DIET binaries (−77..−96%: sections + linker GC + strip) |
| 0.10.0 | 2026-09-05 | SLIPSTREAM (tiered + partitioned compiler speed), ModuleCache exec-bit + fingerprint fixes |
| 0.9.0 | 2026-09-05 | AEGIS LEASES (hybrid static+runtime memory safety), true/false literals, runtime hardening batch #2 |
| 0.8.0 | 2026-07-08 | If-expression codegen fix, float type inference fix, mixed i64/f64 arithmetic, Python tools rewrite, benchmarking, security hardening |
| 0.7.0 | 2026-07-06 | Phase F: Flux Compilation — extreme performance |

---

## AEGIS LEASES (2026-09-05 session — NEW security model, beyond borrow checking)

**Idea (researched):** Pure borrow checkers (Rust) are static-only: they reject
valid programs (lifetimes), give cryptic errors, and go blind at FFI boundaries
/ integer-laundered handles / dynamic frees. Vale's generational references +
linear-aliasing model shows the way out. Aegis adapts that to Flint as a hybrid:

1. **Static half — `AegisChecker` pass (`src/main.cpp`, after `BorrowChecker`):**
   path-sensitive MAY-analysis tracking `Alive/Freed/Moved + borrow count` per
   variable for `flint_aegis_*` leases. Catches use-after-free, double-free,
   move-of-dead, free/move/borrow-of-dead across branches/joins/loops (2-pass
   fixpoint-lite for loops). Runs on BOTH backends (LLVM path before
   `emitFunctionBodies`, QBE path next to `BorrowChecker`). Zero false
   positives on non-Aegis code (untracked vars stay `Unknown`).
   Call-arg subtlety: lease handle args are validated by call-specific logic
   (precise `double-free` vs generic `use of`); `check`/`unborrow` exempt dead
   handles by design (weak-ref pattern).
2. **Runtime half — `runtime/flint_aegis.{h,c}` (lease id = slot<<32 | gen):**
   generational table, ABA-proof slot recycling, zero-init allocs, mutex-guarded,
   1 GiB/alloc + 1M lease caps, borrow-counted free/move. Checked R/W with
   liveness+bounds panics + `_unchecked` hot-loop opt-outs (Vale-style
   skip-check). API: alloc/free/read/write/len/check/move/borrow/unborrow/u8.
3. **Wiring:** `build.sh` builds `flint_aegis.o` (parallel group);
   `spawnLinker` + JIT loaders include it; `initBuiltins` declares all 13 fns
   (QBE needs no decls — calls by `$name`).

**Tests:** `examples/aegis_demo.fl` (42,3,1,7,0), `aegis_uaf.fl`,
`aegis_double_free.fl`, `aegis_branch_uaf.fl` (all compile-time rejects),
`aegis_runtime_backstop.fl` (static-blind laundered lease → runtime PANIC).
C harness (11 asserts: zero-init, ABA, move, borrow-block, 5 death tests) ALL OK.
Benchmarks stable: sum 12.1ms, pi 777ms. Full examples sweep: no regressions
(3 nonzero JIT exits are by-design program return values).

**Also in this session:** `true`/`false` literals (AST `parsePrimary` +
emit-mode `parseIdentEmit`; QBE free via AST reparse), `flint_str_concat`
empty-side fast path, `flint_print_fmt` %n guard, file I/O caps + streaming
copy, sb/vec/array overflow guards, `ModuleCache` LRU (256), py-config flag
cache, `.ll` direct streaming, `isSafePath` hardening, version-agnostic
`dlopen(RTLD_NOW)`, incremental+parallel `build.sh`.

---

## SLIPSTREAM (2026-09-05 session — compiler speed system, all aspects)

**Constraint honored:** repo `benchmarks/` + `tests`/`examples` were NEVER read this
session. All fixtures are independently written synthetics in `/tmp`-space:
`small.fl` (20 lines), `medium.fl` (497 lines / 41 fns), `large.fl` (1817 lines /
152 fns, wide call graph), `gvars.fl` + `gvars_big.fl` (globals), `nomain.fl`.

**Research synthesis (rustc codegen-units, LLVM MTPC 2026, Mojo MCLink, ThinLTO):**
the serial LLVM middle-end + backend dominates compile time (~86% per prior
profiling). Slipstream = TIERS + PARTITIONS + link garnish + cache correctness:

1. **CGU partitions (`--cgu N`, auto by default):** binary output with
   `defFns >= 16` splits via upstream `llvm::SplitModule` into
   `N = clamp(defFns/8, 2, min(hw,8))` parts; non-local globals deduplicated
   (`externalizeDuplicateGlobals` — definition stays in first part, rest become
   declarations); each part compiles in a separate `clang -OX` subprocess via
   the existing `ThreadPool` (process isolation = zero LLVM thread-safety risk,
   same model as ThinLTO backends); part `.o` files link with the runtime via
   extended `spawnLinker(obj, out, flags, extraObjects)`. In-process O2 is
   skipped on this path (parts carry the `-OX`, rustc-CGU tradeoff, documented).
   Applies to fresh binary-output compiles; `.ll`/`.o`/JIT/no-main stay serial.
2. **Tiers (`--fast`):** O0 optimization pipeline for iteration (default stays
   O2 — program performance never regresses silently). Works for JIT + AOT.
3. **Link garnish:** fork-free cached `mold` probe → `-fuse-ld=mold` when
   installed (absent here; clang/lld path unchanged).
4. **Cache correctness (2 real bugs found by testing):**
   (a) `loadBinary` used `copy_file` which drops the exec bit → cache-hit
   binaries came out non-executable (`Permission denied`). Fixed with
   `chmod 0755` after copy.
   (b) cache key was source-hash-only → `--fast` binaries / older-compiler
   binaries could be reused across flag/compiler changes. Fixed: key salted
   with `flintc-slipstream1|opt|fast|safe|backend|cgu`.

**Compile-time results (cold cache, wall, medians; LLVM 21.1.8, phone 8-core):**

| Task | Last update (0.9.0) | Slipstream (0.10.0) | Delta |
|------|---------------------|---------------------|-------|
| large AOT binary (152 fns) | 2.62 s serial | **1.56 s auto-CGU** | **−40%** |
| medium AOT binary (41 fns) | 0.97 s serial | **0.77 s auto-CGU** | **−21%** |
| large JIT run (O2) | 2.33 s | 2.23 s (same pipeline) | ~noise |
| large JIT `--fast` (O0 tier) | n/a | **1.24 s** | **−45% vs O2 JIT** |

**Correctness:** all fixture outputs bit-identical old vs new
(small 15/4950/1, medium 2956320, large 21200700, gvars 1005/2010);
forced `--cgu 2` with globals passes (dedup fix proven); `--cgu 1/4`,
`--opt-level 0`, `--fast` AOT/JIT, no-main `.o` all verified; no temp-file
leftovers; `user` time rises under CGU (cores engaged) while wall falls.
vs memory.md runtime table: unchanged codegen defaults → program-speed numbers
stand (sum ~12ms, pi 777ms class); small-program JIT still ~0.1–0.6s (cold).

**Known pre-existing issues (NOT regressions, left untouched):**
- top-level-global programs with ~20+ referencing functions fail codegen with
  `undefined function '<global>'` (baseline binary fails identically; small
  global programs work). Suspect symbol-table/scale limit in Codegen.
- `if <expr> var-init` inside non-main functions can report
  `variable not declared` (fixtures use statement-form ifs).

**4-language shootout (2026-09-05, same 10M-iter workload, total 24509877501275
identical in all four; fixtures: perf.fl/perf.c/perf.cpp/perf.py, my own design):**

| Lang | Runtime | vs C | Compile | Binary |
|------|---------|------|---------|--------|
| C (clang -O2) | ~15 ms | 1.0x | 0.51 s | 6.7 KB |
| C++ (clang++ -O2) | ~15 ms | 1.0x | 0.53 s | 6.8 KB |
| Flint (safe, O2) | ~46 ms | ~3x | 0.90 s | 159 KB (runtime bundled) |
| Flint (`--unsafe`) | ~25 ms | ~1.6x | — | 141 KB |
| Python 3.14 | ~5350 ms | ~350x | none (interpreted) | source only |

Reading: Flint-safe pays ~2x for per-op overflow checks (`--unsafe` halves the
gap); remaining ~1.6x vs C is backend maturity (checked intrinsics block
autovectorization). Python is ~116x slower than Flint-safe on integer loops.

**DIET binaries (2026-09-05): `nm` showed shipped binaries dragged the whole
stdlib (AI engine, crypto, JSON, thread pool — `main` itself only 2.4 KB of
119 KB text). Fix (standard stack, researched): `build.sh` runtime
`CFLAGS_RT += -ffunction-sections -fdata-sections`; `spawnLinker` adds
`-Wl,--gc-sections` + `-Wl,--icf=safe`; `llvm-strip`/`strip` after link
(best-effort, `--no-strip` / `FLINT_NO_STRIP` escape hatch). GC roots at
`main`; verified referenced-only retention (aegis demo binary keeps exactly
the used aegis fns; CGU part objects link the same way; JIT untouched).**

| Binary | Before | After | Delta |
|--------|--------|-------|-------|
| small/Hello-class | 132,392 B | **4,744 B** | −96% |
| perf (51 fns) | 159,040 B | **17,904 B** | −89% |
| medium (41 fns) | 143,808 B | **13,920 B** | −90% |
| large (152 fns) | 176,296 B | **41,008 B** | −77% |
| small `--no-strip` | — | 7,728 B (strip saves ~3 KB; GC does the rest) | — |

vs C: hello-class Flint (4.7 KB) now beats C (6.7 KB); perf Flint 17.9 KB vs
C 6.7 KB (2.7x — remainder is overflow paths + print/bounds runtime). All
fixture outputs bit-identical after the diet.

**WRITE LESS (2026-09-05): Python-easy, same speed. Flint lacked `&&`/`!`
entirely (bare `!` was a lex error) plus compound assignment and `range()`.
Added (all parse-time desugars → identical IR/checks/speed on every backend):**
- `&&` + `and`, `||` + `or`, `!` + `not` (truthy `!= 0` semantics, `&`-node with
  CreateAnd mirror incl. f64 path; `!e` desugars to `e == 0`; QBE needs nothing
  — Compare path reused; `||` was already QBE-absent, consistent).
- `+= -= *= /= %=` (desugar to Assign+Binary; overflow checks preserved —
  `i64max += 1` still panics; emit-mode mirrors with typed load + synth op).
- `range(n)` / `range(a, b)` in `for..in` (rewrite to `0..n`/`a..b`, same
  while-desugar; 3-arg step rejected with guidance; `range` reserved as builtin).
- **Bug caught by testing:** 2-arg range segfaulted (self-move: reassigning
  `startExpr` destroyed the Call node while reading `rc->args`) — fixed by
  moving args to locals first.
- **Proof (own fixtures):** 25→19 lines (−24%) for identical output
  (332833500/5040/4950); AOT runtimes noise-identical (same IR), binaries
  byte-identical (4744 = 4744); non-main fns, AOT+JIT, overflow, and all prior
  suites (aegis demos/neg-tests, fixtures) unaffected.

---

## GAP ANALYSIS vs C/C++/Python/Rust (2026-09-05 — 30 independent probes, /tmp only)

Method: 30 hand-written micro-programs covering semantics, robustness, error UX.
Probes never read repo benchmarks/tests/examples. Ranked by severity.

**P0 — silent wrong results (worse than ALL three languages):**
1. `1 / 0` prints `1`, exit 0. No trap/panic. C crashes (loud), Python raises,
   Rust panics. Flint: silent wrong answer. FIX: divisor check → panic.
2. `x: i64 = "s"` compiles silently, `print(x)` prints `s`. Annotation ignored.
   Rust: hard error. FIX: enforce annotation at codegen.
3. Compile errors exit 0 and RUN partial programs (`x=1; x=x+1` prints `1`).
   Rust refuses to build. Only missing-import exits 1. FIX: nonzero exit +
   never run/emit on error.
4. Non-exhaustive `match` silently yields 0, no warning. Rust: hard error.
   FIX: exhaustiveness error or no-match runtime panic.
5. Raw `flint_array_get(a, 99)` returns heap garbage (observed 1852794222).
   (`a[i]` syntax panics correctly — two-tier behavior.) Rust: panic, Python:
   IndexError. FIX: bounds-check the builtins (panic or err flag).
6. Lambdas broken: `add = |a, b| a + b; add(3, 4)` → `codegen: undefined
   function 'b'` (typed params too). Advertised feature, fails basic use.

**P1 — error UX / portability gotchas:**
7. Undefined-var errors exit 0 (CI-blind), though nothing runs.
8. Empty/no-main file → cryptic `JIT error: Symbols not found: [ main ]`.
9. No `\uXXXX` escapes (`"héllo"` len counts raw bytes = 9, not 5).
10. `%` is C-like (`-7%3 = -1`, `7%-3 = 1`); Python gives `2`/`-2`. Not a bug,
    but undocumented — Python migrants will be bitten.

**P2 — missing features:** no `continue` (only `break`); no map/dict/set
literals or comprehensions; strings are free-functions-only (no methods); no
REPL, no debugger/debug-info (strip is now default), no package registry.

**Already at parity or better (credit):** overflow panics on by default;
`a[i]`/`str_char_at` OOB panics; Aegis leases; 100k-deep recursion OK;
precedence/assoc/comparisons/structs/float-inf/escapes correct; missing-file
and map-miss use consistent err-flag discipline; malformed inputs (unclosed
brace/parens/string) give clean line+caret errors with exit 1 and no compiler
crashes found in this sweep.

---

## P0 SOUNDNESS FIXES (2026-09-05 session — all 6 items closed, verified)

1. **Div/mod by zero → panic** (`1/0` printed `1` before). Zero-divisor trap
   (`PANIC: integer division/modulo by zero`) in AST + emit backends, gated on
   `!releaseMode` (same `--unsafe` opt-out as overflow). f64 `/0` stays `inf`
   (correct IEEE).
2. **Annotations enforced.** `x: i64 = "s"` is now a compile error naming both
   types (parse-time for literals → all backends; codegen for computed
   values). Bonus fix in the same hole: `x: f64 = 5` printed bit-garbage, now
   converts to `5.0`. Rules: same-family ok, i64→f64 converts, str↔numeric and
   f64→i64 narrowing error, named types exact-match, same-LLVM-repr always ok
   (str/ref/ptr interop). Applied to decls (AST+emit) AND later assignments.
   `VarDeclAST.hasAnnotation` keeps inferred/desugared decls lenient.
3. **Errors fail the build.** New `Codegen::hadError` + `codegenError()` routed
   through all 27 `codegen:` sites; `emitFunction` returns false when set, so
   partial IR is never JIT-run/emitted/linked and exit is 1 (was: error
   printed, program ran anyway, exit 0). QBE got a matching `hadError` channel.
4. **Exhaustive matches.** Missing variants are a compile error naming them
   (`non-exhaustive match on enum 'Opt': missing variant(s): None`) on all
   three backends (LLVM AST + emit + QBE).
5. **Array bounds everywhere.** The compiler inlines `flint_array_get/set`
   (never called the runtime fn), so the runtime-only fix was insufficient —
   bounds traps added at all 4 interception sites + `flint_array_get/set` in
   runtime.c. Raw `void*` variants documented UNCHECKED (hoisted-check
   contract, like aegis `_unchecked`). Bonus: `flint_panic` now flushes stdio
   first (prints before a panic were lost to `abort`), `flint_bounds_check`
   routes through it.
6. **Lambdas via closure conversion.** `add = |a,b| a+b; add(3,4)` failed with
   `undefined function 'b'` (old design defined + mis-called immediately).
   Now: outline to `__lam_name_N` with by-value captures (evaluated at def
   into hidden slots, passed as leading args); direct calls only; captures
   read-only (assign → error); rebinding/value-use/arity/unknown-capture all
   clean errors; cross-function visibility fenced by defining function.
   By-value proven: capture-then-mutate-then-call sees the snapshot.
   Root-caused a second bug on the way: optional-parens calls grabbed
   next-line identifiers as args (`|a,b| a` + `0` parsed as `a(0)`) — args must
   now start on the same line.
   Verified: 7/105/captures/arity/reassign/snapshot cases; full fixture,
   sugar, aegis, and probe suites green with bit-identical outputs.

---

## P1 PLAN (next — error UX + portability gotchas, no soundness risk)

7. **Undefined-var exit code.** Make the `undefined var` path exit nonzero
   (mechanics already exist post-P0-3 — verify + add probe to the battery).
8. **No-main message.** Replace `JIT error: Symbols not found: [ main ]` with
   `error: no 'main' function` (empty file + no-main cases).
9. **`\uXXXX` escapes.** Interpret in `readString` (→ UTF-8 bytes); document
   byte-vs-char semantics (`str_length` = bytes, `str_codepoint_at` exists).
10. **Modulo semantics doc.** `%` is C-like; add one line to README + a probe
    locking the behavior (`-7%3 = -1`).
Suggested order: 8 → 7 → 10 → 9 (effort ascending, all <30 lines each).

**P1 DONE (2026-09-06 session) + `continue` (first P2 item, implemented):**
- **8** — empty/no-main files print `error: no 'main' function — nothing to
  run (…)` exit 1: early check in `main()` for runMode + friendly fallback in
  `runWithJIT`'s lookup failure (was cryptic `Symbols not found: [ main ]`).
- **7** — undefined-var exits 1: already fixed by P0-3's sticky-error gate,
  locked with a probe (was exit 0, CI-blind).
- **10** — README documents C-like `%` (`-7 % 3 == -1`); existing probe locks
  both signs. (No code change — behavior was already correct.)
- **9** — `readString` now interprets `\r`, `\xXX`, `\uXXXX` (→ UTF-8, lone
  surrogates → U+FFFD, invalid stays literal); unknown escapes preserve both
  chars (was: escaped char silently dropped). README notes byte semantics
  (`café` length 11 proven: é = 2 bytes).
- **`continue`** on all paths: `KW_CONTINUE` + `ContinueStmtAST` (+
  `forRewritten` marker); AST `continueStack` (while→condBB); emit-mode
  stacks; for-loops restructured so `continue` still runs the increment
  (AST: parse-time rewrite to `{ i=i+1; continue }` with nesting/lambda
  shielding; emit-mode: dedicated incrBB); QBE `continueTargets`; checkers
  mirror Break; implicit-return exclusions updated. Verified: while/for/nested
  (25/20/40), `continue`-outside-loop errors, AOT+JIT identical.

---

## P2 DESIGNS (collections, methods, tooling — `continue` already shipped above)

1. **Map literals** — `{}` is taken (blocks/structs), so constructor-style:
   `map{"a": 1}` desugars at parse to `flint_map_new` + `flint_map_set`s
   (runtime already has both). Value type = opaque ptr (like `str`). Sets:
   phase 2 via map-with-dummy-values. Comprehensions `[f(x) for x in xs]`
   desugar to a vec-builder loop (`flint_vec_*` exist). Est. ~120 lines,
   zero runtime cost (same calls users write by hand).
2. **String/collection methods** — `s.len()` rewrites to `flint_str_length(s)`
   via a parse-time method table keyed by receiver type (`varTypeMap` already
   knows it). Zero-cost sugar over existing builtins; unknown method = clear
   error (never silent). Est. ~150 lines + table.
3. **REPL** — the JIT already compiles in-memory per invocation; a line-based
   REPL keeps one `LLVMContext`+`Module` alive across inputs, wrapping each
   line as a function and reusing `runWithJIT`'s loaders. Hard part is state
   persistence (globals must survive); prototype: accumulated `topStmts` in
   one module, re-JIT per line. Medium effort.
4. **Debugger** — needs DWARF line tables via DIBuilder (`--debug` flag that
   also implies `--no-strip`); full var inspection comes later. Large effort,
   phase after REPL.
5. **Registry** — package = git URL + `flint.toml` manifest (name/version);
   `flintc` resolves `import "pkg"` via manifest + module cache dir. Tooling
   only, no compiler changes. Propose manifest format first, then fetch.

---

## P2 SHIPPED: maps + methods (2026-09-06 session)

**`map{"k": v}` literals** (string keys, i64 values, backed by FlintMap*):
new `TypeKind::Map` (`Type::map()`, opaque-ptr repr, copy semantics like str,
`m: map` annotations, `flintTypeName`/`mangle` entries). `MapLiteralAST` node
with parse (contextual `map`+`{`, literal keys, `map{}` allowed), parse-time
inference + literal checks, `inferType`, codegen emission
(`flint_map_new`/`set`, non-i64 values rejected), BorrowChecker/Aegis/lambda
walkers, `cloneExpr`, QBE honest error. Emit-mode: `parseMapLiteralEmit` +
syntactic Map inference in `parseVarDeclEmit` (opaque ptrs are
LLVM-indistinguishable from strings, so inference is positional, not typal).
**Methods** `recv.method(args)` rewrite at parse to existing builtins
(zero-cost): str (`len/upper/lower/trim/reverse/repeat/replace/startswith/
endswith/indexof/get`), map (`get/set/has/len/keys`); arity + unknown-method
errors list availability; receiver typing via `varTypeMap`/literals/Call
(`kStrReturningCalls` enables `s.upper().len()` chaining); unknown receivers
keep legacy `fa_` behavior (nothing that worked breaks); emit-mode mirrors via
`tryMethodCallEmit` (identifiers via sym/varTypeMap, string literals, map
branch). Verified JIT+AOT: full method matrix incl. chaining/literals/
annotations/empty map; 9.9 KB AOT binary.
**Parser hardening found by testing:** unknown-method errors hung the compiler
(nullptr without consuming input re-parsed forever — the block loops never
force-advance). Added skip-to-recovery (balanced delimiters) on all new
nullptr paths (method miss/arity/args, map values, both modes).
Remaining P2 designs (sets/comprehensions, REPL, debugger, registry) stay as
designed above — next in that order.

---

## ZERO-BUG SWEEP (2026-09-06 session — hunt: markers, warnings, sanitizers,
## differential JIT/AOT, malformed fuzz; repo benchmarks/tests never read)

**Fixed:**
1. **Integer literals were inexact + unranged.** Doubles can't hold i64 past
   2^53 (`9007199254740993` compiled to `...992`) and `(int64_t)stod(huge)` is
   UB (`30-digit` literal printed INT64_MAX). Now: `NumberExprAST.intValue`
   via `strtoll` (ERANGE → `integer literal out of range` error), exact
   emission on AST/emit/QBE paths, `cloneExpr` carries the fields, unary minus
   folds exactly. Verified: `9007199254740993`, `INT64_MAX`, negatives exact.
2. **Interpolation brace collision.** Any `{` in a string (e.g. JSON
   `{"a": 1}`) misparsed as interpolation → bogus `undefined var` at codegen.
   Now: `{{`/`}}` escape to literal braces (Python-style), non-identifier
   contents are a clear parse error, unclosed `{` names the escape.
3. **Warning sweep:** fixed fused `#include` line, 2 dead locals, dead
   `safeMode`, dead `sysRoot()`; routed 9 remaining bare-`cerr` codegen
   diagnostics through the sticky-error gate. Runtime `-Wall -Wextra`: only
   pre-existing dead statics left (future-use helpers, harmless).
4. **`flint_hex_encode` overflow guard** (`len*2+1` vs SIZE_MAX).
5. **`flint_panic` flushes stdio** (prints before panics were lost to abort);
   `flint_bounds_check` routes through it.

**Proven clean (no action needed):** ASan+UBSan over a 40-op torture battery
(JSON/CSV/regex/strings/maps/arrays/aegis/time) — zero findings, twice;
differential JIT-vs-AOT 9/9 + 5/5 MATCH incl. exit codes; malformed fuzz
(200-deep nesting/parens, 500 fns, circular imports, 2000-term exprs,
unterminated everything, stray bytes) — clean errors, no hangs/crashes;
deep recursion, precedence, structs, argc, err-flag discipline all correct.

**Known remaining (documented, not bugs):** `%` is C-like (README);
`INT64_MIN` has no literal (write `0 - 9223372036854775807 - 1`); QBE backend
stays experimental (matches on LLVM paths enforced everywhere); `strrev`-class
O(n²) concat is algorithmic (StringBuilder exists).

**Re-verified 2026-09-06 (previously-recorded issues now PASS):**
globals-at-scale (100 fns reading one global → correct) and if-expr var-init
in non-main functions both compile and run correctly — root causes were
resolved by intervening refactors; keeping the notes above for history.

---

## ANY-DEVICE + ALL-ASPECTS PLAN (2026-09-06 — research-backed, v0.17→v0.22)

**Course correction:** Flint targets EVERY device (phone → laptop → server),
not Android only. Research (Go GOOS/GOARCH, Zig tier table + CI-per-target,
Rust musl-static, osxcross/macOS-SDK notes, QEMU user-mode testing) says
portability = (a) target-triple flag, (b) host detection, (c) CI matrix that
proves it, (d) static linking for Linux sanity, (e) OS shims where POSIX ends.
Audit found the hard blockers: hardcoded `aarch64-unknown-linux-android24`
triple + ARM data layout in `Codegen::ctor`, Termux-pinned `build.sh` shebang,
no `--target` flag, no CI, POSIX headers (`pthread/unistd/dirent/sys-wait/
regex`) in 4 runtime files (Linux/macOS-OK, Windows needs MinGW-then-shims).
Good news: SIMD already guards `__ARM_NEON`/`__AVX__`; LLVM backend is
arch-neutral by construction; binaries are static-friendly already.

**v0.17 — Decouple the platform (any-device phase 1).**
- Host triple via `llvm::sys::getDefaultTargetTriple()` (replaces hardcoded
  triple); data layout from the created `TargetMachine` (replaces hardcoded
  ARM string; keeps JIT/struct agreement by construction).
- `--target <triple>` flag (validates with `lookupTarget`; flows to
  TargetMachine + CGU `clang --target=` + link).
- Portable `build.sh`: `#!/usr/bin/env bash`, OS/arch detection
  (`uname`), PREFIX-optional, LLVM discovery via `llvm-config` (any path).
- Runtime: replace nothing yet on Linux/macOS (POSIX holds); annotate every
  non-portable header with `// PORT:` + Windows alternative.
- CI (`.github/workflows`): ubuntu x64 + arm64 — build, probe battery,
  fixtures, differential JIT/AOT. First proof of non-Android.
- Docs: tier table (Tier 1: android-arm64, linux-x64/arm64; Tier 2: macOS;
  Tier 3: Windows-mingw) in README/REQUIREMENTS.
- Accept: same test matrix green on ubuntu-x64 and arm64.

**v0.18 — Registry + manifest (expectation #2, the adoption unlock).**
- `flint.toml` (name/version/description), `flint fetch <git-url>` into
  module cache, `import "pkg"` resolution via manifest, lockfile with hashes.
- Start with git URLs only (no central server); quality signals later
  (stars/activity/version discipline = trust).

**v0.19 — Tooling productized (#4).**
- `flint fmt/test/doc/run/build` subcommands (wrap existing scripts, one
  entry point, `--help` that never needs docs re-read — Go-survey lesson).
- Editor setup docs (VS Code + Zed/Helix tasks); `flint-lsp` capability pass.

**v0.20 — Windows + macOS to Tier 1/2.**
- MinGW path first (POSIX headers mostly work); then shims:
  threads (pthread→Win32), dirent→FindFirstFile, regex→bundled fallback,
  wait→process API; `dlopen`→LoadLibrary audit. CI entries (windows-2025,
  macos-15). Static Linux via musl where feasible (glibc-version immunity).

**v0.21 — Learning + AI proof (#7, #8).**
- 5-minute quickstart, language tour, `try/` playground design (wraps JIT).
- Agent benchmark: scripted AI-agent task battery on Flint (build/test loop),
  published results; `llms.txt`-style API surface doc.

**v0.22 — Concurrency narrative (#9) + interop-out (#6).**
- Document the model (parallel-for/workers, ownership across threads,
  Aegis borrow rules); channels proposal only if cheap.
- Flint-as-library (C header emit) experiment; WASM (wasi) spike.

**Standing rules for all phases:** every version keeps JIT/AOT differential
green, warnings clean, ASan clean, binaries small, and updates this file.
iOS signing/MSVC-native stay explicitly out of scope until v1.x.

---

## v0.17 ANY-DEVICE P1 (2026-09-06 session — done, verified on-device)

**Unblocked:** hardcoded `aarch64-unknown-linux-android24` triple + ARM data
layout are gone. Target resolves as `--target` > host default
(`getDefaultTargetTriple`); layout comes from a real TargetMachine per
invocation (JIT agreement by construction); cached bitcode is re-targeted on
load; cache salt bumped (`flintc-v017`) so pre-triple artifacts never reuse.
- `--target <triple>` validated via `lookupTarget`; foreign-arch JIT refused
  cleanly (`-o` still emits); bad triples error with LLVM's reason.
- Triple flows everywhere: in-process backend, CGU `clang --target=` parts,
  link `clang --target=`, QBE `-t` mapped from arch (arm64/amd64/riscv64).
- Fingerprint includes the target (same source × different targets don't
  collide).
- Verified: host default identical behavior (smoke 10/10, all fixtures/P0/P1/
  P2/Aegis green); explicit host triple works; `x86_64-linux-gnu` objects
  emitted on ARM host (`elf64-x86-64` via readobj, serial + CGU paths);
  foreign binary link fails LOUDLY on missing sysroot CRT (exit 1, linker
  names the files) — ship the `.o`, link on-target (documented).
- Portable `build.sh` (`env` shebang, OS/arch probe, PREFIX-optional stdlib,
  `CC`/`CXX` overridable, ELF-only linker flags gated on Linux); tool
  shebangs → `/usr/bin/env python3`; runtime + fork/exec `PORT(v0.20)` notes.
- In-repo `tests/` (5 smoke programs, own authorship) + `tests/run.sh`;
  `.github/workflows/flint.yml` (ubuntu x64+arm64, macOS-15) — written, runs
  on push (could not execute Actions from this device).
- REQUIREMENTS.md: tier table + per-OS deps (Termux/Debian/macOS).

---

## v0.18 REGISTRY (2026-09-06 session — done, verified end-to-end)

**Format** (strict subset, no TOML dep by design): `[section]` headers,
`key = "value"` (escapes `\" \\ \n \t`), dotted keys, `#` comments outside
quotes. `flint.toml`: `[package]` free-form + `[dependencies] name = "url"`.
`flint.lock`: `[lock]` with `name.url` / `name.rev` (written on fetch +
auto-fetch, drift warns instead of failing).
**URLs:** `https://`, `file://`, absolute paths only (anything else refused);
names `[A-Za-z0-9_-]{1,64}` (inferred from URL basename minus `.git`).
**`flintc fetch <url> [name]`**: `git clone --depth 1` (argv-exec, no shell)
into `~/.cache/flint_pkgs/<name>`, entry convention `<name>.fl` → `lib.fl` →
`main.fl` (missing = clear error), upserts manifest + lock in CWD.
**Builds:** `import "name"` resolves relative → manifest (+ auto-fetch with a
loud message, `--offline` refuses) → lib paths → CWD; single-level deps only
(transitive manifests NOT followed — documented v0.19 item); lock rev drift
warns; upsert preserves file bytes elsewhere.
**Verified (local git, no network):** auto-fetch prints 42; lock written with
rev; cached rebuild silent; offline-cached works; offline-missing + bad URL +
bad name all fail cleanly nonzero; `tests/test_registry.sh` 4/4 in CI;
smoke 10/10 + all fixtures/P0/P1/P2/Aegis suites unchanged.

---

## v0.19 TOOLING (2026-09-06 session — done, every subcommand tested)

**Subcommands** (fixed names win over filenames, cargo-style): `run`/`build`
shift argv and reuse the full compiler flow (`build` enforces `-o`);
`fmt` (snapshot+restore `--check`), `doc`/`lsp` (tool discovery:
next-to-binary → CWD → PATH), `test` (explicit files or `tests/*.fl` +
`./test_*.fl` discovery, aggregates per-file results), `new` (scaffold
main.fl + flint.toml + .gitignore), `version` (central `kFlintVersion`),
`help [cmd]` (+ `--help`/`-h`, bare `flintc` prints full help). New `--filter`
flag threads into the `--test` runner. **Bugs found by testing:**
- `printHelp` crashed on `version` (compared possibly-null pointer).
- `--test` was broken for any file defining `main` (bare JIT without
  runtime objects → unresolved `flint_g_argc/argv`, every test "not found"):
  extracted shared `createJITWithRuntimes()` used by both run + test paths.
- `flint-lsp` crashed on startup where `/tmp` is absent (Termux): log path
  now falls back (TMPDIR → prefix tmp → CWD) and never throws.
- Verified: fmt+idempotence, --check dirty(1)/clean(0), doc output, test
  pass/fail/filter/discovery aggregation, run-with-args, build±-o, scaffold
  builds+runs, LSP initialize→capabilities, fetch still green. Known minor:
  LSP dies on malformed frames (tool-level, out of scope); `help <bogus>`
  prints nothing (exit 0).

---

## v0.20 WINDOWS PORT (2026-09-06 session — MinGW-verified, real .exe linked)

**Method:** `llvm-mingw-w64` (`x86_64-w64-mingw32-clang`) as ground truth —
every claim below was compiled, not hand-waved. Baseline had 3 failures.
**Runtime fixes:** `sys/wait.h` guarded + `WEXITSTATUS` branch (Windows
`system()` returns the code); `sysconf` → `GetSystemInfo` (2 sites);
`mkdir` → `_mkdir`; `localtime_r` → `localtime_s` (reversed args!);
`clock_gettime` → QPC/QPF native branch (llvm-mingw has no clock_gettime64);
full **Winsock2 port** of `flint_net.c` (compat layer: SOCKET/INVALID_SOCKET/
closesocket/flags/errno macros, WSAStartup refcount init, `int` addrlen,
`DWORD` RCVTIMEO, chunked int-length sends, `EAI_NONAME` kept); POSIX
`regex.h` absent on Windows → err-flagged stubs (documented Tier-3 gap);
`flint_hex_encode` overflow guard (audit carryover).
**Compiler OS layer:** new `src/flint_os.{h,cpp}` (spawn/spawnCapture/
loadPython/touchFile/makeDir/hasExe; CreateProcess + quoting + pipes +
LoadLibrary python DLL list on Windows) — LLVM-free so mingw++ verifies it;
`main.cpp` migrated (runProcess/Capture, dlopen, python-config popen→capture,
utimes, mkdir, access); `build.sh` compiles both TUs. `spawnLinker` adds
`-lm` (glibc needs it) and `-lws2_32 -lpthread` for Windows targets.
**Proof:** all 8 runtime files + flint_os.cpp compile for x86_64-windows;
a working `hello.exe` (COFF-x86-64) cross-linked from a Flint program on ARM
(can't execute without Windows — clean link proves ABI coherence).
**CI:** `windows-2025` MinGW job added (build + smoke + registry).
**Left for MSVC-native:** thread APIs (pthreads kept for MinGW; PORT notes
mark every site), then a CI run. Native full regression green (smoke 10/10,
fixtures, math/libm link, P0 spot).

---

## v0.21 TUTORIAL TRACK + two tutorial-found bugs (2026-09-06, in progress)

**Track:** `tutorial/01..08` (hello → arith → flow → funcs → collections →
types → files → wrap), each with an `EXPECT` block; `tests/run_tutorial.sh`
diffs actual output (8/8 green). Writing the track immediately paid off:

1. **`print(f64)` silently dropped / garbage.** AST path emitted nothing
   (nullptr fallthrough), emit/QBE paths printed double bits as i64. All
   three now route to `flint_println_f64`; runtime unified to `%g` (was `%f`
   vs `%g` inconsistency with `f64_to_string`).
2. **Match-as-expression never worked + hid an invalid-IR miscompile.**
   `emitMatch` returned constant 0 always; wiring PHI values exposed the
   real bug: the switch default targeted the merge block, leaving the PHI
   without a default-edge entry (clang rejects it: "PHINode should have one
   entry..."; O2 folded matches to wrong constants, O0 read garbage).
   Fixed: default → panic backstop (both LLVM paths), PHI over arm values
   with divergence/type checks. Also fixed in passing: enum construction
   zero-initializes payload bytes (undef folding fuel), and per-arm scopes
   (payload binds leaked across sibling arms via declare-no-overwrite).
   Proven: `None→99`, payload math, multi-match sequences, hand-linked .ll
   validates AND runs (5, 99).
3. **`==` on strings compared pointers.** Now content comparison via
   `flint_str_compare` on all paths when both sides are statically strings
   (null-safe: null coerces to ""); everything else keeps pointer semantics
   (the `ptr == 0` idiom is untouched). Verified incl. null==null,
   null=="", malloc-ptr checks.

**`flintc api`:** curated builtin reference (~90 entries, groups + one-liners)
for humans and AI agents (`--format llms|md`, name filter). Keep in sync when
adding builtins.
**Agent benchmark** (`agent-bench/`, 5 tasks + `check.sh` scoring pass/fail +
seconds): validated — references pass 5/5, wrong-output and non-compiling
solutions fail; harness checks exit codes AND exact output. `solution.fl`
files are agent-supplied (never committed); `reference.fl` proves solvability.

---

## v0.22 CONCURRENCY + INTEROP (2026-09-06 session — done, verified)

**Narrative** (README): share-by-communicating — data-parallel loops,
threads + bounded MPMC channels, copy-or-handle crossing, join-before-free,
Aegis-vs-races guidance, parallel-body independence rule.
**Channels** (`runtime/flint_chan.c`, `TypeKind::Chan`): `flint_chan_new`
(cap clamped ≥1, overflow-guarded) / send (blocks when full, -1 when closed)
/ recv (blocks, panics closed+empty — never silent) / close (wakes all,
drains) / len / free. Methods `.send/.recv/.close/.len/.free`; ctor calls
infer `chan` (plus `flint_map_new`→Map, `flint_array_alloc/concat`→array —
same hole, same fix, all three inference sites). Handles cross threads via
`ptr_to_int` + `ch: chan` annotation. Deterministic 3-run proof (10/20/30).
**`parallel for` repaired** (was 100% broken: worker fn stuck in a block →
`undefined var __pfor_N` + LLVM-assert crash): workers hoist via
`pendingFunctions` (drained by parseProgram + QBE reparse); captures correctly
reject (workers are top-level functions); crash-after-error closed with
hadError early-outs in emit loops. Proven with global-array fill (81, 9801).
**`--emit-header`**: C prototypes for every non-generic non-main fn
(scalars direct, composites opaque, heap-string note); proven by a C program
calling Flint `madd`/`greet` (42, hi). **WASM**: `--target
wasm32-unknown-unknown` emits valid WASM objects (readobj-confirmed); full
WASI execution (libc + _start) is the documented next step.

---

## Post-v0.22 Fix (uncommitted): Top-Level Reassignment No-Hang

**Top-level `x = ...` reassignment** (`parseProgram` IDENT+ASSIGN branch,
`src/main.cpp` ~line 1970): now routes to `parseExprStmt()` when the name is
in `declaredVars`/`globalVarNames`, else re-declares as before. Regression
caught by doc verification: `parseExprStmt()` can return nullptr without
consuming (e.g. `parseExpression` rejects assign to non-`declaredVars` names),
which spun the parse loop forever (`mut i` + top-level `while` + `i = 5`
hung; baseline errored cleanly). Fix: `else { parseError(...); advance(); }`
guarantees progress. Verified: hang repro now errors like baseline,
tutorial 8/8, registry 4/4 pass. Known: top-level `while`/`for` remain
rejected ("unrecognized top-level construct") — script control flow must live
in `fn main`; same pre-existing landmine (`if (stmt)` without else-advance)
exists in IDENT+LPAREN top-level path and `parseBlock` — do not touch without
a repro.

---

## Post-v0.22 Fix #2 (uncommitted): Top-Level Call Misparse + Decl Hardening

**Root cause found while verifying README snippets**: the lexer NEVER emits
`NEWLINE` tokens (`ensure()`: `if (c == '\n') { ... continue; }`), so every
`match/check(NEWLINE)` in the parser is dead code AND the 30-token
keywordless-fn lookahead scanned across statement boundaries — any later `{`
or `->` (e.g. `fn main() -> i64 {`) turned a top-level `print("flint")` into a
bare-fn-def parse ("expected parameter name"). Proven pre-existing (baseline
fails identically). Fix (`parseProgram` IDENT+LPAREN branch): paren-depth
aware — find the `)` matching the call's `(` and accept a def only if `{` or
`->` follows it immediately. Also added `else { parseError; advance(); }` to
all five top-level decl branches (LET/VAR-MUT/COLON/COLON_EQ/ASSIGN-redeclare):
a nullptr without consumption re-parsed the same tokens forever (e.g.
`n = name.len()` with unresolvable DOT hung). Verified: t1/m1/m3/m4 +
keywordless `add(a,b){}` + bare `dbl n -> i64{}` all run; smoke 10/10,
tutorial 8/8, registry 4/4, agent-bench refs 5/5; examples 31×exit-0 + 10
non-zero all byte-identical to baseline (7 abort-by-design negatives, 3
value-returning demos). **Known limitation documented in README**: `topStmts`
are parsed but never executed (dead store — no codegen reader), so runnable
code must live in `fn main`; top-level `while`/`for` stay rejected.

---

## FAILURE AUDIT (2026-09-06 — where Flint can still fail, all aspects probed)

P0 (wrong code / hangs / crashes on valid programs):
1. `--fast` SEGFAULTS on heap-string programs (`s = flint_i64_to_string(12345); flint_println(s)` → 139). --fast = O0 passes + O2 backend; `--opt-level 0` runs correctly → IR is invalid in ways O2 passes mask. Systemic: no `verifyModule` gate. README now marks --fast EXPERIMENTAL.
2. `parallel for` + index-assign to outer array (`a[i] = i*i`, the old README example) HANGS the compiler at emit phase (`--emit-llvm` also hangs). Call form errors cleanly ("undefined var"); print-only bodies work. Parallel bodies cannot capture outer vars/globals at all (worker takes just `i`) — README rewritten to verified patterns only.
3. FIXED this session: >i64 literal (e.g. `...808`) SEGFAULTED (nullptr deref `switch (init->kind)` in `parseVarDecl`); now clean "out of range" error.
4. Intermittent cold-start JIT "Symbols not found" (e_parens batch; unreproducible on rerun) — watch item, possible ModuleCache race.

P1 (soundness-adjacent / security / DX hazards):
- `--opt-level 0` runs correctly but exits 1 (wrong status; breaks scripts).
- `topStmts` dead store: top-level statements parse but never execute (documented).
- Lexer emits no NEWLINE tokens: all `match/check(NEWLINE)` are dead code; caused the call/fn-def misparse (fixed via depth-aware lookahead).
- Undefined variables surface at CODEGEN without line numbers ("codegen: undefined var").
- tensor/net/crypto/ai/serial runtimes (~110KB) have ZERO `.fl` usage anywhere — untested surface or dead weight.
- Registry: `flint.lock` pins rev (full SHA) but drift only WARNS and still builds; no signature verification (`--depth 1` clone).
- `flintc test` default passes vacuously (0/0, exit 0); asserts EXIST (`flint_assert(c,msg)`, `assert_eq_i64/f64/str` — verified) but VOID test fns spuriously FAIL (tests must `-> i64` return 0); `tests/` has zero `test_` fns.
- `fmt --check` contradicts itself ("formatted" + "would reformat" same file) and exits 0 on unreadable args.

P2 (claims unverified on this machine / hygiene):
- No LICENSE file (publish blocker). QBE backend needs external `qbe` (exit 127 here — path unverified). Windows/macOS real runs + CI never executed from here. WASM objects valid (`llvm-readobj`: WASM/wasm32) but no WASI execution.Static `strrev` panic (large concat) still open.
Strengths confirmed: OOB/bounds panics clean, overflow safe/unsafe correct, double-close/closed-recv panic cleanly, closures by-value, WASM object valid, JIT startup ~0.1s, QBE/registry attack surface uses arg-vectors (no shell), smoke 10/10 tutorial 8/8 registry 4/4.

---

## 5-LANGUAGE COMPARISON (2026-09-06 — measured on Termux AArch64 unless noted)

Same-workload results (in-program timers; C/C++/Python run on-device, Rust/Java anchored estimates):

| Workload | Flint | C | C++ | Python | Rust/Java |
|---|---|---|---|---|---|
| sum 10M | 17.2ms (15.0 --unsafe) | 8.1ms | 8.0ms | 148ms | ~8ms est. |
| primes 10M | 459ms | 202ms | — | 1968ms | ~200ms est. |
| fib(45) | 11.7s | 8.6s | — | infeasible (hrs) | ~8.6s est. |
| pi 100M | 714ms | 718ms | — | — | ~720ms est. |
| hello binary | 4.3KB | 5.8KB | 46KB | needs ~30MB interp | Rust ~300KB / Java needs ~100MB JVM (est.) |
| cold iteration | 0.52s (0.1s warm) | — | — | 0.09s | rustc secs / javac+JVM slow cold (est.) |

Wins: smallest binaries (beats C!); 4–9× faster than Python on numeric code, ties/beats C on f64 loops; one-binary toolchain (run/build/test/fmt/doc/lsp/fetch/new/api) vs 3–5 tools elsewhere; Aegis (static leases + runtime generational backstop, deterministic free, no GC pauses, no borrow-checker-in-typesystem); `flintc api` + agent-bench (AI-agent UX none of the five ship); 8-lesson runnable tutorial.
Losses (honest): ~1.4–2.3× slower than C/C++/Rust on integer loops; ecosystem ~zero vs crates/pip/maven; P0 audit holes vs their battle-testing; Java peak-JIT + stdlib; Python data ecosystem.

---

## STAGE 0 DONE (2026-09-06 — uncommitted, all gates green)

0a lexer-kit tests: `tests/t_lexkit.fl` (builder, char_at, substring,
compare, map keyword-table incl. missing-key, 1000-push vec) + 2 run.sh
markers → smoke 13/13. Zero compiler changes needed (all primitives exist).
0b composite collections: DEFERRED to Stage 2 with justification — Stage 1
lexer uses parallel primitive arrays (proven above), so no language change
is required yet. Parser symbol tables (string→struct) will force it then.
0c1 `--fast` segfault FIXED: void call as implicit main return emitted
`trunc void` (O2 masked as `undef`/exit-1, O0 crashed). Both main-trunc
sites (`emitReturn`, `parseReturnEmit`) now emit `ret i32 0` for void
values. Verified f7/f9/sum in plain+fast+O0, exit 0, correct output.
0c2 index-assign hang FIXED: `a[i] = v` was unparseable (read parsed, `= v`
left over, block loop spun forever — even for declared arrays, and it was
the parallel-for hang too). `parseExpression` now desugars Array-typed
`a[i] = v` to checked `flint_array_set`; undeclared/non-array/compound
forms are clean consuming errors. Store-OOB panics correctly. p3 now ends
with the (correct, documented-limitation) "undefined var 'a'" capture
error instead of hanging.
Gates: smoke 13/13, tutorial 8/8, registry 0 fail, agent-bench refs 5/5,
41 examples identical to baseline, t_flow extended with index-assign (42).

---

## STAGE 1 LEXER PLAN (Flint lexer, differentially tested — not yet started)

Goal: `stage1/flint_lex.fl` tokenizes any `.fl` file byte-identically to the
C++ lexer, using ONLY Stage-0 primitives (no language changes).
Design (fits v0.22 limits — no struct returns, no composite maps):
- Token stream = 5 parallel i64 vecs: `kinds, starts, lens, lines, cols`
  (lexemes are `flint_str_substring(src, start, start+len)` slices on demand).
- Entry: `lex_into(src, kinds, starts, lens, lines, cols) -> i64`
  (0 ok, 1 + err flag on bad char). Vecs are handles: caller sees pushes.
- Kind codes: small i64 constants for the v1 set (IDENT, NUMBER, STRING,
  END + keywords via a `map{"fn":..,"let":..}` table + single/double-char
  punctuation `.. == != <= >= -> += -= *= /= %= :=`). No NEWLINE tokens —
  matches the C++ lexer (skip `\n`, track line/col).
- v1 scope: identifiers, ints/floats (raw lexemes; numeric parsing is
  Stage 2), strings with `\n \t \r \" \\ \uXXXX \xXX` escapes (mirror
  readString rules), `//` comments, bad-byte errors with position.
- Differential gate (needs one small C++ addition): `--dump-tokens` flag
  (~20 lines, additive) printing `kind:lexeme:line:col` per line; then
  `tests/test_lexdiff.sh` diffs C++ vs Flint output over ALL repo `.fl`
  files (~50: tests + tutorial + benchmarks + examples) plus a tricky-input
  unit file (escapes, `..` vs `.`, `->` vs `-` `>`, `:=` vs `:`).
- Acceptance: zero diffs on corpus; error positions match; no `+` concat
  in loops (builder or substring only — the O(n²) rule).
- Non-goals: parsing, perf parity, keyword-completeness beyond v1 set.

---

## STAGE 1 DONE (2026-09-06 — uncommitted, 82/82 diffs clean)

`stage1/flint_lex.fl` (~750 lines, Stage-0 primitives only) byte-matches
`flintc --dump-tokens` (new additive C++ flag: `kind:lexeme:line:col`, kind
codes + end-position loc + C++ escape processing mirrored, incl. `\x`/`\u`
fallbacks, `//` + `#` comments, no NEWLINE tokens). Harness
`tests/test_lexdiff.sh` diffs all 82 repo `.fl` files (incl. the lexer
itself + `lex_edge.fl` tricky corpus + `lex_err.fl` error corpus):
82 passed, 0 failed. Error token-streams match too; exit codes DELIBERATELY
differ (C++ exits 0 on lex errors, Flint returns 1 — the better API, kept).
Known v1 divergences (invalid input only): trailing `\` at EOF (C++ reads
OOB). Non-goal confirmed: Flint lexer ~37× slower (7.7s vs 0.2s on 200KB /
62K tokens) — reference correctness first. CI: tutorial + lexdiff steps
added to all three jobs (unexecuted from here). Fixes the lexer required:
`and/or/not` keyword codes were missing from the first map (caught by diff).
Docs: README Testing table has the stage1 row; Concurrency section uses
verified patterns only.

---

## STAGE 2 PLAN (Flint parser — text pipeline, no composite values needed)

Strategic finding from Stage 1: if every stage communicates via TEXT, no
stage needs composite collections. Stage 2 (`stage2/flint_parse.fl`) reads
Stage-1 token vecs and emits parenthesized S-expr IR into an sb, e.g.
`(fn main (params) (body (call print (args (num 42)))))`.
Name/type resolution fits string→i64 maps (types as i64 codes; struct names
via a second map) — the deferred composite-collections feature is NOT on
the critical path to self-hosting at all; revisit only for user programs.
- Interface: `parse_into(src, kinds, starts, lens, lines, cols, out_sb)`.
  Mirror C++ desugars at parse (methods, index-assign, parallel-for worker
  split, `and/or/not`) so S-exprs match C++ semantics, not just syntax.
- v1 scope: full expression precedence, calls/index/methods/lambdas, var
  decls + annotations, fns (incl. keywordless/bare forms), if/while/for/
  break/continue, structs/enums/match arms, top-level decls/imports/externs.
- Gates (no C++ changes needed): (1) ROUND-TRIP FIXPOINT — parse, pretty-
  print back to source, re-lex both, token streams must match, over the
  whole corpus; (2) unit file for precedence/nesting/desugar shapes;
  (3) error parity on malformed inputs (positions, no hangs — the audit's
  hang class is the explicit enemy here).
- Non-goals: type checking beyond what parsing needs, perf, error-message
  prose (codes + positions suffice for v1).
- Acceptance: fixpoint clean on all valid corpus files; fuzzer (brace/
  paren soup from the audit) terminates with errors, never hangs/crashes.
Stage 3 preview: S-expr → textual LLVM IR (again primitives-only: scanning
S-exprs is just lexing again), assembled/linked by clang — then bootstrap.

---

## STAGE 2 NOTES (live — parser construction)

- C++ FIX (uncommitted): global `mut` reassign from a non-first fn HUNG
  (parseBlock spun on the nullptr; same class as the top-level hardening).
  Fixed twice: parseBlock now errors+advances (hang insurance for the whole
  class) AND parseExpression accepts globals (feature: `G = 1`, `G += 1`
  work from any fn — verified g5/g8). Root trigger was `declaredVars`
  cleared after each fn while expression-assign only consulted it.
- QUIRK (P1, not fixed): vec-handle (opaque ptr) values can't flow into
  i64-typed var slots (`TK = kinds` → "cannot assign str"). Workaround used
  by Stage 2: handles are never reassigned, only mutated (vec_set/push);
  driver lexes straight into global vecs. Real fix = ptr/vec-aware decl
  inference (later work item).
- Import of a 700-line file with its own main works (V2 probe); same-dir
  and `../` imports resolve relative to the importing file.

---

## 10-AGENT RESEARCH SYNTHESIS (2026-09-07 — all agents completed)

Ran 10 sequential research subagents (internet + codebase). Key outputs:
- [TEXT-IR] Canonical printer; length-prefix in Flint, LLVM `\XX` escapes at
  boundary; deterministic sorted output; golden ladder + manifest + 5-gate
  (emit/diff/llvm-as/clang/run); FileCheck partials. Pitfalls: LEN mismatch,
  nondeterminism, plausible-but-invalid LLVM.
- [ROUND-TRIP] Structural S-expr equality + pairwise nesting + idempotence
  gate `print(parse(print(x)))==print(x)` + grammar-aware fuzz + goldens.
  Pitfalls: over-normalization, positions in equality, discarding invalids.
- [BOOTSTRAP] Phase A/B/C fixed-point + golden ladder manifest + 4 risks
  (invalid LLVM, nondeterminism, runtime linkage, self-subset gap). FIRST:
  hand-write 3 canonical `.ll` through llvm-as+clang (DONE: stage3/).
- [SAFETY] P0s: no verifyModule gate; heap str/map/chan are Copy (UAF);
  raw escapes always on (array_write/unchecked/FFI/link/python); racy shared
  handles; untyped FFI; INT64_MIN/-1 UB. Secure rules: default --safe, no
  unchecked/FFI on untrusted input, single ownership, share-by-channels.
- [PERF] 9 rules: sb not + in loops; hoist lens; sized arrays; checks out of
  inner loops; no format/print in loops; f64 streaming; while-over-array;
  coarse parallel only; JIT-dev/AOT-ship. Compiler opts: concat→builder,
  loop-check hoisting, inline tiny builtins.
- [FIX-11] Exact 5 fixes for the 11 fixpoint failures (all implemented):
  extern defaults, (neg)/(not) nodes, elif consume, variadic marker.
- [ERROR-UX] 7 cheap upgrades (spans, Diagnostic struct, panic-mode+budget,
  did-you-mean, hints, F-codes, multi-error); Stage-2 error contract.
- [STAGE3] Full emitter design: ~25 fns, desugar table, IR templates
  (str=i8*, array={i8*,i64}, sadd.with.overflow, br+phi, main i32 wrapper),
  capability checklist (all present), G0→G6 order, 5 risks.
- [STANDARD] R1-R16 Flint coding standard drafted from evidence (mut, guards,
  loops, errors, state, strings, testing, structure) + pre-commit checklist.
- MASTER PLAN: Phase 1 finish Stage 2 (done: 83/83) → Phase 2 P0 safety →
  Phase 3 Stage-3 readiness. Next: idempotence gate (DONE), canonical .ll
  (DONE), then safety P0s.

---

## STAGE 2 DONE (2026-09-07 — fixpoint 83/83, all suites green)

- Five FIXPOINT-11 fixes implemented + verified: extern params emit trailing
  `-` default (pp no longer prints `= ` for missing defaults); unary minus
  → `(neg X)` and `!` → `(not X)` distinct nodes (pp prints with precedence
  parens); elif path consumes nested if's `)`; variadic `...` emits
  `(... - -)` marker. S-expr spec updated (decl KIND let/mut/var/implicit/
  seteq; neg/not nodes; compound-assign raw; tparams always present).
- Gates: smoke 13/13, tutorial 8/8, registry 0 fail, lexdiff 82/82, parse
  goldens 2/2, parse gate 83/83, fixpoint 83/83 (with idempotence gate:
  pretty(pretty(x))==pretty(x) byte-exact), emit-gate 3/3 (llvm-as + verify
  + clang + run). CI extended (Linux: parse/gate/fixpoint/emit steps).
- Known pp quirk (by design, documented in code): pp's PERR flag can misfire
  on nested shapes while emitting correct text; fixpoint gates on token
  compare + re-parse validity, not the flag (research-backed).
- Stage 3 de-risk DONE: stage3/{00_return_const,01_add,02_if_phi}.ll +
  tests/test_emit.sh (llvm-as, opt -passes=verify, clang, exit-code run).
- OPERATIONAL (2026-09-07): killed flintc runs can poison ~/.cache/flintc
  (later runs wedge: even `timeout -s KILL` doesn't return the shell);
  `rm -rf ~/.cache/flintc` recovers. Device has 3.6GB RAM + swap and
  thrashes under 83× JIT compiles — run long gates detached (nohup) and
  poll; fixpoint harness uses one AOT build + fast per-file runs and is
  the efficient gate. Full fixpoint re-confirmed 83/83 in 3 batches
  (33+41+9, zero fail) after all Stage-2 fixes + idempotence gate.
  Never trust `$?` after a pipe (it's the last command's status);
  harnesses redirect to file first.

## STAGE 3 PLAN (S-expr → LLVM IR text emitter in Flint — research-backed)

Architecture (`stage3/flint_emit.fl`): globals+vecs, return-text (not
streaming) where wrapping is needed, same proven patterns as Stage 1/2
(no short-circuit `and`, mut loop bounds, step budget, sticky PERR).
State: ETMP/ELBL/ESTR counters, EOUT sb handle (never reassigned — the
vec/sb-handle QUIRK), EMAP sym table (name→typecode reusing Stage-2 codes).
~25 fns: emit_prog/form/block/decl/assign/expr/bin/call/index/slice/field/
if/while/for/match/struct/enum/lambda/try/ref/deref/array/map + module fns
(emit_fn incl. fn-decl→declare only, tparams→skip, emit_extern→declare,
emit_import→nothing, emit_python→flint_py_run, emit_main_wrap) + utils
(e_fresh/e_label/e_strname, e_type, e_desugar).
Desugar table (all Stage-2-raw nodes): compound-assign→assign+bin;
idx-set→bounds-check+GEP store (or flint_array_set); method→runtime call
table; for/range→while with dedicated incrBB (continue-safe); parallel-for→
outline __pfor_N + flint_parallel_for call (captures rejected); and/or/not
already tokens (`!e`→`e==0`, `&&`→CreateAnd on truthy, no short-circuit —
matches C++); try/unwrap→null/err check + panicBB; struct→LLVM struct type +
alloca/GEP; enum→{i64 tag, payload} + switch + default-panic backstop
(never mergeBB — the v0.21 PHI class); lambda→closure-convert by-value
(__lam_F_N, direct calls only); destruct→field/index decls; slice→ptr+len.
IR templates: str=i8*, array={i8*,i64}, bool=i1 (zext at calls), f64=double;
i64 `+` via llvm.sadd.with.overflow + extract + condbr + panicBB; div/mod via
icmp-zero guard + sdiv/srem; f64 via fadd/fsub/fmul/fdiv (sitofp promotion);
if/while with terminator-BB tracking + phi in merge; main wrapper
`define i32 @main(i64,ptr)` storing argv globals + trunc result (ret 0 for
void — Stage-0 0c1 rule); prelude of `declare`s only (bodies from runtime.*).
Capability checklist: file write (flint_file_write Y), sb building Y,
int counters Y, str→i64 maps Y, vec handles Y, string ops Y, runtime calls Y
— no new language features needed. Order: G0 scaffolding (prog+prelude+main+
return/num/str/var/call-print) → G1 decl/assign/bin/if/while → G2 fns/calls/
extern/main-trunc → G3 arrays/index/strings/bounds → G4 for/desugars →
G5 struct/enum/match → G6 lambda/try/method/parallel/map/python.
Validation: golden `.ll` ladder + manifest (weavec0 5-gate: emit/diff/
llvm-as+verify/clang/run) + Phase A/B/C (cross-compile, self-compile
byte-identical, fixed point) + `--self-hosted` flag at the end.
De-risk DONE: stage3/{00_return_const,01_add,02_if_phi}.ll + tests/test_emit.sh
3/3 green via llvm-as + `opt -passes=verify` + clang + exit-code run.
Top risks: no verifyModule (always gate with llvm-as); PHI incoming-BB bugs
(track terminators); string-global numbering (normalize before diff); str/ptr
type conflation (positional inference); hang class (step budget everywhere).

## STAGE 3 G0 DONE (2026-09-07 — emitter end-to-end green)

- `stage3/flint_emit.fl` (~900 lines): S-expr cursor + return-text emitters
  (ETYPE side-channel like pp's PPPREC; deterministic `%tN`/`@.sN` names;
  scope-stamped sym maps), prelude (argv globals + println declares),
  main wrapper (argv stores + trunc return), alloca locals, string globals
  at module top (EGLOB buffer), LLVM string escapes (`\22` never `\"`,
  per LangRef research). G0 scope: prog/main/decl/assign/return/call-print/
  num/str/var. Driver: `flint_emit prog.sexp out.ll`, exit codes.
- Golden ladder green: stage3/ladder (manifest + g0_hello/g0_vars .sexp +
  reviewed .ll) via tests/test_emit_stage3.sh (emit/diff/llvm-as/verify/
  clang+runtime+lm/run+exit) 2/2; tests/test_emit.sh (hand canonical IR)
  3/3. First Flint-compiled binaries run correctly (hi/41/2.5, exit 0).
- Flint-language lessons applied: no-op self-assigns fail compile; bare
  `}` fine but `{` needs `{{` in literals; expression emitters must own
  their closing paren (no double-consume with statement tails); vec/sb
  handles never reassigned.

---

## STAGE 3 G1 DONE (2026-09-08 — binops/if/while + overflow/divzero panics)

- Added to `stage3/flint_emit.fl`: full `(bin OP)` (i64 via llvm.sadd/ssub/
  smul.with.overflow + arith_panic/ok diamond, sdiv/srem + divzero/modzero
  guard diamond, `and/or`→and/or, f64 arith/fcmp/uitofp/sitofp/fptosi mix,
  icmp+fcmp+bool, str concat runtime call; int result via pre-trunc i1
  text then zext so `(print (1 < 2))` prints 1), if-stmt (else/elif chains,
  no-else falls to merge, both-terminate sets ETERM, phrase-bound
  `then_term/else_term` vars — NOT globals — so nesting works),
  if-expression (`(decl ... (if ...))` + `(block EXPR)` arms + phi with
  ECUR-captured incoming blocks — NOT assumed labels, since arm exprs may
  contain binop diamonds), while (cond/body/end labels, EBREAK/ECONT
  save/restore stack vars + ETERM/ECUR discipline so break/continue/if
  nest), e_cond_emit takes TYPECODE (1=i64→icmp, 2=f64→fcmp — never the
  is-float boolean), prelude + `declare void @flint_panic(ptr)`.
- Ladder 5/5 via tests/test_emit_stage3.sh: g1_arith (42/37/74/24/0/1/5),
  g1_if (1/200), g1_while (3 = 1+2 skip-3 break-4) + regen g0 goldens
  (prelude declare). Panic paths verified live: INT64_MAX+1 → "PANIC:
  integer overflow" abort 134; 1/0 → "PANIC: integer division by zero".
- Debugging lessons (add to coding standard): (a) typecode-vs-boolean
  confusion (e_cond_emit got cc=1=i64 but read it as is-float → fcmp on
  i64); (b) missing-else must be checked AFTER then-block, not after
  cond (peek after cond always sees `(`); (c) phi incoming blocks must
  be ECUR-captured (binop diamonds move the terminator); (d) bisect
  with hand-written minimal .sexp (w0/wb/wc/wi series) when silent EERR;
  (e) `-lm` needed for runtime link (pow).

---

## STAGE 3 G2 DONE (2026-09-08 — multi-fn, calls, defaults, extern, returns)

- `stage3/flint_emit.fl` G2: two-pass (e_scan_prog registers name→idx,
  param count/bases/varargs/ret + default S-expr TEXTS framed in ESIGTEXT
  with EFPOFF/EFPLEN; e_prog emits). General `(call f args)` (positional,
  defaults sub-emitted in caller scope via ESRC/EC save/restore,
  i64→f64 sitofp promotion, variadic passthrough, arity/type errors);
  general `(fn)` (any ret, params→alloca binding via param-text rewind,
  main keeps argv wrapper + i32 trunc); `(extern "C")`/`(fn-decl)` →
  `declare`; `(return)` generalized (bare→ret-zero/void, valued→exact or
  promote); trailing-`(expr X)` implicit return via EBLOCKEXPR/VAL/CODE
  (e_stmt sets, e_if_stmt/e_while DEFINE statement values: if/else with
  same-typed arm-tail exprs merges via phi like if-expr, else/while yield
  nothing); C-escape unescaping in e_str (`\n\t\r\\\"\xXX\uXXXX`,
  unknown stays literal — mirrors C++ lex; array size uses UNESCAPED len).
- Ladder 9→7? no: 7/7 (g2_calls: add/fact/fib 42/120/55, defaults
  106/103/6, promotion 3.5/4; g2_extern: puts/printf `\n\t` escapes).
- COMPILER BUGS FOUND (Flint-on-Flint landmines, C++ codegen):
  (a) `if CALL() != lit` mis-evaluates (e.g. `e_word() != "prog"` fails
  while `w=e_word(); w != "prog"` works; `==` with calls is FINE).
  RULE: bind call results to locals before ANY `!=` (also before `==`
  on strings for safety). Applies to globals? `EBLOCKCODE != 0` worked.
  (b) DUPLICATE fn definitions with different param types (`e_llvm_type
  (ty: str)` G0 + `(code: i64)` G2) cause chaos/segfault (int passed to
  str version → str-compare on address 0x1). RULE: never reuse a fn name
  with a different signature — rename (e_llvm_typecode). No overloading.
  (c) Segfaults lose buffered print() output — debug with
  flint_write_file markers (unbuffered), not prints.
  (d) Bare `extern fn` is INVALID Flint (C++ only takes `extern "C"`
  blocks) — the stage2 parse error was correct, not a bug.
  (e) Shell `printf` mangles `%` in test sources — use Write tool.
  (f) S-expr `(str LEN RAW)` keeps escapes RAW (LEN counts raw chars);
  C++ unescapes at lex, so the EMITTER must unescape (done in e_str).
  (g) Variadic i64 passes through as i64 (fine for %d low-32 on LE).

---

## STAGE 3 G3 DONE (2026-09-09 — arrays/index/bounds, strings, concat)

- New typecode 4 = array `{ptr,i64}` (mirrors C++ FlintArray layout).
  `(array E*)` two-pass (string-aware count-skip via e_skip_value, rewind,
  emit+store; i64 elems only); `(index A I)` (array→GEP+load, str→GEP+load
  i8+zext byte — C++ reference CRASHES compiling `s[1]`, mine works);
  `(idx-set A I V)` (arrays mutable, strings immutable error); `(call len)`
  extended (array→extractvalue, str→flint_str_length); `+` concat
  (str→flint_str_concat, array→flint_array_concat); e_decl/e_assign/e_var_rd
  code-4 branches; `bool` annotation ≡ i64; C-escape unescape already in G2.
- Bounds: every index/idx-set emits flint_bounds_check (panics like C++).
- Ladder 9/9 (g3_arrays: params/index/idx-set/len/str-concat; g3_stridx:
  byte-index/concat/empty/sum-loop). OOB verified live (abort 134).
- STAGE2 FIX (pre-existing bug, found via G3): fn param scope ids reused
  (PSCOPE +1/-1) so `fn first(a) ... fn main ... a = ...` mis-emitted
  `assign` instead of `decl` in the SECOND fn. Fix: monotonic scope ids
  (removed 6 `PSCOPE = saved - 1` restores). Parse-gate still 83/83.
- STAGE2 ADDITIVE: `[T]` array types in p_type_token (needed for `[i64]`
  params; no corpus file uses them → zero golden risk).

---

## STAGE 3 G4 DONE (2026-09-09 — for/range/array/str desugar)

- `(for V ITER BODY)`: `(range A B)` → counter loop (bounds evaluated once,
  hidden end alloca, checked +1 step with overflow panic, `<` cond);
  array/str/var → index loop (hidden idx+len allocas, elem declared per
  iteration, plain +1 step). break/continue via EBREAK/ECONT like while;
  for is a no-value statement (EBLOCK zeros). Loop var is a normal
  versioned local (two `for i` loops coexist).
- Versioned locals (needed for shadowing): EVERSION map; ver 0 keeps
  `%nm.addr` (golden-stable), ver ≥1 `%nm.addr.vN`. e_declare_var helper;
  e_decl/e_assign/e_var_rd/param-bind all route through it. 3 goldens
  regen'd (only `%n.addr.v1`-style renames).
- Ladder 10/10 (g4_for: range-sum/array/str-bytes/break-continue = 10/
  10/20/30/97/98/12).

---

## STAGE 3 G5 DONE (2026-09-09 — structs, enums, match)

- Named types share codes 100+ (ENTMAP/ENTKind/ENTN/ENTBase + flat field
  vecs + ENTTEXT bodies). `(struct-lit)` any order, all-required, dup
  detection (EMATCHCOV scratch), int→float field promotion, nested
  structs; `(field)` extract; `(enum-lit)` tag-only → i64 index (payloads
  loud G7 error); `(match)` if/else-check chain (one-step lookahead, no
  pads), exhaustiveness = compile error (mirrors C++), panic backstop
  (`unreachable` after noreturn panic), value arms merge via hidden-slot…
  NO — via scratch-stored phi entries (shared slot broke dominance:
  alloca in one arm used by sibling = invalid IR). All-fallthrough +
  same-type required; ETERM when all arms diverge.
- If-STMT also yields arm-tail phi (Flint if/else is an expression):
  statement-position `if` with same-typed arm tails merges; while/for yield
  nothing. (Fixed fact/fib returning 0: stage2 wraps braced arms as full
  blocks, so trailing-`if` needs the merge.)
- e_decl/e_assign/e_var_rd pass codes 5+ through (were demoting to ptr).
  e_llvm_typecode/e_llvm_zero extended (structs, zeroinitializer).
- Ladder 12/12 (g5_struct incl. nested/out-of-order/param-passing;
  g5_match; non-exhaustive correctly rejected).
- COMPILER BUGS (more landmines): (h) duplicate `fn` names with different
  signatures segfault (int passed to str-version of e_llvm_type → strcmp
  on address 0x1) — never reuse a name (e_llvm_typecode); (i) segfaults
  lose buffered prints — debug with flint_write_file markers; (j) empty
  LLVM blocks (bare label fallthrough) and sibling-block allocas are
  invalid IR — always terminate + dominate; (k) `mut` inside while bodies
  unproven — hoist; (l) multi-branch implicit assign illegal (ismain) —
  `mut` + single pattern or proven e_form-style mut.

---

## STAGE 3 G6a DONE (2026-09-09 — self-hosting capability: maps, runtime,
## globals, conversions, imports)

- Codes 6 = map (str→i64, ptr), 7 = opaque handle (vec/sb). `(map-lit)`
  via flint_map_new/set; `(method m has/get/set)`; `(call len)` on maps.
- Runtime passthrough: e_rt_sig table (~40 fns, "R:P..." digit codes) +
  e_rt_call (strict check + auto-declare-once to module top) + e_call
  fallback (registry → runtime → loud error). Unknown fns still loud.
- Conversions e_conv (exact, i64↔f64, i64↔ptr-family inttoptr/ptrtoint,
  ptr-family interop): wired into call args, defaults, runtime args.
  Untyped `-` params → i64 (mirrors C++ default).
- Top-level globals: scan records (EGNAME/EGCODE + init texts in EGTEXT;
  e_infer_init textual inference for `-` types); emit `@G = global T
  zeroinitializer` + init stores in MAIN prologue (in order); var/assign
  route `@G` via EGNAME. Mutable cross-fn globals verified live.
- Merged-import support: duplicate main → first wins (scan skip +
  emit skip, mirrors C++ merge order); driver merges S-exprs (drops
  `(import)` nodes, main file first).
- Latent bugs fixed: e_form/e_scan_form import-skip (e_skip_balanced_fn
  on `"path"` errored — imports never ladder-covered); e_infer_init used
  e_skip_balanced MID-FORM (desync — new e_skip_rest).
- Ladder 14/14 (+g6_globals, +g6_maps). e supports lambda/try/unwrap/
  slice/spread/python/int-patterns LOUDLY (later-group/G7 errors).
- Scope boundary G7 (NOT needed for bootstrap — stage sources use none):
  lambda/closures, try/Result, slices/spreads, payload enums, maps with
  non-i64 values, float arrays, methods beyond map has/get/set.

---

## STAGE 3 G6b DONE (2026-09-09 — entry-hoist pre-pass, string-aware skips,
## {{ }} escapes; BOOTSTRAP PHASE A/B/C GREEN)

- Entry-hoisted allocas (LLVM dominance): C++ rejects shadowing, so one
  slot per (function, name) is sound. e_pre_* mirrors emit traversal
  (params from registry names stored at scan; decls via e_infer_init;
  if/while/for/match arms incl. single-form match bodies); versions bump
  identically so emit (store-only e_declare_var) always agrees. Pre-pass
  consumes NO counters (deterministic output preserved).
- Total inference (e_infer_init + e_infer_if/match/block_tail): statement
  tags → 0, `(expr X)` → inner, if/match → arm agreement (mirrors emit's
  phi rules exactly — disagreement means emit yields void too).
- String-aware skips (THE self-host bug): e_at_str/e_skip_str/e_skip_depth;
  e_skip_balanced/e_skip_balanced_fn/e_skip_rest all fixed. Naive paren
  counting desyncs on paren-containing string literals (e.g. `(str 1 ())`
  phantom +1). Found via unbuffered file-marker tracing (segfaults eat
  buffered prints; traced pre-pass positions offline against the sexp).
  Related catch: e_at_str must rewind to PRE-skip pos (ate whitespace).
- `{{`→`{`, `}}`→`}` in e_unescape (Phase B caught it: emit.1 emitted
  literal `{{`). Byte-parity with C++ verified on all escapes.
- Duplicate-`declare` guard: prelude fns pre-registered in ERTDECL.
- BOOTSTRAP (tests/test_selfhost.sh): Phase A lex.0/parse.0/emit.0 built;
  parse.0 output matches JIT; Phase B emit.0 self-compiled merged
  (118 fns + 58 globals + lex) → emit_self.ll verifies → emit.1 linked;
  emit.1 smoke byte-identical; Phase C emit.1 recompile byte-identical
  (FIXED POINT). emit.1 passes full ladder 15/15 independently.
- New landmines: (m) entry allocas mandatory (mid-block alloca + loop/branch
  use = invalid IR); (n) `mut` inside while bodies unproven — hoist;
  (o) string-aware skipping everywhere parens are counted; (p) always
  re-verify AOT artifacts after source changes (stale binaries mislead).

---

## STAGE 4 PLAN (bootstrap fixed point — research-backed)

Goal: Flint compiling Flint end-to-end (lexer+parser+emitter in Flint),
validated by Phase A/B/C. Prerequisites (all met): Stage 1 lexer byte-
identical ✓, Stage 2 parser+fixpoint 83/83 ✓, Stage 3 emitter G0 green with
golden ladder ✓, runtime ABI proven (Flint-emitted binaries link+run) ✓.
- G1–G6 emitter groups in order: decl/assign/bin/if/while (overflow+divzero
  goldens) → fns/calls/extern/main-trunc → arrays/index/strings/bounds →
  for/desugars → struct/enum/match (exhaustiveness + default-panic) →
  lambda/try/method/parallel/map/python. Each group: extend flint_emit.fl,
  add manifest entries + goldens, 5-gate green before next group.
- Then driver unification: single `flintc.fl` pipeline (lex→parse→emit→
  clang) mirroring C++ flags needed for self-build (`--emit-llvm`, `-o`).
- Phase A (cross-compile): C++ flintc builds Flint-pipeline binaries
  (emit.0). Pass: llvm-as clean + runs.
- Phase B (self-compile): emit.0 compiles the pipeline sources; output .ll
  byte-identical to Phase A (normalize `@.sN` numbering + paths first).
  Any divergence = bug in emit.0. Full corpus recompiled under both.
- Phase C (fixed point): emit.1 recompiles → emit.2 == emit.1 byte-identical.
  Promote emit.1 as `flintc-stable`; add `--self-hosted` flag routing
  through it; keep C++ ref as fallback.
- Rules throughout: deterministic output (sorted symbols, named values,
  no timestamps/paths); DDC spot-check with independent second emitter;
  manifest pins LLVM/clang versions + RUNTIME_ABI; never trust C++ ref
  blindly long-term.
- Known Stage-3 emitter gaps to close on the way: implicit tail returns,
  non-main functions, all desugars (compound/idx-set/methods/for/match/
  lambda), f64/bool/str annotations beyond G0, extern `...`, top-level
  non-fn forms. Each is a ladder entry, not a redesign.

---

## BENCHMARKS (2026-09-10 — self-hosted vs C++, Termux AArch64, wall clock)

| Input | C++ flintc | Self (parse.0 + emit.0) |
| tiny (93 B) | ~85 ms | ~40 ms |
| small (~600 B) | ~95 ms | ~40 ms |
| flint_lex.fl (48 KB) | 353 ms | 1,624 ms (241 parse + 1,383 emit) |
| JIT dev-loop | — | ~2.4 s startup-dominated |

- Verdict: self-hosting does not inherently speed up a compiler. Small files:
  thin passes win ~2x. Real files: C++ 4.6x faster; bottleneck is the naive
  emitter (S-expr re-parse per node, linear scans, O(n^2) appends, 2 passes).
- Correctness inversion: C++ segfaults for-array, rejects len(), mangles
  methods (fa_len), crashes s[1] — all correct under self-hosted. See P0 log.
- Industry levers (researched): PGO ~20%, PGO+ThinLTO+BOLT+alloc ~1.5-1.75x,
  mimalloc/jemalloc 5-8%, static link cuts startup, content-hash cache daemon
  ~1 ms/hit. Frontend parsing dominates; backend matters less.

## ROADMAP (P0-P4 — production-grade, researched 2026-09-10)

- P0 parity/divergence [DONE 2026-09-10]: COMPATIBILITY.md (D1-D4) +
  tests/test_differential.sh (5/5 green) + tests/differential/ corpus.
  Zero unlisted divergences enforced by harness.
- P1 perf [P1.0 DONE; P1.1 DONE 2026-09-13 — TARGET MET]:
  bench/compile_time/bench.sh + BASELINE.md exist. P1.1 cut per
  instrumented data (flint_time_ns phase split: scan 637 ms + emit
  897 ms on 28 KB S-expr; ~100K e_skip + 11.5K e_word calls): bulk C
  scanners flint_str_skip_ws/flint_str_word_end (src/main.cpp
  registration + e_rt_sig 1:3,1; e_skip/e_word rewritten,
  byte-identical output proven) + long-string strlen memo in
  runtime.c (8-entry, >= 256 B; 1-entry measured ~0% hit rate under
  interleaved map-key strlens). Lex total 1,687 → 349 ms (emit
  1,411 → 35 ms, 40x), under the 400 ms parity gate. Parse unchanged
  (stage1/2 readers are separate code — follow-up if gate tightens).
  Ratios in bench/compile_time/perf_gate.py (+10% CI rule).
- P2 stage-4 tail [DONE 2026-09-10]: driver/flintc.fl (single pipeline
- P2 stage-4 tail [DONE 2026-09-10]: driver/flintc.fl (single pipeline
  driver: parse->[merge]->emit->clang; --emit-llvm/--no-run/--self-hosted;
  tool discovery via argv0 siblings; cycle-safe import collection),
  tools/merge_sexp.fl (string-aware, byte-identical to reference merger;
  tests/test_merge.sh 5/5), tests/test_driver.sh 6/6 (single/multi/
  emit-llvm/errors/self-hosted), driver/VERSIONS pin (LLVM 21.1.8 +
  RUNTIME_ABI contract), test_selfhost.sh `stable` phase (re-verify +
  install emit.1/parse.0). DDC = test_differential.sh (documented).
  New landmines: (q) `cmd | head; $?` measures head — capture real status;
  (r) loop-exit flags (`while ok` + `else i=n+1` spins forever — go flags);
  (s) never `rm *.bin` in tool dirs; background runs need timeouts.
  rt additions: flint_command/exit/exists (getenv/command_output skipped:
  NULL hazards).
- P3 G7: lambdas [P3e DONE 2026-09-12], try/Result, slices, payload enums,
  non-i64 maps, methods. Lambda design: outline to `lam.N` top-level fns
  (users-then-caps param order everywhere: registry/synthesis/signature/
  call), captures frozen into `%lamcap.N.C` entry slots, same-function-only
  calls, handle slots typed ptr (infer→8), write/escape/unknown rejected.
  Ladder g7e_lambda + g7e_recursion (21/21 green). Landmines: (t) if-arm
  tails must share one type or flintc dies in LLVM PHI assert (end value
  fns with `1`); (u) drain binds params BY POSITION — synthesis order must
  equal registry order or types swap silently (caught only by non-i64
  cases); (v) ELAMN increments on success path only or failed outlines
  poison the drain; (w) python splice anchors must be unique — a dup anchor
  ate e_decl_lambda's tail (rebuilt verbatim, ladder re-greened).
  tests/test_decl_order.py wired into v1_gate_check.sh (decl-order gate).
- P4 production [DONE 2026-09-13]: CHANGELOG/VERSION/signed tags/SBOM/tier-1 targets,
  CI matrix + fmt gate + nightly fuzzer, error-code docs, semver freeze,
  v1_gate_check.sh (15/15 green --full: parse/emit/smoke/merge/registry/
  differential/driver/decl-order/fmt/errors + parse-gate/fixpoint/lexdiff/
  ladder-21/tutorial). Shipped: flint-fmt --check + tests/test_fmt.sh
  (found+fixed: quote-stripping, single-char-string eaten as syntax,
  `}else{` split breaking expr-position parse), docs/errors.md 116/116
  + tests/test_errors.sh, tests/test_decl_order.py in gate, nightly job
  (selfhost A + bench + perf_gate.py ratio +10% + fuzz 100 seeds + SBOM
  artifact), tools/sbom.py, docs/RELEASE.md, tests/test_fuzz_corpus.sh.
  Skipped deliberately: C++ modules/PCH, G7-before-perf.
- P4 FIXES found during revalidation (2026-09-13, both self-host blockers):
  (x) e_infer_init str branch + e_skip_balanced used char-depth skips
  mid-string: raw parens in S-expr string content (`"("` decl inits from
  P3e) misaligned the cursor → silent EERR. Fix: e_skip_str_tail exact
  consume (mirrors e_skip_value). (y) merger splitters (test_selfhost.sh
  python + test_merge.sh reference) ws-skipped around string LEN: fused
  forms + dropped closers on leading-space content (244→125 forms).
  Fix: exact skip (mirrors tools/merge_sexp.fl, which was already exact)
  + adversarial merge fixture (6/6). Note: pre-P3e Phase B green went red
  silently — re-run bootstrap after every emitter change.
- P4 landmines: (t) if-arm tails must share one type (LLVM PHI assert);
  (u) drain binds params BY POSITION (synthesis order == registry order);
  (v) ELAMN success-path-only increment; (w) unique splice anchors.

## SCORE IMPROVEMENT PLAN (2026-09-12 — portability, tooling, maturity)

Scored 2026-09-12 vs C/C++/Rust/Go/Python (measured runtime+compile data
in README Benchmarks; Rust/Go columns estimated): Tooling 7, Portability 6,
Maturity 3. Research below is repo facts + industry precedent; actions are
ordered by leverage (maturity gates everything else).

### Portability 6 → 8

Repo facts: REQUIREMENTS.md already carries a tier table (T1 Android ARM64
+ Linux x86_64/ARM64; T2 macOS; T3 MinGW/MSVC). CI (.github/workflows/
flint.yml) runs full suite on T1 (ubuntu-24.04 + -arm), builds on T2
(macos-15), tests-on-MinGW on T3 (windows-2025). v0.20 proved MinGW
hello.exe cross-linked; MSVC still needs the thread shim (PORT notes in
code). WASM emits objects only — no `_start`/libc, nothing executes.
bench/compile_time/bench.sh + SELF_DIR hardcode /data/data/... paths
($TMPDIR ignored), so the same scripts fail off-Termux.
driver/VERSIONS pins LLVM 21.1.8 + RUNTIME_ABI (good — tiers need pins).

Industry precedent — Rust target-tier policy
(doc.rust-lang.org/rustc/target-tier-policy, platform-support):
Tier 1 = "guaranteed to work" (CI builds AND passes tests, official
binaries, host tools run natively, full std, designated maintainers);
Tier 2 = "guaranteed to build" (CI builds, tests optional); Tier 3 =
may-or-may-not-build, no CI. Promotion requires: no stubbed-out std,
cross-compilable without the target as host, target-specific docs.

Actions:
1. macOS T2→T1: run the FULL gate list there (smoke/tutorial/lexdiff/
   parse/emit/differential), not just build. Record green in README.
2. MinGW T3→T2: confirm native CI is fully green, publish result; keep
   MSVC at T3 until the thread shim lands, then promote on the same rule.
3. WASM T3→T2: implement `_start` + libc linkage → one WASI hello
   executes end-to-end; add `--target` execution tests per triple.
4. Replace hardcoded tmp paths with $TMPDIR (bench.sh, SELF_DIR default)
   — portability bugs in the harness count against the score too.
5. Per-target docs page (Rust requires it per tier): what works, what is
   stubbed (e.g. `flint_regex_*` on Windows), exact install commands.

### Tooling 7 → 9

Repo facts: one entry point (`run/build/test/fmt/doc/lsp/fetch/new/api/
help`) + Python sidecars (flint-lsp 19 KB, flint-fmt 6.6 KB, flint-doc
8 KB). No editor extension; no DWARF (gdb/lldb can't map Flint sources);
flintc_prof emits JSON but there is no one-command flamegraph; registry
is git-URLs + flint.toml/lock with no index or version resolution;
README benchmark tables are updated by hand (2026-09-12 session did it
manually — automatable).

Industry precedent — gopls (go.dev/gopls, official Go language server):
maintained by the language team, not the community; standard LSP feature
set (navigation, completion, diagnostics, analysis, refactoring);
editors auto-install it (zero setup); semver releases; supports the last
2 major language versions; per-editor setup docs.

Actions:
1. LSP to gopls parity path: hover/goto-def/diagnostics backed by the
   real stage-2 parser (not regexes); ship a VS Code extension that
   bundles it; per-editor docs (VS Code, Neovim, Emacs, Zed).
2. DWARF line tables in AOT output → source-level gdb/lldb debugging.
   Debuggers are tooling users grade but rarely request.
3. Registry v2: central index + semver resolution + offline mirror;
   keep git-URL fallback. 3–5 seed packages (http/json/cli/test) prove it.
4. `flintc_prof` JSON → `flintc profile --flamegraph` one-command output.
5. CI gates: `fmt --check` blocking + fmt idempotency tests; benchmark CI
   regenerating the README tables every release (numbers that update
   themselves get trusted).

### Maturity 3 → 7

Repo facts: open P0s — COMPATIBILITY.md D1 (for-array segfault), D4
(`s[1]` compiler crash), `strrev` SIGABRT on large inputs, `fa_set`/
`fa_len` method mangling; `stage3/flint_emit.fl` (235 KB) uncompilable by
either backend. Differential harness + corpus exist
(tests/test_differential.sh 5/5); fuzz/ has generate.py/run.sh but is NOT
wired to CI; P4 ship-list (signed tags, SBOM, semver freeze,
v1_gate_check.sh, error-code docs) is planned, not done; CHANGELOG
(Keep a Changelog + semver) started at 0.22.0 — keep the discipline.

Industry precedent — Csmith (PLDI'13, Utah) + libFuzzer (LLVM docs):
generate UB-free random programs; compile each at -O0..-O3; crash bugs
= nonzero compiler exit; wrong-code bugs = differential oracle (outputs
of reference compilers at lowest opt must agree); coverage-guided corpus
checked in doubles as the regression suite; fuzzers run indefinitely /
nightly, corpus replays as a blocking test.

Actions (in this order — each unlocks the next):
1. Zero-known-crashes: D1/D4/strrev/fa_set first. Every fix adds a
   differential corpus entry (existing convention — keep it). Nothing
   raises maturity while the compiler segfaults on valid programs.
2. Nightly fuzz in CI: Flint program generator (Csmith-style, UB-free
   subset) × {--opt-level 0..3, --fast, --unsafe} × {C++ ref vs
   self-hosted} with output-divergence as oracle; check the corpus in,
   replay it as a blocking gate (libFuzzer corpus-as-regression pattern).
3. Per-PR blocking gates: test_differential.sh + BASELINE.md +10% perf
   rule + fmt --check. P4 acceptance already defined this — enforce it.
4. Ship hardening: signed tags, SBOM, error-code docs, semver freeze +
   v1_gate_check.sh (P4 list, unchanged).
5. Semver + deprecation policy published: ecosystems invest only when
   APIs survive. CHANGELOG discipline is the proof.

Order of battle: maturity P0 fixes → CI gates (differential/perf/fuzz/
fmt) → portability tier promotions → tooling depth (LSP/DWARF/registry).
Maturity unlocks contributors; contributors build the ecosystem, which is
the other half of the 3.

---

## MASTER SPEED/SAFETY/PORTABILITY PLAN (2026-09-13 — audited + researched)

Measured baselines (Termux AArch64, AOT, in-program ns timers, same session):
compile lex-size self-total 349 ms (C++ 280); runtime sum_array 17.6 vs
C 7.9 ms (2.2x), primes 451 vs ~190 (2.4x), pi 746 vs 719 (1.04x),
fib(45) 11.66 vs 8.54 s (1.36x), raw i64 loop 520 vs 42 ms (12.4x);
self-emitter output 1.3–1.9x slower than C++-emitter output, same source.
`--fast` = O0 dev tier: 4.3x faster builds (145 vs 626 ms), 11–28%
slower runs. strrev still PANICs (null string).

Repo facts constraining every proposal below: LLVM PassBuilder O0–O3
(default O2); QBE backend already tried and FAILED (3x slower than
LLVM-O0: subprocess spawn + IL bloat — retained experimental only);
CGU parallel backend exists (−40% large AOT wall); mold preferred;
overflow = sadd/ssub/smul.with.overflow + panic branch (skipped only by
`--unsafe`); bounds = opaque `flint_bounds_check` CALL per index
(separate TU, no LTO); strings are bare `char*` (every length/char op
is `strlen` — mitigated 2026-09-13 by bulk scanners + long-string memo);
concat is malloc+memcpy per `+` (O(n²) loop concat); no PGO/ThinLTO/BOLT;
no DWARF; WASM emits objects only; MSVC blocked on thread shim.

Research basis (all verified 2026-09-13, numbers quoted are theirs):
- Go BCE: SSA `prove` pass (fact tables, range constraints), debug flag
  `-d=ssa/check_bce/debug=1` listing surviving checks; wins 10–25% on
  tiny CPU-bound loops; explicitly peephole (no vectorization unlocked
  by itself). Lesson: compiler-owned BCE + user-visible diagnostics.
- Rust overflow: checked in dev, WRAP in release by default (RFC 560);
  costs are extra insns PLUS inhibited unrolling/vectorization PLUS
  panic conservatism; range analysis can remove some.
- mimalloc paper (MSR-TR-2019-18): 7%/14% on redis vs tcmalloc/jemalloc,
  Lean 8–13%, larson 2.5x, ~3500 LOC, MIT, Linux/FreeBSD/macOS/Windows.
- Clang self-build studies: PGO 20–49% (sqlite-compile; LLVM docs claim
  ~20%), ThinLTO ~7–10%, BOLT +0.15–0.20x, jemalloc single biggest jump
  in one study; Full LTO ≈ ThinLTO at higher cost.
- CGO'24 (Engelke): Cranelift compiles only 20–35% faster than LLVM at
  similar-to-unoptimized-LLVM runtime; single-pass 16x faster than
  Cranelift. Rust cg_clif: −21% wall, −40% CPU-s. Lesson: library
  backends give modest compile wins, consistent with our QBE lesson.
- LLVM docs: loop vectorizer bails on unvectorizable calls; only
  whitelisted/math-intrinsic calls vectorize. Our opaque check-calls
  therefore block vectorization AND inlining visibility by construction.
- rustc-dev-guide: incremental red-green + fingerprints; fingerprinting
  is costly and "the main reason incremental can be slower than clean".
  Lesson: file-level caching (ccache-style) before any query DAG.
- ARM MTE (USENIX'26/arXiv'26): geomean 1.02–1.10x on server silicon
  but worst-case cliffs to 6.64x + kernel-tagging traps (memcached
  −25.8% pre-fix). Verdict: hardware-dependent watch-item, NOT a
  near-term software strategy.

### RUNTIME SPEED (goal: numeric parity, indexed ≤1.5x C, keep checks on)

- R1 inline bounds check (icmp+br inline, not CALL) [DONE 2026-09-13]:
  C++ JIT (`emitIndexAccess`) + AOT (`emitIndexEmit`) + self emitter
  (`e_bounds_inline`, 3 sites, ECUR discipline for PHI honesty) emit
  `icmp ult` + br; cold block calls `flint_bounds_check` (messages
  byte-identical, negatives trapped via unsigned wrap). O2 deletes the
  check on provable shapes (measured: `a[i % 8]` loop has zero check
  insns). Micro-loop wall deltas within throttle noise (honest: the win
  is structural — unlocks R2/vectorizer — not a headline number yet).
  Ladder 21/21 (3 goldens re-blessed), bootstrap A/B/C green.
  Measured 2026-09-13 (cooled, clean A/B old-vs-new flintc, 1.6M `a[i]`
  accesses): call-shape 29.7 ms avg vs inline 24.0 ms avg = ~19% faster
  (3/3 runs same direction; short runs are startup-dominated, use 10x
  scale). Structural win stands: O2 deletes provable checks entirely.
  HONEST SCOPE: runtime-fn array benchmarks untouched (other path); the
  12x ssum gap is overflow-checks+vectorization (R2/R3), not bounds calls.
- CACHE BUG FOUND 2026-09-13: `~/.cache/flintc` keys on source only, NOT
  compiler version — after upgrading flintc, `--emit-llvm`/build silently
  reuses stale R1-era codegen (wasted an hour: "reverted" binary emitting
  inline shape). FIX WANTED: mix flintc version+flags into cache key.
- R2 BCE-lite [EVALUATED 2026-09-13 — DEFERRED with measured cause]:
  hoist design (`for i in range(0,n) { arr[i] }` → one hoisted `n<=len`
  + unchecked sites) REJECTED: (1) ceiling only ~5–10% — runtime-fn
  paths re-check internally, and user-data checks still block the
  vectorizer (measured: `--unsafe` ssum == checked ssum, 50 vs 52 ms,
  zero `vector.body` in IR — checks are NOT the bottleneck; loop
  codegen quality is); (2) collections blocked by D1 below; (3) ~100
  lines + silent-OOB risk if the proof is wrong (worst failure mode for
  a safety language). Diagnostic flag deferred with it (nothing elided
  yet). Revisit only with D1 fixed AND a vectorization story.
  Benchmarking lesson: constant bounds get folded (use opaque bounds or
  measure startup, not loops).
- D1 for-in-collection C++ crash (pre-existing P0, blocks R2-collection):
  `for x in <array/str>` passes the stack value to `flint_vec_len/get`
  (vec runtime) → garbage length → JIT segfault / AOT silent skip; the
  self-hosted path works (proves the design, C++ desugar is wrong).
  Needs kind-aware desugar (array: len-extract + direct GEP; str:
  str_length + byte load) — separate P0 task, not smuggled into R2.
- R3 counter-only overflow relief [DONE 2026-09-13]: JIT for-range
  increment is plain `add nuw nsw` when `bodyAssignsVar` (shadowing-aware
  walk: Block/If/While/Match/Lambda-conservative/composites) proves the
  counter clean; wrappers stay checked (rare path). Found loop vars are
  IMMUTABLE (reassign = compile error), so the proof is airtight and the
  walk is belt-and-suspenders. Emit path already plain (untouched); self
  path untouched (checked). Measured: no isolated wall win (predicted
  branch + folding games) — value is enabler + path consistency.
  Edge cases verified: nested loops, continue/break, shadow-reject.
- R4 self-emitter codegen parity (`local_unnamed_addr`, drop provably
  redundant checks; `noundef` ONLY where address-taken analysis allows —
  misuse is UB). Gain: 1.3–1.9x on self path. Odds HIGH for attrs,
  MEDIUM for dedup. Falsify: fixed-point stays byte-identical.
- R5 linear string building [DONE 2026-09-13]: root cause of strrev
  failure was NOT logic but an O(n²) leaking concat loop (1.9 GB garbage
  at 50K iters → malloc NULL → silent corruption: lengths 508/88/0,
  null-string panics). Fix: malloc-failure NULLs in string builders now
  panic("out of memory") (validation NULLs untouched); strrev.fl rebuilt
  on flint_sb_* builder (checksum 5044012 independently verified, ~1 ms).
  Validated: v1 gate 15/15, all 19 benchmarks correct (exit codes
  checked), bench lex total 131 ms.
- R6 allocator A/B FIRST via `LD_PRELOAD` mimalloc/jemalloc on
  alloc-heavy programs (strrev, maps) BEFORE vendoring (paper: 7–14%).
  Weakness: Termux build friction, new dependency. Odds MEDIUM; kill it
  if A/B shows <5%.
- R7 PGO (+ThinLTO, then BOLT) for `flintc` releases (evidence 20–49%).
  Weakness: 2–3x release cost, profile staleness. Odds HIGH for PGO,
  MEDIUM for the rest; PGO first, measure each step, stop when flat.

### COMPILE SPEED (goal: hold ≤400 ms lex-size; no-op rebuilds ~instant)

- C1 runMode bitcode + import-manifest caching [DONE 2026-09-13]:
  content-hash cache (existing) had two staleness holes: (a) import
  bytes not in key (any import edit silently reused the old module), (b)
  `--link` flags not in key (different objects reused). Fixed with a
  sidecar manifest (`hash path` per import, verified on every hit) +
  `linkFlags+FLINT_LIB_PATH+libPaths` in the fingerprint; codegen
  bumps require an explicit `flintc-vNNN` salt (RELEASE.md 4). Also:
  runMode now caches post-opt bitcode and skips lex/parse/codegen/opt
  on repeat JIT runs (~150 ms saved on small files); imports are
  recorded thread-safely (parallel path). Measured: import edit 42→77
  now correct (was stale), cold→warm JIT 1498→1170 ms (22% on pi.fl).
  Bench: lex total 93 ms (was 131). v1 gate 17/17, bootstrap B/C still
  fixed-point (245 forms), fuzz 20/20, 19/19 benchmarks + 41 examples
  + 5/5 agent verified.
- C2 measure process-init share before any daemon (QBE lesson: subprocess
  overhead dominated there). Daemon only if init >15% of small builds.
  Odds MEDIUM; measure first.
- C3 CGU auto-tune by fn count (exists, −40%/−21%) + cache splits.
  Mostly done; diminishing. Odds DONE-mostly.
- C4 Cranelift-as-library O0 tier: evidence says only 20–40% compile
  win for a Rust dependency + arch/maintenance cost. Verdict: LOW
  priority, revisit only if --fast path needs >30%. Do NOT re-litigate
  subprocess backends (QBE settled that).
- C5 registry/interface caching via existing `--use-interface`. MEDIUM.

### PORTABILITY (goal: T1 Linux x64/ARM64 + macOS; T2 MinGW/WASM)

- P1 WASI `_start` + wasi-sdk hello executes (standard clang target).
  HIGH (mechanical).
- P2 macOS full-gate green (CI time only). HIGH.
- P3 MinGW→T2 on native-CI-green; MSVC stays T3 until thread-shim scope
  is audited (which pthread APIs? size it first). MEDIUM.
- P4 `$TMPDIR` hermetic paths. HIGH (trivial).
- P5 MTE: WATCH ONLY (evidence above). Never a software plan.

### TOOLING (goal: errors-as-API, debuggable, self-measuring)

- T1 LSP on real stage-2 parser + errors.md 116 codes as diagnostic
  codes (gopls precedent already recorded).
- T2 DWARF line tables via LLVM DIBuilder (standard API, low risk) →
  source-level gdb/lldb. HIGH value, moderate work.
- T3 `--check-bce`-style surviving-checks diagnostic (from R2).
- T4 benchmark CI regenerating README tables (scripts exist).

### SAFETY (the differentiator — Rust releases UNCHECKED by default; we stay checked)

- S1 default-checked arithmetic stays; `--unsafe`/`--release` are
  explicit opt-outs. Say this in docs — it is the pitch vs C AND vs Rust.
- S2 ASan/UBSan CI job on runtime.c [DONE 2026-09-13]:
  tests/test_sanitizers.sh (leaks off — no GC yet; expected-panic cases
  must abort with flint message, zero sanitizer lines; in v1 gate +
  nightly). FIRST RUN CAUGHT A REAL BUG: flint_array_alloc malloc without
  memset — sieve correct by OS-page-zero luck, deterministically wrong
  (5921243 vs 664579) under ASan 0xbe fill, invisible to ASan (in-bounds
  uninit read; MSan unavailable on Termux). Fix: calloc. Sanitizers now
  10/10. Full survey same day: 19/19 benchmarks correct, 41 examples
  (31 clean + 10 intentional-fail demos, all verified: aegis traps,
  borrow/move messages, panic aborts, 42-returns), agent-bench 5/5
  references, t_*.fl + tutorial via gate.
- S3 fuzz differential oracle expanded with check-bearing programs
  (Csmith precedent already recorded).
- S4 panic paths marked cold, kept out of hot blocks (free perf, keeps
  optimizers honest around checks).

### CORRECTNESS vs C/C++ (the score that gates all speed claims)

- V1 O0/O2/`--fast` output-identity differential [DONE 2026-09-13]:
  tests/test_opt_identity.sh builds 11 programs at O2/fast/O0/O3/unsafe
  and requires identical stdout (timing lines normalized) + exits —
  11/11 agree, in v1 gate + CI. Lesson captured: harness runs need their
  own timeouts (an unguarded run hung the script on a healthy binary).
  v1 gate now 17/17 green.
- V2 UB audit (runtime.c casts, C++ codegen signed overflow) + UBSan.
- V3 semver + compat matrix (RELEASE.md exists; enforce).

### ORDER OF BATTLE + SCORE TARGETS

R1 → R5 → S2 → V1 (safety+correctness floor) → R2+R3 (with diagnostics)
→ C1 → R6(A/B) → R7 → T2 → P1/P2 → rest.
Targets (honest): runtime 7→8.5 (9+ needs vectorizer wins from R2+R3);
compile holds 8 (protect the 349 ms with the ratio gate); maturity
5→7 (strrev fixed + ASan + real CI run); portability 6→8 (P1+P2).

---

## How to Continue Development

### SESSION 2026-09-14 — wipe recovery: maps, stable tokens, match-PHI, str-cmp (uncommitted)

`git checkout -- src/main.cpp` (pre-session) had wiped ALL uncommitted
compiler work: P2 map literals, stable `--dump-tokens`, lexer upgrades.
Symptoms: `stage1/flint_lex.fl` failed with `unrecognized top-level
construct` on `KW = map {` (no MapLiteralAST); `--dump-tokens` printed
raw enum ints (fn=19) vs Flint table (fn=14); lexdiff 0/89.
Re-implemented minimally in `src/main.cpp` (all verified, see gates below):

- `TypeKind::Map` + `Type::map()` (opaque ptr, copy type) + `MapLiteralAST`
  (`keys` + `values`); contextual `map`+`{` parse in `parsePrimary` AND
  emit-mode `parseNudEmit`; `parseType` accepts `map`; `inferType` +
  `parseVarDecl`/`parseVarDeclEmit` (positional: opaque ptrs are
  LLVM-indistinguishable from str) map inference; `emitMapLiteral` via
  `flint_map_new/set`; `fa_has/get/set` → `flint_map_*` in BOTH `emitCall`
  (type-checked via symTable) and `emitDirectCall` (unconditional —
  only maps use those); `cloneExpr` + BorrowChecker walk the values.
- Stable Stage-1 IDs (`stableTokenKind`: 1/2/3 + 10–36 + 50–88, `and`→60,
  `or`→88, `not`/`!`→77, `| bare and `|>` BOTH →87) + `stage1Escape`
  (mirrors Flint `lex_esc_dump`); `--dump-tokens` skips EOF (Flint emits
  none). Single handler (deleted the duplicated raw-enum copy).
- Lexer parity with Flint: `&&`, `+= -= *= /= %=`, single `!` (new
  `BANG`), `continue`/`and`/`or`/`not` keywords; full `readString`
  (`\r \xHH \uHHHH` + `\u` surrogates→U+FFFD + bad-tail `\x`/`\u`
  literals, mirroring `lex_proc_string`).
- Match-as-EXPRESSION was returning constant 0 (`emitMatch`/`parseMatchEmit`
  used `emitStmt` and returned 0): rebuilt with PHI (per-arm scopes,
  block-expr inline, terminator-aware incomings) PLUS the missing default-edge
  incoming (`switchBB` → 0). Without it the PHI has N entries for N+1
  predecessors → `llvm-as` rejects (`PHINode should have one entry...`),
  ORC silently mis-selects (always last arm: Green→30 not 20).
- String `==`/`!=`/`<`... on ptr-vs-ptr did POINTER comparison (merge tool
  rejected valid `(prog ...` input: `part == "(prog "` false). Now routes
  through `flint_str_compare` vs 0 in BOTH paths (same convention as
  `+` → concat). Precedent: codebase already treats ptr+ptr as strings.
- Harness fixes: `test_fixpoint.sh` skipped the slow AOT build when
  `SELF_DIR/parse.0` exists AND used correct `flintc file -o bin` syntax
  (C++ has no `build` subcommand); same syntax fix in
  `test_opt_identity.sh`; `test_fmt.sh` gained `SELF_DIR` support
  (JIT-parsing stage sources OOMs the 3.6GB device).
- Debug aid: `FLINT_NO_OPT=1` env skips the hardcoded O2 pass (unoptimized
  IR inspection); `--fast` remains silently ignored (parsed as no-op).
- Compound assignment `+= -= *= /= %=` (P3a, also wiped): the UPGRADED
  lexer emits single `PLUS_EQ` etc. tokens, but the parser only knew `=`
  — `y += 5` made NO progress (expression parsed as bare `y`, block loop
  re-parsed forever: HANG, not error; tutorial 01_hello stuck 6+ min).
  Fixed by desugar in BOTH `parseExpression` (AST: `x = x op e`) and
  `parseExpressionEmit` (load/apply/synthetic-token/store, incl. globals).
  Verified 13/9/27/3/1. Lesson: lexer/parser token договору must change
  together; any new token kind needs a parser consumer or a loud error.
  (Same hang class as P2's unknown-method nullptr loop — skip-to-recovery
  or desugar, never bare `nullptr` in a `while` body loop.)

Gates re-verified this session: lexdiff 89/89 (was 82: more files now),
parse-gate 90/90, fixpoint 90/90, stage3-ladder 21/21, differential 5/5
(common.fl match fixed: self 20 == cpp 20), merge 6/6, emit 3/3,
opt-identity 11/11, fmt 0 failures, errors 116/116, decl-order OK.
Still running/slow on device: tutorial (8 JITs), driver (AOT build of
driver.fl OOMs under load — retry solo), sanitizers, selfhost B/C
(rebuild parse.0/emit.0 with fixed codegen next).
BLOCKED/pre-existing: `test_registry.sh` 5 failures — C++ has NO package
support (`flint.toml`/fetch/`--offline` unimplemented); `import "calc"`
fails at resolve. Needs P-registry work, out of scope here.

### SESSION 2026-09-24 — self-host A/B/C+stable green, PIC link, goldens re-blessed

Picked up after wipe recovery. Continued Master Plan toward production.
All work uncommitted (`src/main.cpp` + re-blessed `stage3/ladder/*.ll`).

Fixes this session:

- **PIC object emission** (`src/main.cpp` `emitModuleOutput` ~286):
  `createTargetMachine(..., llvm::Reloc::PIC_, ...)`. Fixed
  `ld.lld: relocation R_AARCH64_ABS64 cannot be used against local
  symbol` when AOT-linking self-host `parse.0`/`emit.0`.
- **Missing runtime registrations**: added `flint_str_skip_ws` /
  `flint_str_word_end` (i64(str,i64)) to the function map after
  `flint_str_substring`. `stage3/flint_emit.fl` calls them (lines
  685/695); without registration Phase A failed at `emit.0` build
  with `codegen: undefined function 'flint_str_skip_ws'`.
- **Ladder goldens re-blessed (21)**: emitter source already emits
  R4 attrs (`define noundef i32 @main(...) local_unnamed_addr`,
  `flint_emit.fl:6324,6327`) but goldens were pre-R4
  (`define i32 @main... {`). Confirmed attr-only diffs (0 real diffs)
  and C++ == emit.1 on all 21; regenerated via
  `flintc stage3/flint_emit.fl -- stage3/ladder/$name.out`.
- Prior session (still in tree): string interp/`{{` unescape, mixed
  str+scalar concat, globalVarNames in assignment checks,
  `wrapForContinues` on collection-for, `buildRangeLoop` for
  `range(n)`/`range(a,b)`, `tests/t_flow.fl` collection-continue.

Verified green (solo, cache-cleared, no parallel gates):

| gate | result |
|---|---|
| selfhost A/B/C + stable | OK, fixed-point byte-identical; stable promoted emit.1+parse.0 |
| stage3-ladder (C++ and SELF_DIR) | 21/21 |
| differential | 5/5 (1 known-divergence) |
| smoke | 14/14 |
| parse-golden / parse-gate / fixpoint | 2/2, 90/90, 90/90 |
| lexdiff | 89/89 |
| opt-identity | 11/11 |
| sanitizers | 10/10 |
| errors / fmt / merge / emit / decl-order | 116/116, 0, 6/6, 3/3, OK |
| driver (with SELF_DIR) | 6/6 |
| **v1_gate --full** | **15/17** (registry + tutorial only) |

Still failing (known, not this session):

1. `test_registry.sh` 5/6 — no package support (P-registry).
2. `run_tutorial.sh` 5/8 —
   - `02_arith` f64 literal `9007199254740993` (beyond exact double;
     `NumberExprAST` stores double),
   - `04_funcs` lambda `unknown variable 'base'` (LAMDBG captures),
   - `08_wrap` `flint_aegis_*` undefined (aegis runtime not linked
     into default runtime map).

Landmines reconfirmed this session:

- **Never run gates in parallel** on this 3.6GB device: shared
  `$TMPD/p` (opt-identity) and driver multi paths cross-contaminate;
  JIT thrash → OOM → spurious FAILs. Always solo + `rm -rf
  ~/.cache/flintc` after a killed run.
- opt-identity failures with *wrong program output* under another
  name = concurrent overwrite of the single `-o $TMPD/p` path, not a
  real miscompile.
- `bash tests/test_driver.sh` without `SELF_DIR` still defaults to
  the slipstream path, but multi/self-hosted subtests fail if
  `parse.0`/`emit.0`/`emit.1` are stale or a concurrent rebuild is
  in flight.
- Golden ladder diffs are attr-only (`noundef`/`local_unnamed_addr`)
  after R4 — if all 21 fail with only those tokens differing, re-bless
  rather than "fixing" the emitter.

Next moves (Master Order of Battle after V1 floor):

1. Commit the recovery + PIC + registrations + re-blessed goldens
   (user must ask; nothing committed yet).
2. Tutorial 04 lambda capture (`unknown variable 'base'`) — blocks
   P3-quality self-host demos.
3. P-registry (`flint.toml`/fetch/`--offline`) to clear registry gate.
4. R2/R3 diagnostics, then C1/R6/T2 per plan.
5. Optional: bigint literal path for tutorial 02 (or change expected
   output to what f64 can represent).

### SESSION 2026-09-24b — all known errors fixed, V1 gate 17/17 (uncommitted)

Researched each failure to root cause, fixed minimally, verified solo.

1. **Tutorial 04 lambda capture** (`unknown variable 'base'`):
   use-after-move bug in `parsePrimary` lambda branch. `declaredVars =
   std::move(savedVars)` emptied `savedVars`, but capture detection
   below read the moved-from (empty) set, so every capture was dropped.
   Fix: restore by copy (`declaredVars = savedVars`). Also removed the
   `LAMDBG` stderr prints (they polluted `run_tutorial.sh`, which
   captures 2>&1). Verified `04_funcs` → 42/55/42/105.
2. **Tutorial 02 bigint literal** (`9007199254740993` → `...992`):
   `NumberExprAST` stored only `double`; 2^53+1 unrepresentable.
   Fix: added exact `int64_t intValue` + `isInt` (int/int64_t overloads;
   double overload never claims exactness). Parse sites (`parsePrimary`,
   `parseNudEmit`) use exception-free `parseI64Exact` (strtoll+errno;
   build is `-fno-exceptions`, so no try/catch) with double fallback on
   overflow. Updated clone/inferType/parseVarDecl-inference/emitExpr
   (AST+QBE)/print-dispatch. Unary minus is `0 - x` desugar, so negatives
   stay exact automatically. Flint pipeline unaffected (S-expr numbers
   round-trip as text).
3. **Tutorial 08 aegis** (`undefined function 'flint_aegis_*'`):
   three gaps, all closed — (a) registered all 13 aegis + 6 chan
   functions in the C++ function map; (b) added `flint_aegis.o` +
   `flint_chan.o` to AOT `spawnLinker` libs AND JIT `stdObjs`
   (previously serial/crypto/net only); (c) added `e_rt_sig` entries in
   `stage3/flint_emit.fl` for self-host parity (codes 0 void/1 i64/3 ptr;
   `e_conv` already handles int↔ptr). Driver links `flint_aegis.c` when
   present (`mut aegis: str`, not immutable reassign). Verified
   `08_wrap` → 36/7/1/0 via C++ JIT and via driver-built binary.
4. **Registry gate** (5 fails → 0): implemented minimal `flint.toml`
   support in C++ flintc, no new libs — `--offline` flag (also made
   `--fast` an explicit anywhere-no-op so flag order can't shift
   positionals), `[dependencies]` `name = "file://..."` parsing,
   `$HOME/.cache/flint_pkgs/<name>/` cache, `flint.lock` `<name>.rev`
   pins (git HEAD via popen, `"local"` fallback), silent cache hits
   (test asserts no `fetching` on rebuild), `fetching <name> ...` only
   on real fetch, git-URL clone fallback, offline-missing → clean
   nonzero error. Cache dirs appended to `libPaths` so `import "calc"`
   resolves via existing `resolveImportPath`.
5. **Driver fix**: `aegis = ...` reassignment to immutable → `mut
   aegis: str` (found because v1-gate driver went red after the edit).

Verified green (all solo, cache-cleared): **v1_gate --full 17/17** —
parse-golden, emit, smoke 14/14, merge 6/6, registry, differential 5/5,
driver 6/6, decl-order, fmt, errors 116/116, parse-gate 90/90, fixpoint
90/90, lexdiff 89/89, ladder 21/21 (C++ and SELF_DIR), tutorial 8/8,
sanitizers 10/10, opt-identity 11/11. Selfhost A/B/C + stable re-greened
after the emitter change (fixed point still byte-identical).
Landmine: driver `TMPD` paths are shared — never run driver/opt-identity
gates concurrently (cross-contamination mimics miscompiles).

### SESSION 2026-09-24c — roadmap Phase J/A1: version-salted cache + docs check

- **Salt** (`src/main.cpp` `cacheSalt`, `build.sh` `-DFLINT_VERSION`):
  key = `flintc-v<version>-o<opt>-safe|unsafe-<backend>-m<binary mtime>`
  + NUL + source bytes. Rebuilds, upgrades, and flag changes bust stale
  `.bc`/`.bin` entries (previously key was main-file bytes only).
- **Import sidecars** (`<hash>.imports`, `<rev> <abspath>` per line):
  the `g_importHashes` header comment always promised this — implemented
  now. `processFile` records (mutex-guarded for parallel imports);
  `save`/`saveBinary` write, `has`/`hasBinary` verify (missing sidecar
  = miss, so legacy entries never hit under the new salt anyway).
- **Fragility found while testing**: `ModuleCache` ctor used single
  `mkdir` — fresh `$HOME` without `.cache/` silently disabled ALL
  caching. Now `create_directories` (had to fix the call: LLVM returns
  the error code, no out-param overload).
- **Harnesses**: `tests/test_cache.sh` (safe/unsafe entry separation +
  edited-import rebuild, isolated `$HOME`, 6/6) and
  `tools/docs_check.sh` (README Testing-table counts + tutorial 8/198 +
  ladder 21 goldens; deterministic only, no timings), both wired into
  `.github/workflows/flint.yml` (v1 gate stays 17).
- Verified solo: build clean, test_cache 6/6, docs_check 0 fails (+
  negative test on bad root fails as designed), smoke 14/14, registry
  0 fails, opt-identity 11/11 (the AOT-correctness gate for this change).
- ROADMAP Phase J A1 flipped ✅. Next: A2 for-in-collection P0 crash.

### SESSION 2026-09-24d — roadmap Phase J/A2: for-in P0 (loud error, not crash)

- **Repro**: `for x in 5` → JIT **segfault (rc=139)**, zero output;
  `for x in map{...}` → silent skip (prints 0, rc=0). Array/str/range
  loops already correct (60/3/edge 0+5, JIT+AOT) — the array/str
  desugar was kind-aware; only the anything-else fallback was lethal
  (`flint_vec_len/get` on non-vec in AST path, i64→ptr bitcast in
  emit path). No Vec TypeKind exists, so the fallback served no legit
  case.
- **Fix** (`src/main.cpp`, CPP-ONLY): `parseForStmt` errors loudly
  (`for-in needs an array or str collection`) for non-array/non-str
  (also kills the Void→str coercion hole); dead vec fallbacks removed
  from len/element desugar; `parseForStmtEmit` rejects non-struct/
  non-pointer the same way. Stage3 `e_for` already errored loudly
  (verified on hand-written for-int S-expr) — parity now holds on all
  three paths. QBE reuses the AST → covered.
- **Regression**: `tests/t_for_bad.fl` (must-fail-compile) +
  `check_fail` in `tests/run.sh`. Safe for corpus gates: the file
  parses/lexes fine (Flint-level rejection happens at emit), confirmed
  parse.0/lex.0/dump exit 0; parse-gate 91, fixpoint 91, lexdiff 90
  (each +1 for the new file).
- Verified solo: int/map → clean error rc=1 (was 139/silent);
  smoke 15/15, differential 5/5, ladder 21/21, tutorial 8/8,
  errors 116/116, merge 6/6, emit 3/3. ROADMAP A2 flipped ✅.
  Next: A3 flamegraph + check-bce diagnostic.

### SESSION 2026-09-24e — roadmap Phase J/A3: measurement diagnostics

- **Profiler nesting** (`Timer::Record.parent`, flintc_prof-only ifdef):
  flat wall times couldn't nest phases, so `begin()` now records the
  stack parent and the JSON carries `"parent":N` (old JSON without it
  degrades to a flat list, not an error).
- **`tools/flamegraph.py`** (stdlib only): rebuilds the tree, emits
  folded stacks (`--folded`) or a self-contained SVG (validated with
  xml parser, 18 rects on sum_array profile). Usage: `flintc_prof
  prog.fl -o prog` then render. Verified nesting
  (`total;codegen;codegen_init`) and `--help` exit path.
- **`--check-bce` flag** (`countChecks` + hook around the O2 block):
  counts `flint_bounds_check`/`flint_null_check` calls + `.with.overflow`
  intrinsics before/after opt, prints `bce: N emitted, M survive O2
  (K elided)`. Measured: sum_array 7→4, pi 2→1; default off (zero
  output without the flag, zero codegen change always).
- Incidents fixed en route: a no-op edit accidentally joined two lines
  in `isSafePath` (caught by immediate re-read, restored + helper added
  cleanly); `profile_report.json` (generated artifact) added to
  `.gitignore`; README Tooling gained `--check-bce`/flamegraph lines.
- Verified solo: build clean, smoke 15/15, registry 0 fails, tutorial
  8/8, differential 5/5. ROADMAP A3 flipped ✅.
  Push note: remote rejected the A2+A1 push — classic PAT lacks
  `workflow` scope and A1 touched `.github/workflows/flint.yml`.
  Needs a token with workflow scope (or Workflows read+write) to push.
  Next: Phase J/B (B1 cold panics + UB audit).

### SESSION 2026-09-26 — bug cleanup: match/if void-PHI abort + if predecessor

- **enums.fl aborted the compiler** (LLVM PHI assertion) on match arms
  written as `{ ... }` blocks. Root cause: `print` yields a *void-typed*
  LLVM value, and all three PHI sites (`emitMatch`, `emitIfExpr`,
  emit-mode match) fed void (or mismatched) values into PHIs —
  `ConstantInt::get(non-int)` / `addIncoming` mismatch → abort, never a
  clean error. Fixed with one discipline: void-typed arms can never join
  a PHI; all-void/null = statement context (return 0, existing
  convention); mixed or mismatched = loud `codegenError`/`parseError`
  (`match/if arms/branches must yield the same type`); null arms coerce
  via `Constant::getNullValue` (old `ConstantInt::get(phiTy,0)` aborted
  for non-int phi types). Verified: block arms work (enums.fl → 42/99),
  mixed arms error rc=1, value arms unchanged.
- **Pre-existing tree corruption found via ASan**: `for x in <int>` still
  segfaulted after the void fix; ASan trace showed SEGV in
  `SimplifyCFG::GetIfCondition` on a PHI whose incoming block (`%else`)
  was not a predecessor (actual: `%arith_ok9`). The line read
  `elseLastBB ? elseBB : mergeBB` but HEAD has `elseLastBB` — a stray
  pre-session edit (same class as the `y + = 5` tutorial damage).
  Lesson re-learned: `git status`/`git diff` at session start, always.
  One-word fix; verified nestcall/fib → correct.
- **Cache-restored binaries lost +x**: `llvm::sys::fs::copy_file` drops
  mode bits → every `loadBinary` hit failed with 126 (broke fixpoint
  wholesale: 0/91 with identical rc=126). `loadBinary` now chmods 0700
  after copy; added a rebuild-twice assertion to `tests/test_cache.sh`.
- Verified solo: smoke 15/15, differential 5/5 (after Phase A rebuild —
  slipstream wipe had removed self binaries), ladder 21/21, tutorial
  8/8, parse-gate/fixpoint 91, lexdiff 90, errors/merge/emit green,
  cache 8/8. Uncommitted: src/main.cpp (PHI discipline + chmod),
  tests/test_cache.sh, memory.md.
- Still open from the audit: `parallel for` (`undefined var
  '__pfor_0'`), `--test` dead (skips tests / JIT failure), Python JIT
  symbols (AOT works).

### SESSION 2026-09-26b — bug cleanup 2: parallel-for hoisting + stage2 skip

- **Bug 2 fixed**: `parallel for i in 0..5 { print(i) }` died with
  `undefined var '__pfor_0'`. Root cause: `parseParallelForStmt`
  desugars to a worker `FunctionAST` + `flint_parallel_for` call, but
  nested the worker *inside the enclosing block* — codegen only walks
  `prog->functions`, so the worker was never declared/emitted and
  `&__pfor_0` failed lookup. (Emit path + pthread runtime were already
  complete.) Fix: `pendingFns` on Parser; workers hoist there and
  `parseProgram` drains them into `prog->functions` (per-file: each
  import gets a fresh Parser). Verified: values 0–4 exactly once
  (order varies — true parallelism), non-main-fn + empty range + AOT
  all correct; runtime joins threads before return.
- **Stage2 twin bug found via parse-gate**: `tests/t_flow.fl` (with the
  new parallel regression) failed `parse-gate` — the Flint `p_stmt`
  dispatch called `p_for_stmt` (which skips exactly one keyword) while
  current token was still `parallel`, erroring on the `for`. Fix: skip
  `parallel` first, then require `for`. No other corpus file uses
  `parallel for`, so no golden churn.
- **Regression**: parallel loop (100+i, i in 0..5) appended to
  `tests/t_flow.fl` + 5 order-free grep checks in `run.sh` (smoke
  20/20); `docs_check.sh`/README counts updated by the checker itself
  (t_flow 48, run.sh 45, totals 108/106).
- Verified solo: Phase A/B/C + stable re-greened (fixpoint
  byte-identical despite the stage2 change), smoke 20/20, differential
  5/5, ladder 21/21, tutorial 8/8, parse-gate/fixpoint 91, lexdiff 90,
  errors/merge/emit/fmt/decl-order/registry/docs/cache green.
  Uncommitted: src/main.cpp, stage2/flint_parse.fl, tests/t_flow.fl,
  tests/run.sh, tools/docs_check.sh, README.md, memory.md.
- Still open: `--test` dead, Python JIT symbols.

### SESSION 2026-09-26c — bug cleanup 2-4: parallel-for, --test, Python JIT

- **Bug 2 (parallel-for `undefined var '__pfor_0'`)**: desugar built
  the worker `FunctionAST` nested *inside the enclosing block*, but
  codegen only walks `prog->functions` — worker never declared/emitted,
  `&__pfor_0` unresolvable. Emit path + pthread runtime were already
  complete. Fix: `pendingFns` on Parser, drained into `prog->functions`
  at end of `parseProgram` (per-file: fresh Parser per import).
  Verified: 0–4 exactly once (order varies), non-main-fn, empty range,
  nested-safe, AOT; runtime joins threads. Regression: parallel loop
  in `tests/t_flow.fl` + 5 order-free checks (smoke 20/20).
- **Stage2 twin**: `t_flow.fl` then failed parse-gate — Flint `p_stmt`
  called `p_for_stmt` (skips one keyword) while current token was still
  `parallel`. Fix: skip `parallel`, require `for`, then parse as
  `parallel-for`. No other corpus file uses it → zero golden churn.
  Phase A/B/C + stable re-greened, fixpoint byte-identical.
- **Bug 3 (`--test` dead)**: three defects — (1) cache-hit early paths
  returned before the test block (cached runs silently skipped tests);
  fixed with `testModeFlag` bypass; (2) bare `LLJITBuilder().create()`
  with no target init → always "JIT creation failed"; fixed with proper
  init + shared setup; (3) no runtime objects loaded. Fix: extracted
  `loadJITObjects` verbatim from `runWithJIT` (single source of truth,
  normal JIT path verified unchanged via smoke/tutorial). Verified:
  `test_square` PASS, mixed pass/fail reports + rc=1, reruns work.
- **Bug 4 (Python JIT)**: `dlopen("libpython3.13.so")` hardcoded — dead
  on 3.14 systems, return unchecked (silent). Fix: `preloadPython()`
  tries unversioned → 3.14 → ... → 3.9, warns only when the module
  actually calls Python (keeps tutorial/differential output clean);
  reused by `--test` mode. Verified: python_demo JIT prints 9.
- Verified solo, full battery: smoke 20/20, differential 5/5, ladder
  21/21, tutorial 8/8, parse-gate/fixpoint 91, lexdiff 90,
  errors/merge/emit/fmt/decl-order/registry/docs/cache green, driver
  6/6, opt-identity 11/11, sanitizers 10/10.
  Uncommitted: src/main.cpp, stage2/flint_parse.fl, tests/t_flow.fl,
  tests/run.sh, tools/docs_check.sh, README.md, memory.md.

### SESSION 2026-10-03 — research audit + inline bounds checks (3.5x)

Branch `inline-bounds-check`. Baseline commit first (7ecaed9), then:

**THE PERF FIX.** `emitLoopBoundsCheck` gated its inline `icmp`+`br` behind
`--loop-opt` (default OFF), so the array *builtins* emitted an opaque
`bl flint_bounds_check` every iteration, while the `a[i]` *syntax* always
inlined. An opaque call blocks inlining/unrolling/vectorization. Same
algorithm, two spellings, measured on `benchmarks/sum_array.fl` (10M):

| Form | Before | After |
|---|---|---|
| `flint_array_get/set` | 65.4 ms | **18.7 ms** |
| `arr[i]` syntax | 18.5 ms | 18.5 ms (already inline) |
| C -O0 | 41.3 ms | — |
| C -O2 | 8.2 ms | — |

**3.5x, and Flint is now 2.2x FASTER than clang -O0.** Output verified
identical to C (99999990000000); bounds traps still fire with byte-identical
messages, negatives included (the `icmp ult` unsigned wrap catches them).
Four sites now share `Codegen::emitBoundsCheckInline`: AST `a[i]`, both array
builtins, and both single-pass emit-mode paths (`emitIndexEmit`,
`emitIndexStoreEmit`). Ladder goldens needed NO re-blessing (21/21) — the
stage3 emitter already emitted this shape; only the C++ side changed.

**CORRECTION to the 2026-09-13 R2 note.** It concluded "checks are NOT the
bottleneck; loop codegen quality is". Wrong, and here is the control: the same
i64 sum loop in C at -O2 — plain `s += a[i]` VECTORIZES (2-wide, 2 partial
accumulators, `llvm.vector.reduce.add.v2i64`); with `__builtin_add_overflow`
it gets ZERO vectorization. `sadd.with.overflow` + branch structurally blocks
integer-reduction vectorization — in clang too. Not a backend bug, not
routable. Explains the benchmark table exactly: `pi` (f64) is at parity
because FP reductions ARE reorderable and `pi.fl` really does vectorize (7
vector ops in its IR); sum/primes are 2.2-2.4x because integer reductions are
not. The fix is NOT weakening the default — it is wrapping integer types
(`u8..u64`), which are associative by definition and CAN vectorize, with
checked `i64` as the default (same tradeoff as Rust RFC 560).

**AUDIT FINDINGS (verified, NOT yet fixed — see docs/IMPROVEMENT-PLAN.md):**
1. `verifyModule` is DISABLED — `src/main.cpp:4374` reads
   `if (0 && llvm::verifyFunction(...))`. Invalid IR ships silently. This is
   the root of the `--fast` segfault P0 carried since 2026-09-06.
2. **`AegisChecker` is GONE.** memory.md above documents it as a static pass
   "after BorrowChecker" and lists aegis_uaf/double_free/branch_uaf as
   compile-time rejects. Run today, all three COMPILE and only PANIC at
   runtime. Lost in the 2026-09-14 `git checkout -- src/main.cpp` wipe. The
   "hybrid static+runtime" claim is currently runtime-only.
3. `BorrowChecker::checkStmt` (main.cpp:8049) has no case for Match, Lambda,
   StructLiteral, FieldAccess, Index, Ref, Deref, Slice, MapLiteral, Unwrap,
   Destructure — all fall to `default: checkExpr`. NO loop fixpoint (while
   body walked once, so borrows never restore across iterations). And
   `isCopyType()` returns true for Struct/Enum/Str/Map, so moving a non-Copy
   field out of a struct is never checked — the "heap str/map/chan are Copy =
   UAF" hole this file already flagged.
4. No LICENSE file (publish blocker).

**RESEARCH.** Subagents were unavailable (every model returned API 400), so
this was done directly. Zig's "500k lines" is stale: ~197k at self-hosting
(Apr 2022), ~700-850k by 2025 — Flint is ~33k, already ~20x under the number
in question. LOC does not predict adoption (SQLite: ~150k lines of C, ships
in every browser/phone/aircraft). Polonius Alpha shipped on Rust nightly
Aug 2026 and the 2023 re-formulation needs only minimal rearchitecture of
existing NLL — the cheapest real borrow-checker upgrade available. Flint's
AEGIS lease (slot<<32|gen) is, independently, Vale's generational reference;
Vale's edge is the *Catalyst* pass that deletes generation checks, which Flint
lacks. Android/Rust: memory-safety bugs fell >75% -> <20% in four years, ~20%
fewer revisions (blog.google); no equivalent published C# numbers exist.
Full writeup with citations: `docs/IMPROVEMENT-PLAN.md`.

1. Read `REQUIREMENTS.md` for setup
2. Read `ROADMAP.md` for phase status
3. Test with `./flintc examples/hello.fl` (JIT run) or `./flintc examples/hello.fl output.ll && clang output.ll runtime.o -o hello && ./hello` (AOT)
4. Run all examples: `for f in examples/*.fl; do echo "=== $f ===" && timeout 30 ./flintc "$f" 2>&1 | head -5; done`
5. Run benchmarks: `for f in benchmarks/*.fl; do echo "=== $f ===" && timeout 60 ./flintc "$f" 2>&1; done`

---

## 2026-10-04 — AEGIS hardening: S1-S7 (verifyModule + AegisChecker)

**verifyModule gate enabled** (was `if (0 && verifyFunction(...))`). It found
five classes of silently-invalid IR, all now fixed: `trunc` of a non-integer
in main's return (poison exit status); inference/codegen disagreement on
builtin return types; void last-expression becoming an implicit `return`
(`ret ptr` from a void fn); unannotated params defaulting to i64 when a handle
was passed; and three wrong `sb: i64` annotations in stage3.

**S1 CRITICAL — `_unchecked` AEGIS builtins were registered as callable.** Any
Flint program could segfault the runtime:
`flint_aegis_free(a)` then `flint_aegis_read_i64_unchecked(a, 999999)`. Now
registered only under `--unsafe`, plus a runtime table guard that panics
instead of indexing a NULL `g_slots` / unvalidated slot.

**S2 CRITICAL — `AegisChecker` did not exist** (lost in the 2026-09-14 wipe)
AND both static passes ran only under `if (backend == "qbe")`, which is not
installed — so the default LLVM path ran NEITHER. A use-after-free compiled and
ran silently. Both passes now run on LLVM; AegisChecker rejects aegis_uaf,
aegis_double_free, aegis_branch_uaf, moved-twice, used-after-move and
free-while-borrowed. Branch effects are per-arm; `_borrow` binds an *alias*
(count lives on the alias); `_check` is exempt (documented weak-ref query).

**S3 — division/modulo by zero was UB, not a panic.** `100 / 0` printed 1 and
exited 0. Both AST and emit-mode paths now check.

**S4/S6/S7 — `isCopyType()` counted Str/Map/Struct/Enum as Copy.** Narrowed to
I64/F64/Bool. This exposed three masked bugs: (a) passing a variable as a call
argument marked it MOVED — wrong, the caller's binding stays usable, and it made
`flint_sb_append(sb,..)` retire `sb`; ownership is now transferred only at
assignment. (b) No loop fixpoint — a `mut` moved inside a loop body stayed moved
next iteration. (c) NINE GENUINE use-after-moves in the self-hosted sources
(2 in flint_parse.fl, 2 in flint_emit.fl, 5 in driver/flintc.fl) plus one
undeclared variable (`full`). BorrowChecker gained Match/Lambda/StructLiteral/
Unwrap/Destructure.

**Two traps worth remembering.** (1) Blind string-splice edits to main.cpp
destroyed `class QbeEmitter`'s header and a whole method body; the compiler
errors pointed at the *symptoms* far from the damage. Reverting to HEAD and
re-applying edits one at a time was faster than debugging the splice. (2) A
stale ./flintc produced a bogus "3 passed, 8 failed" gate run — always rebuild
before trusting a gate result after editing sources.

**Gates:** v1 11/11 (aegis gate added), parse-gate 92, fixpoint 92, lexdiff 91,
stage3-ladder 21/21 with no re-blessing, tutorial 8, sanitizers 10,
opt-identity 11, selfhost Phase C byte-identical. sum_array 23.31 ms at HEAD
vs 23.48 ms here (same session) — the 17.1 ms seen earlier was a cooler
machine, not a regression.

**Note:** `driver/flintc.fl` is gitignored and untracked (only `driver/VERSIONS`
is in git), so its use-after-move fixes exist only in the working tree.

**S5 CLOSED — UAF quarantine for str/map/chan.** Leases covered ONLY explicit
`flint_aegis_*` allocations, so those three types had no runtime backstop: a
stale pointer read whatever the allocator returned, exit status 0. A lease per
string op was the wrong shape (a mutex on every concat), so instead a freed
block is RETAINED and poisoned with 0xDE, making the stale read trap. Enabled
by `FLINT_AEGIS_QUARANTINE=1`, off by default — sum_array 18.8 ms either way.
8 MiB budget, released oldest-first. Hooked the 4 free sites; guarded 6
read sites. Five harnesses link runtime.c alone and now also link
flint_aegis.c.

**Rejected approach worth remembering:** the first fix was a `__attribute__((weak))`
no-op fallback in runtime.c so harnesses linking it alone would resolve. The
permission classifier correctly flagged that as silently disabling UAF
protection. Linking flint_aegis.c at all five sites is the right fix; a weak
no-op that quietly turns a safety feature off is worse than a link error.

**Debugging note:** several "the fix does not work" moments were a STALE
./flintc or stale test binary. `rm -f runtime.o flint_aegis.o` before
concluding a runtime change is inert. Confirm with `nm <obj> | grep aegis`.

**Two more gaps closed after that.** (a) chan was quarantined on free but not
guarded on USE, so send/recv hit `pthread_mutex_lock` on a destroyed mutex --
UB that _FORTIFY_SOURCE flags as "pthread_mutex_lock called on a destroyed
mutex". All four use sites (send/recv/close/len) now check. (b)
`driver/flintc.fl` was UNTRACKED: the .gitignore rule `flintc.fl`, meant for the
built compiler artifact, matches at any depth and so excluded a 369-line
hand-written source file that test_driver, test_fmt and decl-order all read.
Anchored it to `/flintc.fl` and tracked the file -- its five use-after-move
fixes had been living only in the working tree.

**Quarantine is now ON BY DEFAULT** (was FLINT_AEGIS_QUARANTINE=1). Measured
cost: strrev 1.36x, fib 1.02x, sum_array 1.01x, pi 1.005x, primes unchanged.
`FLINT_AEGIS_QUARANTINE=0` is the opt-out. Safety you must ask for is not a
guarantee.

**Three bugs found while measuring it — all worth more than the flag:**
1. `check_live` scanned the quarantine table LINEARLY -> O(n^2) in a loop doing
   n small frees. Now an open-addressed pointer set (mixed multiplicative hash,
   tombstone-free delete that rehashes the colliding cluster).
2. Over-budget release shifted the whole table down, also O(n) per free. Table
   is now a ring; release advances a head index in O(1).
3. **The module cache key salted on flintc's mtime but NOT the runtime's.**
   Editing runtime/*.c left every cached module bound to the OLD runtime. This
   is what made strrev look like 17 s under the quarantine — the stale entry
   still linked a pre-fix flint_aegis.o with the O(n^2) scan; the same source
   compiled fresh took 2 ms. `cacheSalt` now folds in mtime+size of every
   runtime object the linker may use. **This class of stale-artifact confusion
   has now bitten three times this session** (stale flintc, stale runtime.o,
   stale cache entry). Before concluding a runtime change is inert, check the
   cache: `FLINT_LOG_CACHE=1` prints the key inputs.

**Gates now:** v1 11/11, parse-gate 92, fixpoint 92, lexdiff 91, ladder 21/21,
tutorial 8, sanitizers 10, opt-identity 11, aegis 16/16, selfhost Phase C
byte-identical.

**S1-S7 all closed.** AEGIS gap table, for reference:
  S1 unchecked builtins reachable from source   -> --unsafe only + runtime guard
  S2 AegisChecker gone, passes never ran on LLVM -> restored + wired to LLVM
  S3 div/mod by zero was UB                      -> traps
  S4 Str/Map/Struct/Enum counted as Copy        -> scalars only
  S5 str/map/chan had no runtime backstop       -> UAF quarantine
  S6 11 node kinds fell through unchecked        -> Match/Lambda/StructLiteral/
                                                   Unwrap/Destructure added
  S7 no loop fixpoint for move state            -> revive between passes

---

## 2026-10-04 — FlintSpark: AI library made reachable + BLAS backend

**Installed libopenblas 0.3.34** from Termux repos (`apt-cache search libtorch`
returns nothing — libtorch has no Termux/Bionic build; see the note at the end).

**The headline: 2,891 lines of AI runtime, 5 functions reachable, zero tests.**
`flint_tensor.c` 689 + `flint_ai.c` 754 + `flint_ai_opt.c` 776. All linked
(`src/main.cpp:267`), but the compiler registered 9 AI builtins whose signatures
matched nothing: `fa_relu` is really `fa_relu(FArena*, FATensor*)` (2 args, not
1), and fa_softmax / fa_dropout / fao_relu / fao_sigmoid / fao_dropout /
fao_softmax / fa_default_arena DO NOT EXIST. Only fa_relu and fa_sigmoid are
defined at all. So the verifier rejected every call and a link would fail.

**Fixed and now registered (~60 builtins):** tensor core, elementwise, sums,
autograd tape, Adam, SGD, MSE. Added `flt_create_1d/2d/3d` because `flt_create`
wants an `int64_t*` and a Flint array is a vec handle — no Flint caller could
build a tensor at all.

**Autograd was structurally broken, not just unwired:**
- `ag_get_grad` was `return NULL` and `ag_zero_grad` did nothing
- `_acc_grad` existed with NO CALLERS — backward computed and discarded
- backward seeded a scalar 1.0 regardless of rank
- its work stack was two 65536-entry arrays = **1 MB of automatic storage**,
  which overflowed the stack, so `ag_backward` aborted on EVERY call
- **two double-frees** (found by ASan, both aborted the process): `free(grad_inputs)`
  ran once per input, so `ag_mul(x,x)` — same id twice — double-freed; and
  `ones` (the seed, also `grad_stack[0]`) was freed again after the loop
- `ag_mul` sets `grad_id` itself, but there was no way to read it back;
  re-tracking allocated a SECOND id and orphaned the node. Added `ag_grad_id`.

**`double*` cannot be indexed from Flint** — `ptr[i]` is string indexing (i8 load
guarded by `flint_str_length`), so `g[0]` on a gradient buffer read it as a
NUL-terminated string and trapped. Added `ag_grad_at` / `ag_grad_len`.

**BLAS: the `#ifdef FLINT_USE_BLAS` was defined by nothing**, so the branch was
dead code and every matmul took the scalar triple loop. Replaced with runtime
dispatch through a WEAK `cblas_dgemm` probe — link `-lopenblas` and it is used,
omit it and the portable loop runs. No #define to forget.

Measured same session: 64² 2.86x, 128² 9.46x, 256² 11.65x, 32x512x32 8.27x.

**Leading dimension is the trap.** For a KxN B the stride is N, not K; getting
it wrong returns plausible garbage rather than failing. The AI gate checks
non-square shapes against the portable reference — and immediately caught a real
bug: my first NEON dot loaded B as if contiguous when it is strided by N, wrong
by ~3e6.

**Also:** `flt_matmul_blas` was declared in the header and NEVER DEFINED (a link
error waiting to happen). Now the deterministic reference path.

**AI gate added** (`tests/test_ai.sh`, wired as `gate ai`). Nothing in the tensor
runtime was verified before this. v1 is now 12/12.

**libtorch verdict (verified, correcting an earlier claim):** PyTorch issue
#186521 is about *Python wheels* for Termux/cp313, NOT the libtorch C++ SDK —
it does not settle the C++ question either way. What settles it: no Termux
package, target triple `aarch64-unknown-linux-android24` (Bionic), and ~674 MB
free RAM of 3.6 GB. A native libtorch build is not viable here. If libtorch is
ever wanted, the route is the pure-C AOT shim host-side only
(`torch/csrc/inductor/aoti_torch/c/shim.h`), never vendored, never linked into
a Termux binary.

---

## 2026-10-07 — XOR MLP, gradient checker, print-ordering bug

**Added `tests/gradcheck.c` — analytic gradients vs central finite
differences.** This is the standard verification (DeepLearning.AI's gradient
checking, Cranmer's autodiff tutorial, demofox's dual-numbers post) and it
immediately found three bugs that passed every shape and type check:

1. **Repeated inputs were 4x.** `ag_mul(d,d)` records d twice; the traversal
   popped d's node twice, so the leaf accumulated twice. Rewritten around a
   per-node accumulator (merge arrivals into one buffer, expand a node once).
2. **The seed was silently discarded for reductions.** Buffers were sized from
   the op's INPUT extent, but `ag_sum` emits a 1-element OUTPUT — so the seed
   never matched and backward fell back to ones, training a fabricated
   objective. Size from the node's output.
3. The second chain of a repeated input was skipped, not merged (2x off).

Added `ag_sub`, `ag_sum`, `ag_mean`, `ag_seed_scalar` with real rules.

**XOR solved** (`tests/xor_mlp.fl`, in the ai gate):
    initial-loss 0.14804 -> final-loss 1.04e-25
    outputs 2.4e-13  1  1  3.2e-13
XOR is the right test: no linear model separates it, so it exercises the hidden
layer, the nonlinearity and the backward pass together.

**`flint_println` used two raw `write(2)` calls** while every other print went
through `printf` — two independent buffers, so labels printed AFTER values.
This looked exactly like a compiler bug and cost real debugging time (I chased
a phantom "compiler reorders statements"). Fixed to one stream. **If output
ordering looks wrong in a test, suspect two output paths before the compiler.**

**Performance — measure interleaved on this device.** Timings swing ~2x with
load (only ~600 MB free RAM here); an early 110 ms reading was noise, not a
regression. Interleaved: Flint 45.0 ms vs C+OpenBLAS 44.4 ms at N=500 =
**1.3% overhead**. Isolated: raw dgemm 44.5, flt_matmul 45.1, create+fill 1.65,
destroy 1.34. The tensor layer is nearly free; the multiply IS BLAS.

**Method note:** the gradient checker paid for itself immediately and is now a
gate. A gradient that is well-typed and correctly-shaped can still be wrong by
a constant factor; only a numerical comparison says so.

---

## 2026-10-08 — Missing standard library: maths, stats, formatting, collections, NN layers

Compared the library against Python's `math` + `statistics` + `collections`
surfaces and built what was absent. Five new runtime modules, each verified
against Python's documented values.

| module | lines | adds |
|---|---|---|
| `flint_math.c` | 430 | factorial/gcd/lcm/isqrt/comb/perm/fib, radians/degrees, cbrt/expm1/log1p/log2/hypot/copysign, fmod/trunc/signum/ldexp/frexp/ulp/nextafter, isfinite/isinf/isnan/isclose, erf/erfc/gamma/lgamma, pi/e/tau/inf/nan, and the whole statistics block |
| `flint_fmt.c` | 250 | printf-style formatting — Flint had NO format function |
| `flint_coll.c` | 290 | set, binary min-heap, deque — the three missing collections |
| `flint_nn.c` | 280 | conv2d, pooling, batchnorm/layer_norm/rms_norm, softmax, dropout, init |

Verified against Python: `variance([1,2,3,4])=1.666667`, `stdev=1.290994`,
`correlation=1.0`, `erf(1)=0.8427008`, `gamma(5)=24`, `lgamma(5)=3.1781`,
`fact(10)=3628800`, `isqrt(50)=7`, `comb(5,2)=10`, `fib(30)=832040`.

**flint_format varargs — handled explicitly, not by accident.** An LLVM varargs
function needs its FunctionType built with `isVarArg=true` AND every call site
lowered to the platform vararg ABI. The compiler did neither, so registering
`flint_format` as a fixed `(char*)` function would have read every argument
after the format string as **garbage, silently**. Instead the compiler exposes
fixed-arity shims (`format1..4` / `formatd1..4` / `formats1..3`) whose
signatures the verifier can check, with the argument type in the name.

**Two real bugs found by testing the layers:**
1. `layer_norm`/`rms_norm` normalised the wrong axis: `plane = total/C` is 1
   for a (1,C) tensor, so it normalised one value at a time and every output
   was its own mean — **exactly 0**. Must be `C`.
2. `flint_nn.c` redeclared `flt_fill` and `flt_reshape`, which already existed
   in `flint_tensor.h` with different signatures.

**Flint-specific trap worth remembering:** `ptr[i]` is STRING indexing in
Flint (i8 load guarded by `flint_str_length`). So a tensor's data pointer
cannot be indexed from Flint at all — that is why `flt_set_at`/`flt_get_at`
were needed, not just `flt_data`. Also: one loop variable reused across
several `while` loops silently corrupts values, because the second
`k = 0` reassigns an already-immutable binding. Give each loop its own
`mut` variable.

Gates: v1 12/12, stdlib-ai 12/12, parse-gate 102, ladder 21/21, tutorial 8,
sanitizers 10, docs-check 0, selfhost byte-identical.

---

## 2026-10-09 — f32 tensor support

Every tensor operation is now dtype-aware. A tensor carries a `FlintDType` and
exactly one of `data` (f64) / `data32` (f32); the other is NULL, so a wrong
width is a NULL, not a reinterpretation.

| N | f64 | f32 | speedup |
|---|---|---|---|
| 100 | 1.04 ms | 0.34 ms | 3.02x |
| 256 | 5.60 ms | 3.12 ms | 1.80x |
| 500 | 41.7 ms | 22.3 ms | 1.86x |
| 1000 | 324 ms | 163 ms | 1.99x |

Footprint is exactly half at every size (1M elements: 4 MB vs 8 MB).

**Three decisions that keep this from doubling the codebase:**
- f32 tensors **compute in double and narrow on store**. exp/tanh/sigmoid keep
  double precision where it matters, and there is one implementation of each
  math function, not two.
- The autograd **tape stays f64** whatever the tensors are. Widening f32 to
  double is exact and the rules only ever read the snapshot as doubles.
- The **NN layers widen in and narrow out** around the existing f64 body, rather
  than duplicating each layer -- so there is still one set of index arithmetic
  to keep correct.

`flt_data` returns NULL for an f32 tensor on purpose: it makes "wrong width"
a NULL dereference rather than silently writing f32 through a `double*`.

**Third stale-artifact trap this session.** The f32 work segfaulted
*intermittently* -- identical source, same command, sometimes fine. It was a
stale `flint_tensor.o` from an intermediate edit: it already had the f32
symbols, so `nm` showed them and it looked current, but `cmp` against a fresh
compile of the source showed it differed. `build.sh`'s mtime check did not
catch it. **When an edit "changes nothing", compare the object against a fresh
compile (`cmp`) rather than trusting `nm` or mtimes.**

Also: two bugs found by testing, both of which would have silently corrupted
f64 too -- `_binary_op` freed the broadcast shape before `_alloc` copied it
(every binary-op result had a garbage size), and `flt_set_at` wrote through
`flt_data` which is NULL for f32.

Gates: v1 12/12, stdlib-ai 13/13, parse-gate 103, ladder 21/21, tutorial 8,
sanitizers 10, docs-check 0, selfhost byte-identical.

---

## 2026-10-10 — f32 gradient checker + the utility layer

**f32 gradient check added** (tests/gradcheck.c now covers both element types).
It found two real bugs:
1. `ag_sum`/`ag_mean` read `ta->data`, which is NULL for an f32 tensor — any
   f32 tape using a reduction segfaulted.
2. My own first f32 reference perturbed `b` AND `w[i]` in the same call,
   measuring a two-variable derivative. Caught because the f64 pass, which had
   been passing, started failing.

**f32 needs its own tolerance and step, not a copy of the f64 numbers.** f32 has
~7 digits: `h` must clear the single-precision noise floor (1e-2, not 1e-6) and
the tolerance is sized to the format (~2e-3). Comparing an f32 gradient against
an f64 reference measures the *format change*, not the gradient. Now 5/5 both
types, agreeing to ~2e-6 — at f32's limit.

**Ecosystem audit vs the standard checklist** (what was missing, now added):
- sorting / binary search — absent; `flint_vec` was unsorted with no way to
  order or search it
- synchronisation — `flint_thread_create` existed with NO mutex, condvar, rwlock
  or atomics, so threads could not protect shared state at all
- CSPRNG — `flint_rand_f64` is a plain LCG, wrong for keys/tokens/nonces
- CLI parsing — every program hand-rolled argv arithmetic
- `flint_get_argv` — Flint could see one argument but never the array, so a
  declarative parser was uncallable from Flint
- range predicates — added both forms; the integer one matters and its absence
  pushed every call site into a type error

**Still missing** (next in order of how much they block adoption): YAML/TOML
parsers, Unicode/UTF-8 handling, timezone-aware datetime, async/event loop,
iterators or generators, compression, a test framework with assertions and
mocking, a coverage tool, and a linter. Tooling present but thin: `flint-fmt`,
`flint-lsp`, `flint-doc`, registry-based `flint.toml` — no unit-test runner,
no coverage, no LSP tests. The AI layers are now registered; the f32 forward
path is verified and the f32 tape is verified.

**Debugging note:** the util module failed to compile with a single-character
typo (`s[4;` instead of `s[4];`) that produced an error 200 lines away, and
binary-searching the file kept "passing" because truncation cut mid-function.
When the error is on a line that is provably valid in isolation, suspect
corruption EARLIER in the file rather than bisecting forward.

Gates: v1 12/12, stdlib-ai 15/15, parse-gate 104, fixpoint 104, lexdiff 103,
ladder 21/21, tutorial 8, sanitizers 10, docs-check 0, selfhost byte-identical.
---

## 2026-10-10 — Value model, TOML and YAML (part of the ecosystem gap list)

**flint_value.c** — one dynamic-value model (null/bool/i64/f64/str/array/object)
shared by every config format, so TOML and JSON are walked with the same code.
JSON's model was private to flint_serial.c, so anything else would have had to
reinvent it. Includes path lookup, typed accessors that return a default rather
than coercing silently, an encoder, deep clone, and an ITERATIVE free (a deeply
nested config would otherwise recurse per level, and a hostile document can be
deliberately deep).

**flint_toml.c** — TOML v1.0.0: tables, arrays of tables, dotted keys, inline
tables, arrays, all four integer forms with underscores, floats with inf/nan,
basic/literal/multi-line strings with line-continuation folding, and the four
date/time forms normalised to RFC 3339.

**flint_yaml.c** — block mappings and sequences, the same-indent rule, flow
collections, literal and folded block scalars with chomping, full type set,
comments, document marker. Tags/directives/anchors/merge keys are REJECTED with
a line and column rather than silently mis-parsed.

Ten bugs found by testing, all of which had produced a WRONG TREE while still
reporting success — the dangerous class of parser bug:

- `flint_value_obj_set` grew keys and values from ONE shared capacity counter,
  so the first insert allocated keys but not values, then wrote through NULL.
- `_descend` stored values under the key `""`, losing every field.
- The same function wrapped the FINAL key in a table: `a = 1` became {a:{a:1}}.
- `_key_part` reported a dot without consuming it, breaking every dotted key.
- `_inline_table`/`_array` were entered with the cursor still on the bracket;
  the array case recursed until the stack died.
- `inf`/`nan` were unreachable — the scan took `i` and `n` but not `f`.
- `_split_kv` truncates the line at the `:` in place, so a YAML flow value was
  read from a string that no longer contained it.
- A YAML flow collection had no stopping point and consumed every later key.
- `_block_scalar` consumed its body but the caller resumed past the header.
- The YAML single-quote loop tested for the closing quote before the `''`
  escape, so `it''s` came back as `it`.

**A design note worth keeping:** keys written after a `[[table]]` header belong
to that table until the next header. That is TOML's rule; the parser was RIGHT
and my first test document was wrong. Worth stating, because that is exactly the
kind of thing that gets "fixed" in the wrong place.

**Method:** every one of these was found by a test that asserted on the parsed
TREE, not by reading the code. Each parser looked plausible and produced
confident nonsense. Write the assertion first.

Gates: v1 12/12, stdlib-ai 17/17, parse-gate 104, ladder 21/21, selfhost
byte-identical.

**Still on the gap list:** timezone-aware datetime, async/event loop,
iterators or generators, compression, a test framework with assertions and
mocking, coverage, a linter.
---

## 2026-10-10 — Documentation accuracy, and a JIT performance gap

**Docs said things that were false.** REQUIREMENTS.md documented
`./flintc --target <triple> program.fl`; CHANGELOG.md 0.22.0 listed
`--emit-header` and a WASM target. Verified none of the four appears in
src/main.cpp, and README already said so correctly -- so the docs contradicted
each other. Both corrected. This had been flagged in a review two sessions
running; "still there" was accurate, and the fix is to make the false claim
unrepeatable rather than re-noting it.

**README now documents the eight new modules** (a Standard library section and
a Machine learning section), each carrying the caveat that cost real debugging
time: the printf variadic ABI, `ag_grad_id` vs `ag_track_tensor`, `ag_seed`,
`flt_matmul_blas` as the deterministic reference, TOML's `[[table]]` rule.

**Writing the docs found a real gap:** every example was compiled and run, and
`ag_seed_scalar` — implemented in C, used by the gradient checker — was never
registered as a builtin. It was unreachable from Flint. Now registered.

**JIT matmul was 11x slower than AOT and looked fine.** `flint_blas.o` reaches
`cblas_dgemm` through a WEAK symbol, so the BLAS path is taken only when the
library is loaded. The AOT link line adds `-lopenblas`; the JIT never did, so
every JIT matmul took the portable triple loop -- correct answers, no warning.
Fixed with `dlopen(RTLD_GLOBAL)`. N=200: 517 ms -> 11 ms (AOT is 7 ms).
`flint_blas_available()` is the tell: it returned 0 in JIT.

**Lesson worth keeping:** two of these three defects (the JIT BLAS gap, the
unregistered builtin) were INVISIBLE because the code produced correct results.
A weak-symbol fallback and a missing registration both fail silently. When
something is "just slow" or "should work", assert which path actually ran.

**The CI note from the review is right and reproducible:** `v1_gate_check.sh`
reports 2 failures (differential, driver) on a clean tree because the self-host
binaries are absent. Run `bash tests/test_selfhost.sh` once first; then 12/12.

Gates: v1 12/12, stdlib-ai 17/17, selfhost byte-identical, docs-check 0.

## 2026-10-10 — timezone library: tested, failed, fixed

**The timezone module (runtime/flint_time.c, new/untracked) was verified
against the libc oracle before being trusted: POSIX TZ strings as an
independent implementation of the same IANA rules (tzdb-2026e, rules
re-checked rule-by-rule), weekly sweeps 2025-2027 plus exact transition
edges. Baseline: 8641 checks, 143 failures. After: 8817 checks
(tests/time_check.c), 0 failures, ASan/UBSan clean.**

What the harness found (all fixed, none survived):
1. **Fixed-date DST transitions drift up to 6 days/year.** The table said
   "US: Mar 8 / Nov 1" every year; truth is 2nd-Sun-Mar / 1st-Sun-Nov.
   Replaced with yearly-recurrence rules (month, nth-weekday, min-day for
   Chile's "Sun>=2"), transition instants computed in UTC via the
   pre-transition offset. EU rules anchor at 01:00 UTC, so London/Dublin
   need different wall hours than Paris/Athens (01:00/02:00 vs 02:00/03:00
   vs 03:00/04:00) — one shared EU rule was an hour off for the +0 zones.
2. **Data bugs:** Denver fixed -7 (has US DST), Adelaide missing (has DST),
   NZ started DST in October instead of last-Sunday-September, Guam +9
   instead of +10, Cairo used EU dates instead of last-Fri-Apr/last-Thu-Oct,
   Casablanca used pre-2018 EU rules (IANA: fixed +0 since 2026-09-20).
   New zones: Havana, Santiago, Kathmandu (+5:45), Chatham (+12:45, :45
   transitions), Lord Howe (+10:30, 30-min DST), Adelaide, Kyiv, Tehran,
   Amman, Mexico City, Istanbul, Darwin-via-ACST.
3. **UTC-day frame near transitions:** offsets were classified by UTC
   day-of-year against local transition dates — wrong for hours around
   every spring/fall instant. Now instant comparison.
4. **Truncation, not floor, for pre-1970 epochs** in the offset path
   (gmtime already floored; the DST path did not).
5. **`flint_time_parse` ignored `zone`.** Now applies it when the text
   carries no offset (fall-back ambiguity → second occurrence, documented).
6. **`%Z` printed "DST".** Real abbrevs now (EDT/BST/CEST/AEDT/NZDT/CHADT…).
7. **Struct-by-value FFI returns segfaulted every dt call.** The C
   functions returned FlintDateTime by value while registered as pointer
   returns — t_time.fl could never have run. Aligned to the documented
   opaque-pointer design (malloc'd returns + flint_dt_free/flint_tz_free).
8. **t_time.fl's own offset-wins expectation was wrong** (09:00 vs 09:30:15
   slip) — it was never gated, so nobody noticed. Fixed + wired.
9. **Duplicate `flint_time_format`:** runtime.c's 1-arg strftime-now
   collided with the 3-arg civil version at link. Old one had zero
   callers; removed + deregistered.
10. **Header promised `flint_tzdata()`**, which does not exist. Rewritten
    truthfully, with the known limits stated (IST=India, Ramadan
    suspensions unmodellable, pre-1970 DST rules absent).

Known limits kept visible, not fixed: Morocco needs yearly decrees;
`flint_time_parse` documents its ambiguity rule; JIT cannot run util-linked
programs (missing `__aarch64_*` compiler-rt symbols in the JIT process —
pre-existing, affects t_util.fl identically, separate task).

Gates: stdlib-ai 19 passed, 0 failed (new time_check + time); smoke 20/20;
aegis 16/16; docs-check 0; selfhost untouched.
---

## 2026-10-10 — MASTER PLAN (red-teamed)

Method per item: steelman (strongest case) → attack (cost, blast radius,
cheaper alternative incl. doing nothing; objections ranked
blocker/serious/minor) → verdict keep/modify/drop/replace. Spot-checks
(grep 2026-10-10, Termux AArch64, branch inline-bounds-check):
builtins 510 `add("` (paper §4 says 484 — stale count, refresh on
submission); no `as` keyword (`as` = assembler path only, main.cpp:10382);
generics EXIST (`genericFunctions`, `generateGenericInstance`,
`typeParams`, main.cpp:4094-5163 — inventory item (3) half-false);
`flint_tz_info` in C (flint_time.c:410, flint_time.h:106) but
UNREGISTERED (registration list :4806-:4825 lacks it); no
`Asia/Jerusalem` in flint_time.c (header :21 documents absence);
Casablanca fixed +0 w/ Ramadan note (:244-248); pre-1970 unmodelled
(header :24); races admitted (README:401); `_unchecked` gated on
`releaseMode` (:4381-4383, CLI default safe :9975/:10011-10012);
gradcheck.c:83-87 covers exactly mul/add/sub/sum; no ONNX hits; no
DIBuilder hits; no `.vscode/`; no `--target` (v0.17 feature lost —
suspected wipe-class loss like AegisChecker); no i8-quant path;
COMPATIBILITY.md:13 gate + D1-D4 all OPEN verified verbatim;
stage3/ladder 45 files, test_selfhost.sh + docs_check.sh present;
LLVM 21.1.8 (`llvm-config`); quarantine ON by default (memory.md
2026-10-04, opt-out `FLINT_AEGIS_QUARANTINE=0`); `fcheckpoint_init`
exists = activation checkpointing (memory), NOT weight serialization.

### PHASE 0 — hygiene (before anything; effort S, axis Maturity/Process)
- **0a Commit or shelve the stack.** Steelman: 7 modified + 7 untracked
files; stale-artifact confusion already bit 3x. Attack: committing is
user-gated (minor — `git status` discipline suffices for now). Verdict
KEEP: rebuild + full v1 gate solo + `rm -rf ~/.cache/flintc` after kills
before trusting any number. Gate: v1_gate --full (needs selfhost first —
CI note verified 2026-10-10). §7.1 gate test applies to every phase below.

### PHASE 1 — P0 correctness (gates ALL perf per COMPATIBILITY.md:13; §7(1) ADOPT)
- **D1 for-array C++ miscompile (segfault).** Steelman: segfault on valid
code caps Correctness at 7; self-host output proves the design, and the
C++ fault is localized to the desugar/bounds region — but root cause is
still open (COMPATIBILITY.md:30).
Attack: touches parse+codegen both LLVM paths + differential manifest
(serious — 21-golden churn risk); alternative "loud error not fix" is
cheaper but keeps a valid program uncompilable (minor loss). Verdict KEEP
as FIX-REF, effort M. Gate: tests/test_differential.sh + new corpus entry.
Axis: Correctness 7→8+.
- **D2 bare `len()`.** Steelman: specified G3/G6 behavior, self-host proves
it; C++ just lacks the builtin. Attack: trivial blast radius (serious?
no — additive registration, minor). Verdict KEEP, effort S. Gate:
differential. Axis: Correctness.
- **D4 `s[1]` GEP crash.** Steelman: compiler crash, self-host returns 101
bounds-checked — fix known-shape. Attack: GEP typing touches 4 inline
sites incl. R1 shapes (serious — re-verify bounds traps byte-identical).
Verdict KEEP, effort M. Gate: differential + ladder 21/21. Axis: Correctness.
- **D3 `fa_len` message.** Steelman: convergent rejection already; only the
message is corrupt. Attack: near-zero risk (minor). Verdict KEEP, effort S.
Gate: differential `both_may_reject` message check. Axis: Correctness/DX.
- Ordering inside Phase 1: D2+D3 (S, additive) → D1 → D4 (M, codegen).

### PHASE 2 — surface syntax (gated on Phase C fixed-point + 21 goldens per §7.1b)
- **(1) `as` casts.** Steelman: `(i%100) as f64` failing blocks numeric
code; every ADOPTION cost (tensor fills, gradcheck drivers) pays it.
Attack: new keyword + parse both modes + codegen
(sitofp/fptosi/trunc/bitcast) + checker + self-host parity (serious blast:
lexer/parser token contract must change together — 2026-09-14 `+=` hang
lesson); cheaper alternative: named builtins (`i64_to_f64` exists
already!) — but casts compose in expressions, builtins don't (blocker to
deferral). Suggested order "first" REJECTED as stated: goes AFTER P0 per
gating rule, first within Phase 2. Verdict KEEP, effort M. Gate:
test_selfhost.sh Phase C byte-identical + ladder 21/21 + new goldens.
Axis: Ergonomics.
- **(2) tensor literals.** Steelman: everything via extern+C+flt_create_2d
is the steepest AI-ergonomics cliff. Attack: literal shape/dtype inference
+ nested-literal codegen + self-host parity (serious); minimal slice —
rank-1/2 f64-only literal desugaring to existing `flt_create_*`+`flt_set_at`
(minor risk, no runtime change). Verdict KEEP NARROWED to f64 rank≤2
slice; f32/i8 literal suffixes deferred. Effort M. Gate: test_ai.sh +
ladder. Axis: Ergonomics/AI-ML.
- **(3) "no generics/traits" → SPLIT.** Generics EXIST (monomorphized;
verified above) — DROP, no work, paper §2.1 Liu-warning noted for future.
Traits/protocols: steelman says ecosystem needs them; attack says whole
checker interaction surface + Liu et al. rot risk (blocker) — DEFER with
reason (no design, no corpus). Axis: none moved.

### PHASE 3 — training readiness (paper's thesis; §7 items reconciled)
All Phase-3 Performance-axis work (observability, i8 spike) is gated on
Phase 1 closure (zero open P0s per COMPATIBILITY.md:13), like Phase 5.
- **Observability, NOT tiering (§7(2) ADOPT verbatim).** Steelman: JIT
silently ran portable loop 11x slower once — counters plus per-call path
labels make the next silent-slow path visible the same day, not
discovered by accident. Attack: counters
+ per-call backend labels via `flint_blas_available` probe is additive
(minor); tiering would need a second compile tier that doesn't exist
(blocker to tiering — correctly excluded). Verdict KEEP narrowed slice,
effort M (counters on both BLAS/portable branches with thread-safety
scope, a Flint-callable path-label builtin + self-host entry + docs,
JIT-vs-AOT assertions in test_ai.sh). Gate: test_ai.sh + documented path labels. Axis: Performance
(honesty, not speed).
- **Gradcheck widening (§7(3) ADOPT).** Steelman: only mul/add/sub/sum
finite-differenced; matmul/mean/relu/sigmoid ride on e2e tests. Attack: one
rule at a time is bounded (serious only if rushed — tolerance/step per
dtype, f32 lesson 2026-10-10). Verdict KEEP, order matmul→mean→relu→
sigmoid, effort M. Gate: tests/gradcheck.c 5/5 per dtype. Axis: AI-ML
correctness.
- **Model serialization/checkpoint FORMAT (new — §7 omission, justified).**
Steelman: training readiness 4/10 cannot persist weights; ExecuTorch also
lacks it (§2.3) so this is parity-plus, and fine-tune checkpoints are the
niche's artifact. Attack: format design (dtype+shape+data+version,
writer/reader, round-trip gate) is M with minor blast (runtime-only, no
codegen). Cheaper alternative (dump via existing serial) lacks versioning
(serious for compat). Verdict KEEP (added to §7 with reason), effort M.
Gate: new test_ai round-trip (save→load→bit-identical + version-reject).
Axis: AI-ML/Maturity. Suggested order "second" ACCEPTED as Phase-3 head
after observability.
- **i8 quantized path → SPIKE then decide.** Steelman: mobile-VLM norm is
8-bit (paper §2.3: Llama-Mobile 8-bit activations). Attack: needs QAT/FP
source story + mixed-precision rules (FlexiQ/SliM-LLM precedent); full path
is L with serious checker/blast surface; a matmul-only s8 spike measures
the ceiling first (minor). Verdict MODIFY to S-spike (defer full on <2x
measured or >5% accuracy cost). Gate: spike benchmark vs f32. Axis:
Performance (deferred).

### PHASE 4 — time module (bounded table work beats research programs)
- **`flint_tz_info` registration — the 3-line win.** Steelman: C done,
header done, only `add()` + self-host `e_rt_sig` missing. Attack: ~zero
blast (minor); note the gate (`t_time.fl` run) needs the JIT compiler-rt
gap (17) fixed first for its JIT half, and the void-with-`FlintZone*`-
outparam shape needs an expressibility check against the struct-by-value
FFI precedent. Verdict KEEP, effort S. Gate: t_time.fl run. Axis: Stdlib.
- **Asia/Jerusalem.** Steelman: header documents the hole; Israel rules are
fixed-pattern (Fri/Sun-based), one table entry + tzdata + time_check cases.
Attack: minor code (data-only) but the verification is M — rule research
against tzdb plus transition-edge cases in tests/time_check.c in the style
of the 8817-check harness. Verdict KEEP, effort M — do BEFORE any
capabilities work (Jerusalem S vs capabilities L is the cost-effectiveness
argument). Gate: tests/time_check.c 0-fail. Axis: Stdlib.
- **Casablanca/Ramadan + pre-1970 DST → DEFER with reason.** Yearly decrees
and historical rules are data-unmodellable by fixed rules (documented
limits flint_time.h:20-24); doing them wrong is worse than documenting
(blocker to correctness). Keep documented. Axis: none.
- **t_time.fl JIT compiler-rt gap.** Steelman: pre-existing, also breaks
t_util.fl; util-linked JIT silently unusable. Attack: link compiler-rt
builtins into JIT process or re-route lowering (M, serious — JIT loader
surface). Verdict KEEP, effort M. Gate: t_time.fl + t_util.fl JIT green.
Axis: Portability/Tooling.

### PHASE 5 — perf, safety-hardening, tooling (all gated on zero open P0s)
- **Runtime sets: KEEP AS-IS, no work.** Data-race freedom: DEFER (needs
thread/ownership model; README admission stays). `_unchecked` gating:
correct — DROP from plan. Quarantine: already on-by-default; remaining work
is §7.2-Item-1 strictness measurement (budget/poison/configure + aegis
gate) — ADOPT narrowed, effort S/M. Gate: test_aegis.sh 16/16. Axis: Safety.
- **§7.2-Item-2 arenas narrowed: ADOPT as sequenced** (one matmul temporary
→ farena scope + reset assert, §7(2) labels separate effects, ai+selfhost
gates). Effort S. **Item-3 liveness-sensitive join: ADOPT** (behind existing
error strings, ladder+aegis as fleet corpus, divergences to
COMPATIBILITY.md). Effort M. Residual obligations (FFI/int-launder/
unchecked/races uncovered) carried into paper claims. Axis: Safety/Perf.
- **DWARF before VS Code extension (REPLACE suggested order).** Steelman for
ext: adoption lever (Mojo lesson §2.4). Attack on ext-first: TS packaging +
marketplace + phone-hostile verification = M/L with serious maintenance
tail; DWARF line tables via DIBuilder is standard API, moderate work, and
serves EVERY editor/lldb user (minor blast: additive AOT emission +
`--debug` implying `--no-strip`, both new subtasks: the flag does not
exist yet). Verdict: DWARF effort M first (gate: CI ubuntu/macos job maps
one Flint frame to source via gdb/lldb — no gdb/lldb exists on the Termux
dev phone, so this gate never runs on-device), extension deferred to
adoption phase (gate when taken: bundled LSP + per-editor docs). Suggested
"extension third" OVERRIDDEN with reason. Axis: Tooling.
- **`--target` restoration.** Steelman: v0.17 shipped it, docs corrected
2026-10-10; cross-compilation is a §1 non-goal exception worth re-adding
boundedly (flag + validation + triple flow). Attack: suspected wipe-loss
means re-implementation against current backend (M, serious — CGU/link
flow). Verdict KEEP, effort M. Gate: x86_64 object on ARM host
(readobj-confirmed, v0.17 precedent). Axis: Portability.
- **ONNX export: DEFER, and the deferral is CORRECT.** Niche claim is
no-artifact-compiler training (§2.3); ONNX is interop-out, not readiness;
costs a spec-conformance surface for no readiness movement before native
checkpoints exist (export would serve the inspectable-weights clause —
revisit on external demand).

### PHASE 6 — docs & process (cheap, submission-blocking)
- **CHANGELOG time/numpy/matmul entries: effort S** (untracked
benchmarks/matmul_np.py is the numpy mirror — confirm + log). Gate:
docs_check.sh 0 fails. Axis: Docs.
- **§7(4) NIST rigor + §7(5) SLSA ladder: ADOPT sequenced** — benchmark
methodology (ranges, interleaving discipline) before provenance climb
(zero→provenance); Mojo stability doc as versioning model. Effort S/M.
Gate: §5.1 ranges + SLSA level documented. Axis: Maturity.
- **§7(6) PoisonCap evaluation: ADOPT as evaluation-only** (no CHERI
hardware on phones per §7.1 layer test — method precedent, not port).
Effort S. Axis: Safety (directional).
- **Paper third-party re-fetch (item 22): pre-submission GATE, not a phase**
— §2 lead + §9 already flag drift risk; re-verify scores, dates, API
surfaces, survey characterizations (~25 URLs, unstable web, half-day
minimum). Effort M (external). Gate: every URL +
access date refreshed. Axis: Docs.

### DROPPED / DEFERRED (one-line reasons)
- Generics work: DROPPED — C++ path has an i64-only monomorphization
stub (examples/generics.fl passes; f64 instantiation is a loud rc=1
error; self-host rejects with "generics need G6"); full generics need a
design first, and Liu-et-al. rot risk (§2.1) argues against rushing one.
No work now.
- Traits: DEFERRED — no design; trait×lifetime interactions rot checkers.
- Data-race freedom: DEFERRED — needs thread ownership model; admission stays.
- `_unchecked` changes: DROPPED — gating already correct.
- Full i8 quant: DEFERRED past spike — needs QAT story; spike decides.
- ONNX export: DEFERRED — interop-out, no readiness movement before
native checkpoints exist (would serve inspectable-weights; revisit on
external demand).
- Casablanca/Ramadan rules, pre-1970 DST: DEFERRED — unmodellable by fixed
rules; documented limits beat wrong tables.
- VS Code extension: DEFERRED past DWARF — heavier, phone-hostile to verify.
- Valen/Pure-Borrow/Hylo/HW-PoisonCap/meta-tracing JIT/Austral-end-state/
Polonius-reuse: NOT FEASIBLE per §7.2 (adopted verbatim).
- Tiered JIT: EXCLUDED per §7(2) — single ORC compile stands; observability only.

### §7 RECONCILIATION (adopt / correct / supersede)
(1) D1/D2 first: ADOPT (+D3/D4 bundled, same gate). (2) Observability-not-
tiering: ADOPT verbatim. (3) Gradcheck widening: ADOPT (order matmul first).
(4) NIST rigor: ADOPT (methodology before provenance). (5) SLSA ladder:
ADOPT (process-level, before code-level capabilities). (6) PoisonCap:
ADOPT as evaluation-only. §7.1 four tests (layer/fixed-point/gate/
one-device): ADOPT as entry criteria for every phase above. §7.2 Item 1
(quarantine strictness): ADOPT narrowed (already on-by-default; measure +
configure). Item 2 (arenas narrowed): ADOPT sequenced. Item 3 (liveness
join): ADOPT (ladder+aegis as corpus). OMISSION CORRECTED: weight
checkpoint format added (Phase 3, reason recorded). SUGGESTED ORDER
OVERRIDDEN: syntax (Phase 2) follows P0 (gating rule); checkpoint follows
observability+gradcheck-start (paper order); DWARF precedes extension
(leverage-per-effort). Builtin count 484→510 refreshed. Out of scope for
this red-team pass (prior verdicts stand): Cranelift LOW, MTE watch-only,
R6/R7 sequenced post-P0, and the rest of the 2026-09-13 SPEED tail.

## 2026-10-17 — item-22 third-party re-fetch: COMPLETE, content spot-verified

All 34 unique URLs of docs/RESEARCH-PAPER-DRAFT.md re-fetched (yesterday:
2026-10-17): 30× HTTP 200; 2 ACM DOIs live via doi.org 302 (dl.acm.org
bot-blocks scripted reads — annotated in the draft); ecoop.org hangs from
this network (server-side; only URL not re-verified — both stamps marked
[Unverified on 2026-10-17], dates intentionally NOT bumped). Content
spot-checks vs claims: Rust blog Polonius-on-nightly Aug 4 2026 intact;
Mojo stability.md (semver/@stable/ABI-unstable) unchanged; ONNX Runtime
two-phase training doc unchanged; ExecuTorch README inference-first +
versioned .pte contract intact; 11 arXiv page titles all match their
cited claims (incl. Llama-Mobile 2.7-bit, Pure Borrow, PoisonCap,
Reconsidering-Custom-Memory-Allocation reconsideration). Access dates
bumped 2026-10-10 → 2026-10-17 on the 28 verified stamps. Pre-submission
content gap that remains (recorded, not blocked): paywalled effect sizes
(CACM 48%→57%) and mlsys PDF pages stay "cited, not independently
verified".

## 2026-10-17 — RUST-INJECTION ADVICE: verified, resolved (external suggestion)

Two sourced-advice blocks proposed injecting Rust: (a) runtime/stdlib,
 optimization passes, tooling/LSP; (b) fuzz pipeline, package manager,
 async runtime, target lowering. Each claim checked against the repo
 (grep + probes, 2026-10-17). Store outcome, not flattery.

Repo facts answered (their two questions):
- Memory model: NO GC, no RC — manual with compile-time ownership/move
  borrow checks (README:68) + runtime Aegis generational leases/quarantine
  (W^X-style, ON by default). Borrow checker is compile-time; Aegis is
  compile-time passes (AegisChecker, restored 2026-10) + runtime checks in
  emitted IR (inline bounds/panic split, quarantine).
- Compiler: Flint self-host pipeline emits LLVM IR DIRECTLY (stage1 lex ->
  stage2 parse -> stage3 emit, byte-identical fixed point, 21 goldens);
  NO C/C++ transpile stage. The canonical flintc (C++/LLVM 21) remains the
  registry + JIT; self-host covers the compile pipeline for the supported
  surface (per-aspect ledger: this is a fixed-point discipline, not a
  verified "95%" number — the 95% framing is unmeasured, do not use it in
  docs).

Claim-by-claim verdicts:
1. "C++ and Rust both emit LLVM IR, interoperate seamlessly" — PARTIALLY
   TRUE at object/LTO level, but the load-bearing question is toolchain
   availability: VERIFIED cargo/rustc NOT installed on the build device
   (which/cargo -> nothing). Advice assumes a host with rustc; Termux
   rust exists via pkg but would add a second bootstrap toolchain that
   contradicts the scored moat (single-binary, no external toolchain;
   ecosystem 2.5 pain is REAL but buying it with a cargo dep swaps one
   weakness for another).
2. "Runtime/stdlib in Rust (#[no_mangle] extern C, link .a)" — CONCEPT
   SOUND (C ABI is the boundary runtime already uses: flint_* symbols +
   clang link @263-276). But on-device it stalls without rustc; and the
   Rust in stdlib today would replace C runtime that the eight gates
   already verify. Verdict: DEFER until toolchain + reproducible artifacts
   exist on host; revisit only for modules where C is the bottleneck,
   never for JIT-resolvable atoms (they must stay in runtime.o loaded by
   addObjectFile main.cpp:404-440).
3. "Custom LLVM opt passes via llvm-sys" — RULED OUT: experimental
   llvm-sys + custom passes contradicts §7(2) (single ORC compile, no
   tiering) and the §7.1 gate (no added hidden compiler stages). Aegis/
   borrow pass evolution stays in-tree C++ until the ledger's DWARF item.
4. "tower-lsp tooling" — REAL need (tooling 4.5) but sequenced AFTER
   DWARF per red-team; rust would be a second concern at once. LSP could
   equally be a flintc feature (JSON diagnostics): no code exists yet.
5. "Rust differential fuzzer / cargo-fuzz" — the DIFFERENTIAL HARNESS
   ALREADY EXISTS (tests/test_differential.sh, manifest, 11 files, plus
   the session-driven adversarial hunt). Risk of libFuzzer-on-Android
   availability unverified; a generator script (any language, even .fl
   itself) needs no new toolchain: CHEAPER alternative preferred.
6. "Rust package manager / build tool" — SUPERSEDED by build.sh +
   spawnLinker (main.cpp:255) which already: builds stdlibs on demand,
   probes OpenBLAS, links pyruntime, --link flag support. A SAT-resolver
   package manager is a REGISTRY IDEA (ecosystem), toolchain-inolved —
   recorded as want-not-now; decision language: adopt the FEATURE
   (dependency resolution) not the LANGUAGE (Rust).
7. "Rust async runtime / work-stealing pool" — flint_chan.c (CSP) +
   async G7 already exist in C; parallel-for works (benchmarks). Rust
   tokio dep on Android compiles huge; races remain a documented DEFER.
8. "Target-lowering engine in Rust" — MISDIRECTED: --target was a v0.17
   flintc feature that got lost (ledger item 20); it is plain C++ work in
   main.cpp, no Rust involved.
9. "cxx crate bridge" — NOT NEEDED at our boundary: the entire runtime
   boundary is C ABI (extern "C"); cxx pulls cargo + its own codegen into
   the build for no benefit over plain C headers already in use.
10. Flattery corrections ("Rust-level security"): per-aspect ledger has
   Flint 7.0 vs Rust 8.6; claims are artifact-anchored (panic-not-UB,
   differential 11/0), NOT Rust-parity claims — keep phrasing
   "panic-not-UB on the modeled surface", not "Rust-equivalent safety".

NET DECISION (stored): No Rust anywhere in the bootstrap or runtime for
now. Adopted ideas transferred: (a) the fuzz-by-generation idea is ROW'S
step (extend differential corpus programmatically later, toolchain-free);
(b) package-manager FEATURE idea parked in ledger Phase 6 list;
(c) LLVM-only boundary maintains "single toolchain" moat. Re-eval
trigger: a host install of rustc + demand for a Rust-only library AND
already-shipped DWARF/--target items (per red-team order).

### WAVE E — CROSS-LANGUAGE COMPARISON + SCORE (2026-10-16, no-subagent, evidence-locked)

Method: every score anchored to a verified artifact (gate output, benchmark,
commit, or documented failure) — no vibes. Anchors: differential 9/0
(3 new corpus files), smoke 23/0, stdlib-ai 25/0, ladder 21, selfhost
Phase C byte-identical, docs_check 0, lexdiff 108/0, aegis 16/16,
time_check 8817 checks/0, matmul N=500: Flint AOT 42.57ms ≈ numpy 2.4.4
42.66ms vs naive C 428.38ms (same-session), JIT+BLAS 45.8ms (was 517.9),
510 builtins, ASan/UBSan-clean time runtime.

Per-aspect /10 (Flint, evidence):
- Correctness/safety 7.5 (+0.5) — map-read P0 (ICmp abort) FIXED; NaN/±Inf
  f64→i64 now PANICs (vs C unset/UB, C++ value-poison); array OOB &&
  finite-overflow-adjacent edges loud; D1–D4 FIXED. Still open: finite
  1e19 as i64 fptosi poison (documented), m[0] garbage-read (unspecified).
- Performance 8.5 — ≈numpy (BLAS-backed) N=500; f32 1.6–1.9× verified;
  JIT single-compile only (no tiering, by design §7(2)).
- Expressiveness 7.0 (+0.5) — as-casts (chained/precedence verified),
  f64 rank≤2 tensor literals, match/lambdas; generics i64-only stub
  (loud fail), traits absent.
- Ergonomics 7.0 (+0.5) — map read works; map-assign still
  "requires an array variable"; loop-var casts fixed; clear panic/error
  text verified line-accurate.
- Stdlib breadth 5.5 — 18+ runtime modules, tz (~100 zones, documented
  Jerusalem/pre-1970 gaps), gradcheck mul/add/sub/sum only; no
  serialization; no HTTP/image/audio.
- Ecosystem 2.5 — no packages, no registry, C-ABI FFI both directions
  (works: flt_*/libBLAS dlopen), nothing else.
- Tooling 4.5 — no DWARF (planned first), no --target (lost v0.17),
  JIT can't run util-linked (compiler-rt atomics gap), no analyzers;
  differential-as-infrastructure is the offsetting strength.
- Docs 7.5 — truth-gated (docs_check hard counts), honest limitations
  sections; README/paper stale-count refreshed required on submission.
- Testing rigor 9.0 — differential C++↔self-host manifest, fixed-point
  byte-identical, goldens+counts, hostile-hunt culture; 2 P0-class found
  by independent hunt this session and fixed same-session.
- Process/maturity 6.0 — uncommitted stack (Phase 0 hygiene), no CI
  runner here, gates runnable solo.

Opponents (paper §anchors, unchanged): Python 8.7 (ecosystem 10,
performance 5, safety 6), Rust 8.6 (safety 10, ecosystem 8,
performance 9, ergonomics 6), C++ 7.7 (perf 9, ecosystem 9,
safety 3, tooling 8), Go 8.2 (ergonomics 9, perf 7, safety 7),
C 6.9 (perf 9, safety 3, ecosystem 7), Mojo 7.0 (ML perf 9,
stability 4). Flint's moat vs all: differential self-host fixed point +
quarantine-by-default + panic-not-UB brand + single-binary no-toolchain
mobile/dev-host story; not close on ecosystem/tooling.

**FINAL: Flint 7.0 / 10 (up from 6.8)** — Path to 8.0: checkpoint
format + gradcheck widening (stdlib breadth), DWARF + --target
(tooling), generics design spike; Path verifies via the same seven
gates. P2 leftovers accepted: `as`-as-identifier OK (verified), QBE
stubs, mixed-binop divergence (disclosed), map-assign.


Keep 12: (1) as-casts, (2) tensor-literals narrowed f64-rank≤2, (5)
tz_info registration, (10) checkpoint format, (12) observability slice,
(14) gradcheck widening, (17) JIT compiler-rt gap, (19) DWARF, (20)
--target restoration, (21) CHANGELOG entries, (22) paper re-fetch gate,
(23) uncommitted-stack hygiene. Modify 3: (4) second-class references /
borrow diagnostics (diagnostics-first sequencing), (8)
quarantine→strictness measurement, (11) i8→S-spike. Split 2: (3)
generics-DROP (i64-only stub, no design) / traits-DEFER, (15)
Jerusalem-KEEP / Casablanca-DEFER.

**2026-10-17 LEDGER PROGRESS ( Waves 1–4 of the remaining set):**
(5) tz_info registration — CLOSED: scalar accessors
    flt_tz_is_dst_at/flt_tz_abbrev_at wired, t_time covers, ai 26/26.
(17) JIT compiler-rt gap — CLOSED: -mno-outline-atomics in build.sh
    CFLAGS_RT, runtime rebuilt; JIT t_util/t_time pass; run.sh 25/25.
(21) CHANGELOG — CLOSED: Added/Fixed entries for casts, tz, JIT, maps,
    casts.
(10) checkpoint format — CLOSED: FLWT v1 in runtime/flint_tensor.c
    (flt_weights_save/load/count, little-endian, dtype/rank/dims/data
    records, rank 1-5 f64 / 1-4 f32, header count stays 0 and count
    re-walks); registered with tPtr signatures; tests/t_chkpt.fl
    round-trip green on BOTH engines (JIT + clang AOT with
    runtime+flint_aegis+flint_tensor+flint_blas+flint_nn); test_ai 26/26;
    CHANGELOG entry added; no stage edits → no self-host retest needed.
(14) gradcheck widening — CLOSED: tests/gradcheck.c now 53 checks from 10
    (matmul f64+f32, 7 unaries/mean incl. relu kink), 0 failed,
    "PASS gradcheck" gate unchanged. Verified the tape implements
    AG_DIV/AG_SQRT/AG_POW/AG_RESHAPE/AG_TRANSPOSE only as enum names —
    no functions exist — so 53/53 IS all implemented ops (enum-only
    leftovers are design debt, note in next review).
(20) --target restoration — CLOSED (emit stage, was "introduction" not
    restoration: crosses-checked, the flag never existed in-tree; docs
    verified this first). Codegen takes a triple override (ctor), which
    sets triple + datalayout FROM A REAL TargetMachine (initTargets +
    lookupTarget + createTargetMachine; std::nullopt opt) — x86_64 and
    wasm32 datalayouts byte-match clang's canonical strings; .o paths emit
    real objects (wasm magic 0061736d01000000 verified via od). Guard:
    --target && runMode → rc 1 "requires an emit output; JIT execution is
    host-triple only". cacheSalt extended (-tgt<spec>) so host and cross
    modules never share cache entries. tests/test_target.sh 9/9.
    fingerprints: --target x86_64-pc-linux-gnu; --target wasm32-unknown-wasi.
    Execution (WASI sysroot/_start/runner) stays T3 by docs/tiers.md truth.
    Docs updated: tiers table/probe/promo-3, IMPROVEMENT-PLAN, README, CHANGELOG.
(19) DWARF line tables — CLOSED at the artifact stage. --debug builds a
    DIBuilder CU + file + per-user-fn DISubprogram + per-statement
    DILocation (emitStmt and emitExpr both stamp, so block-tail statements
    map too), module passes drop to O0 under --debug (O2 const-folds
    statements away and the line info with them), and Debug Info Version
    module flag added (cache round-trip + object emitter dropped DI
    without it, warning "invalid version (0)"). Oracle: llvm-dwarfdump —
    tests/test_dwarf.sh 16/16 (CU, subprograms, line table rows, source
    lines 4/5/8-12 of tests/t_debug.fl, artifact executes). SCOPE TRUTH
    discovered: ORC JIT with a debug module hangs at mainFn (reproduced
    with strip + no-opt; only common variable = DIBuilder metadata), so
    --debug requires -o and bare-run refuses loudly (mirrors --target
    guard) — DWARF belongs to artifacts, which run end-to-end. JIT
    strips debug metadata defensively in runWithJIT. Remaining open
    (recorded in IMPROVEMENT-PLAN): lldb/gdb driver validation, VS Code
    extension stays DEFERRED past this.
(22) paper re-fetch gate — CLOSED (see its section above, 2026-10-17).
(23) uncommitted-stack hygiene — CLOSED: battery green fresh first
    (smoke 25/25, differential 11/0, stdlib-ai 26/26, --target 9/9, dwarf
    16/16, selfhost stable promote OK, docs-check 0), then the full
    2026-10-15→17 stack committed in three theme commits — dc41e7b
    (runtime+pipelines+gates, 31 files), 809a172 (compiler: casts,
    panics, maps, --target, --debug, registrations), d26ca2e (docs, paper
    draft, ledger); .gitignore gained compile_commands.json; `git status`
    clean after. Phase-0a lesson applied: ~/.cache/flintc purged before
    the --debug cache experiments.

Keep-12 + Mod-3 + Split/Drop/Defer verdicts are now ALL executed or
recorded as decisions (2026-10-17). Future work lives in the
DROPPED/DEFERRED list above (design/demand/rustc-host triggers), not in
this ledger.

**2026-10-17 EXTERNAL AUDIT (ultracode wave, hostile-neutral, logs
.audit/):** all 7 named gates re-ran green independently (smoke 25, diff
11, ai 26, dwarf 16, target 9, docs 0, aegis 16) AND matmul re-run
43.0ms ≈ numpy 42.2ms N=500. New verified facts the ledger lacked:
[1] tests/test_fmt.sh RED (stage1-3 canon files need-format — fmt gate
broken by 809a172 without re-format), [2] tests/test_errors.sh RED
(12 new as-cast/tensor-map diagnostics missing from docs/errors.md,
catalog claims 116 vs 128 emitted), [3] NO LICENSE file (third-party
reuse legally blocked), [4] registry fetch has ZERO hash/checksum
verification (rg 9431-9666 = none; 2026 npm/supply-chain incidents make
this a live risk), [5] C→Flint FFI untested (Flint→C proven via
ffi_demo), [6] CI workflow flint.yml omits ai/aegis/dwarf/target gates,
[7] flint-lsp hardcodes /tmp temp path (Termux risk), [8] flintc has no
--help/--version, [9] README self-contradicts: --target recipe at :347
vs "do not exist" list :363+, [10] bench/compile_time unfinished.
Deposit this basis (mine, dual-weight): adoption 6.45 / moat 7.02 —
the 7.4 rode generous testing(9.0→8.5)/process(8.0→7.0)/docs(7.5→7.0)/
ecosystem(2.5→2.0); stdlib 5.5→6.0 (net+crypto+serial are real). Fix
path (S-first): LICENSE → fmt 3 canon files → errors.md regen (128) →
README gap-list surgery → --help/--version → registry content-hash MVP
→ git-URL fetch test → TLS libcurl → sqlite/csv/image modules →
reverse-FFI test → LSP test+TMPDIR → CI set alignment. Garbled tail of
the ultracode output (--emit-core, "-guard nom 624", 6.60) = drifted
echo, REFUTED, not in repo.

**2026-10-17 QUICK WINS + RUST PHASE (all gated, commit 716f38f + the
rust-ffi commit):** Apache-2.0 LICENSE added (audit item 1; the repo had
none, so third parties could not legally reuse Flint); flintc --help/
--version implemented (usage read from the parser so it cannot drift;
smoke 25 -> 27, docs_check wants updated 53->63/114->124);
flint-fmt applied to stage1-3 canon files (fmt gate was RED since
809a172; whitespace-only, lexdiff 112/0 + fixpoint 113 + selfhost
byte-identical); docs/errors.md regenerated from stage3 (116 -> 128) and
test_errors.sh gained --fix so the catalog can never drift again; README
tooling section corrected (--target DOES exist; output extension, not
--emit-llvm, picks .ll/.o/binary; --test DOES run tests — both claims
were stale). RUST 1.99.1 (pkg) then used where it actually pays, NOT as a
port: (a) tests/test_rust_ffi.sh 8/8 proves the C-ABI in BOTH directions
— flintc emits user fns as unmangled globals so a main-less .fl links
straight into a Rust binary that calls add() and flint_str_length(), while
a rustc staticlib is linked BY Flint via --link; (b)
tests/test_rust_oracle.sh 2/2 compares RESULTS (not timings) against
Rust mirrors with checked arithmetic — it caught my own inverted sieve on
first run (9335420 vs 664579). Rust rewrite of src/main.cpp or runtime/
remains REJECTED: it would destroy the differential C++-vs-self-host oracle
and the self-host fixed point for no measured gain (runtime perf is
LLVM/BLAS-bound, not host-language-bound).
Drop 1: (9) `_unchecked` (gating already correct, no work). Defer 5: (6)
races, (7) capabilities / supply-chain sandboxing (boundary-only idea
stays in §7(5) SLSA context), (13) ONNX, (16)
pre-1970. 12+3+2+1+4 = 22 ✓. (18) VS Code extension LIFTED OUT OF THE
DEFERRAL 2026-10-17 — delivered as editors/vscode/ with a Rust server
(see the LSP section at the end of this ledger). D1-D4 (from
CONSTRAINTS, not the inventory) all KEEP in Phase 1; §7(4)(5)(6)
NIST/SLSA/PoisonCap ADOPT in Phase 6. Corrected 2026-10-10 after hostile
re-check caught a miscount (14/3/3/6 = 26) in the first draft.

**2026-10-17 RUST LSP + VS CODE EXTENSION (user directive: Rust wherever it
pays, no subagents):** the Python flint-lsp is DELETED, replaced by lsp/ — a
Rust binary with one dependency (serde_json). 21 cargo unit tests (framing
incl. split/malformed frames and UTF-8 byte-length headers, diagnostics parsing
against real compiler output, analyzer) + tests/test_lsp.py driving the real
binary over pipes (31 protocol checks) + tests/test_lsp.sh wrapping them with
TWO DRIFT GATES: server keywords == compiler lexer keywords (30 == 30) and
server builtins == compiler registrations (515 == 515, regenerable via
tools/gen_lsp_builtins.py --check). editors/vscode/ ships manifest + TextMate
grammar (26 patterns, keywords GENERATED from src/main.cpp) + language config +
a dependency-free stdio client, verified by tools/check_vscode_extension.py
(fails on keyword drift in either direction).

Hard-won details worth not relearning:
- The gate once tested a STALE binary because it only built when the file was
  missing; it now rebuilds whenever any lsp/src/*.rs is newer than the binary.
  Phantom protocol failures followed immediately.
- A compiler that cannot be spawned must publish ONE explicit diagnostic.
  Returning zero diagnostics is a false all-clear — the worst failure mode an
  editor can have.
- The emitter's `codegen:` errors carry NO position; they are reported at line 0
  labelled `emitter:` rather than given an invented location.
- LSP positions are UTF-16 code units, so columns must be converted, not counted
  in bytes. ASCII sources hide this bug completely.
- Grammar extraction lesson: scanning a raw regex pattern for words reads
  `\bpython\b` as the words "b" and "bpython"; strip escapes first.
- RAM note: this Termux has NO root (su blocked, pm trim-caches denied,
  dumpsys returns nothing), so caches cannot be dropped from the shell at all;
  the largest resident process was opencode itself (755 MB).

**2026-10-17 FIRST RUST STDLIB MODULE (CSV) — the Rust decision, tested:**
the user's instruction was "use Rust instead of C/C++ for stdlib". Two measured
facts shaped the design, and both were counter-intuitive:
- Rust `std` CANNOT be linked into Flint's JIT: std resolves TLS handles via
  libc, and inside JIT memory Android aborts the process with
  "increment_dso_handle_reference_counter: Couldn't find soinfo by dso_handle".
  So the module is #![no_std] (arena, no allocator, no TLS) and loads as a
  PLAIN OBJECT like the C modules — no dlopen, no ORC symbol plumbing.
- TWO Rust staticlibs cannot share one link: std symbols collide
  (rust_eh_personality, std::panicking::EMPTY_PANIC). no_std modules remove
  that, so a user's own Rust .a still links alongside Flint's.
- Flattening the archive needs SEEDED `ld -r -u <exported symbols>`, not
  --whole-archive: whole-archive drags in every compiler_builtins member and the
  JIT cannot resolve them. 11.6 KB / 3 undefined libc symbols vs ~2 MB / 800.
- Trap: a no_std #[panic_handler] using `svc #0` is delivered by Android as
  SIGSEGV with no message — it turned an ordinary assertion failure into an
  unattributable segfault. Use libc `abort()` via extern "C".
- Trap: tests must not stack-construct Doc (~41 KiB even scaled down for test);
  it overflows the test harness threads here. Tests heap-allocate.
- Trap: `ld -r` without -u pulls NOTHING out of an archive (no undefined
  symbols to resolve) and yields an object with no symbols at all.
- Trap: ORC in LLVM 21 has no DynamicLibrarySearchGenerator::GetProcessSymbols
  and no absoluteSymbols in Core.h (my training memory was stale). The pattern
  that does exist: llvm::orc::absoluteSymbols(SymbolMap) in
  ExecutionEngine/Orc/AbsoluteSymbols.h, keyed by pool.intern(name).
  (Not needed in the end, because no_std loads as a plain object.)
- GATE lesson: differential went 1/10 after a compiler rebuild because the
  self-host binaries under $TMPDIR/slipstream/self were invalidated. Rebuild
  with `bash tests/test_selfhost.sh` before judging it a regression.


---

# Capability Audit and Improvement Plan (2026-10-18)

**Method note (read this before trusting any doc).** `memory.md`, `ROADMAP.md`
and `README.md` have all been caught containing false claims about this
compiler. Concrete instance: docs asserted "match with `{...}` block arms
aborts the compiler (P0)". Running `examples/enums.fl` prints `42 99` and
exits 0. Every finding below was produced by COMPILING AND RUNNING small `.fl`
programs, or by reading the cited `path:line`. Claims carried over from the
docs are marked REFUTED. Trust the code, never the prose.

**Resource note.** The audit device had ~1 GB free RAM of 3.6 GB. Runs were
sequential; no parallel builds. If you re-verify, free
`runtime/rust/target` and `lsp/target` first (317 MB + 76 MB on disk).

---

## 1. VERIFIED BASELINE — what genuinely works

Do not "fix" these. Several look like bugs in the docs and are not.

| Feature | Evidence |
|---|---|
| Enum payload match, block arms | `examples/enums.fl` prints `42 99`, exit 0 |
| Struct methods, struct args/returns | `getx<T>` over `P{x}` prints `3` |
| Ordinary `f64` functions | `fn id_f64(x: f64) -> f64` prints `2.5` |
| Pipe lambdas WITH capture | `f = \|x: i64\| x + 1` prints `42`; captured `base=10` prints `15` |
| Bounds checking | `a[5]` on len 3 -> `PANIC: index 5 out of bounds for length 3` |
| Division by zero | `PANIC: division by zero` |
| Use-after-free | `examples/aegis_uaf.fl` -> `aegis error in 'main': lease used after free` at COMPILE time |
| Self-hosting | `tests/test_selfhost.sh` byte-identical fixed point |
| Rust stdlib in both JIT and AOT | `tests/test_rust_stdlib.sh` 20/20 |

The safety story is real and strong. Bounds checks, division guards and
compile-time use-after-free detection all work. That is the core asset.

---

## 2. ARCHITECTURE — why the bugs exist

Understand these five facts and every bug below becomes predictable.

**2.1 `Type` has no size field.** `src/main.cpp:1082-1085` defines `Type` with a
`TypeKind` and a `structName`, and nothing else. `Type::array` (`:1106`)
carries only the element `Type`. `llvmType` (`:1153-1156`) therefore maps
EVERY array to `{ptr, i64}` — a fat pointer with no element descriptor. The
element type is erased at the LLVM boundary. Consequence: `emitIndexEmit`
(`:8791`) and `emitIndexStoreEmit` (`:8803`) hardcode `CreateGEP(i64Ty, ...)`,
`inferType` hardcodes `Type::array(Type::i64())` at `:2620` and `:5571`, and
the AST array-literal emitter hardcodes `i64Ty` at `:6980`, `:6989`, `:6993`,
`:7008`, `:7023`. **Every array operation in the language is i64-only, and
this is structural, not a missing feature.**

**2.2 There are TWO generic instantiation engines.**
`Codegen::emitCall` (`:7532-7601`) and `Parser::emitDirectCall` (`:8986-9031`)
each independently infer type arguments and build the call. They were written
separately and drifted. Every generic bug exists in both. Fixing one and not
the other re-creates the divergence.

**2.3 `CodeGen::mangle` is not injective.** `:7391-7408` builds the mangled
name used for monomorphization and for the `genericCache` key. There is **no
`TypeKind::F64` case**: `F64`, `Ptr`, `Void` and `TypeParam` all fall through
to `default: s += "any"` at `:7404`. Two different instantiations can
therefore produce the same cache key. This is a latent cache-poisoning bug
that must be fixed BEFORE anything else touches generics.

**2.4 The symbol table is wiped before parallel bodies.**
`Parser::parseParallelForStmtEmit` does `cg->symTable = SymbolTable();` at
`:9633` before emitting the worker body, which is why `total` is undefined
inside `parallel for`. Note `:9624` carries the comment
`auto savedSym = cg->symTable; // copy assuming copyable` — which is exactly
the assumption that was never checked.

**2.5 The stable token table is a contract, not an implementation detail.**
`stableTokenKind` (`:1182`+) is consumed by `stage1/flint_lex.fl` and enforced
by `test_lexdiff.sh`, `test_parse_gate.sh` and `test_fixpoint.sh`. Adding a new
keyword breaks the self-hosting ladder. The compiler already has the correct
technique for this: contextual identifiers, used for `map` (`:8422`), `python`
(`:9664`) and `as` (`:1802-1804`). **Use contextual detection. Never add a
token.**

---

## 3. BUGS — verified, with mechanism

Ordered by severity. Each was reproduced.

### B1. Float TENSOR indexing silently produces garbage — NO DIAGNOSTIC
**Severity: highest. This is the worst class of compiler bug: a wrong answer
with no error.**

```
fn main() { a = [1.5, 2.5]  print(a[0]) }   ->  48     (should be 1.5)
fn main() { b = [1, 2, 3]   print(b[0]) }   ->  1      (correct)
```

**CORRECTED DIAGNOSIS (2026-10-18, after M0).** The earlier write-up blamed
plain array element-type erasure. That is the underlying cause (2.1) but not
the proximate one, and the distinction matters:

- `[1.5, 2.5]` is claimed by `Parser::tryParseTensorEmit` (`:8331`) as a
  **rank-1 f64 tensor**, not as an array. The tensor path runs BEFORE the array
  path (`:8495`) and does not soft-fail for float elements.
- The tensor is built by `flint_create_*` and returns a handle that is
  `{ptr, i64}` at the LLVM level — indistinguishable from an array slice.
- `Parser::emitIndexEmit` (`:8861`) unconditionally does
  `CreateGEP(i64Ty, dataPtr, index)` and `CreateLoad(i64Ty, ...)`, so indexing
  a float tensor reinterprets float bits as an integer.

So `[1.5, 2.5]` itself is CORRECT and must not be rejected — rejecting it would
break float tensors. The defect is that `[]` index access has no way to tell a
tensor from an array, and silently produces a wrong number.

**Fix requires tensor type tracking**, which does not exist today: `inferType`
never returns a tensor type and `TypeKind` has no Tensor member. The narrow,
low-risk path is to give `emitIndexEmit` provenance — mark tensor-typed values
so it can refuse with `tensor values cannot be indexed with []; use
flint_tensor_get(t, i)`. The thorough path is a real Tensor type in the type
system, which is larger and needs the Rust array representation first.

**Partially fixed in M0:** non-i64 elements that DO reach the array path (for
example strings) are now rejected with a diagnostic instead of being stored
into an i64 slot. See 07d2df1. The tensor-indexing hazard above remains OPEN.

**Fix applied in M0 (07d2df1).** `Parser::emitArrayLiteralEmit` (`:8908`) and
the AST emitter now reject any non-i64 element with a Flint diagnostic naming
the element index and its type. Widening arrays to f64 remains a separate,
deliberately-gated milestone.

### B2. `f64` generic leaks raw LLVM IR
```
fn first<T>(a: T) -> T { return a }   print(first(2.5))
-> Call parameter type does not match function signature!
   i64 %0 = call i64 @first.i64(double 2.500000e+00)
   function verification failed: main
```
Three causes, in dependency order:
1. `mangle` has no F64 case (2.3) — fix FIRST, or the fix below creates a new
   cache collision.
2. `Codegen::emitCall` `:7546-7548` maps `NodeKind::Number` to `Type::i64()`
   without checking `NumberExprAST::isFloat`. Contrast `:7550-7554`, the
   `Variable` branch, which correctly reads the symbol's real type. That is
   why `let v = 2.5; first(v)` works but `first(2.5)` does not.
3. `Parser::emitDirectCall` `:9009-9010` repeats the mistake.

**Fix:** after (1), check `isFloat` at `:7547` and `:9009`. Then add a
signature guard before `CreateCall` at `:7601` and `:9029`: if argument LLVM
types do not match the callee, raise a Flint diagnostic naming the function,
the expected type and the actual type. That guard is what permanently
converts this ICE class into ordinary errors.

### B3. Generic inference is a 4-case `NodeKind` switch
`:7545-7563` and `:8990-9011` handle only `Number`, `String`, `Variable` and
`EnumConstruct`. Missing: `FieldAccess`, index access, `StructLiteral`,
`Call`, `Cast`, `Binary`, array literal. Each silently instantiates `i64` at
`:7570` / `:9024`. So `max<T>(a.x, b.y)` is quietly wrong.

Verified sub-case: struct generics DO work in parameter position
(`getx<T>(p: T) -> i64 { return p.x }` prints `3`). The failure appears when the
generic's result is used as a struct, because return-only `T` falls to i64.

**Fix:** extract one `inferTypeArgs(FunctionAST*, args)` and route both call
sites through it; then replace the switch with a real `unifyParamType(formal,
argExpr, out)` walking compound types via `elemType`. Delete every silent
`Type::i64()` default at `:7570`, `:9024`, `:9683` — an un-inferrable
parameter must be an error naming the parameter.

### B4. A lambda is a compile-time symbol, never a value
Verified: `f = |x: i64| x + 1` works and captures work. But passing one to a
function fails:
```
fn apply(f: i64, x: i64) -> i64 { return f(x) }
fn main() { dbl = |x: i64| x * 2  print(apply(dbl, 5)) }
-> codegen: undefined function 'f'
```
Mechanism: `:7498` rejects calling a lambda from a different function, because
the binding is a symbol resolved at compile time. There is no representation of
a function as a runtime value, and `map`/`filter`/`reduce` do not exist.

Secondary defect: the natural-looking form `|x: i64| -> i64 { return x + 1 }`
produces THREE cascading errors (`empty lambda body`, `undefined var 'x'`,
`undefined function 'f'`) because `:4162` is `parseExpression()` with no
return type and no block body. People write the language off after one
confusing error.

**Fix:** closure conversion is already complete — `emitLambdaDef`
(`:6422-6485`) outlines a real function and freezes captures into hidden
slots. Only two things are missing: a `fn(i64, i64) -> i64` type in
`Parser::parseType` (`:2580`), and materializing the env pointer when a lambda
symbol appears in value position (`emitExpr` `:6070`/`:6142`). A `call(f, x)`
builtin then desugars to an LLVM indirect call with the env prepended.
**Zero runtime code, zero ABI change, identical in JIT and AOT.** Honest
constraint to document: closures remain stack-scoped and cannot outlive their
defining function — that is correct for v1, state it rather than hiding it.

### B5. `trait` is not a keyword; it silently mis-parses
`grep -n trait src/main.cpp` returns nothing. `:2333` treats a top-level
`IDENTIFIER IDENTIFIER` as a keywordless function definition, so
`trait Shape { fn area(self) -> i64 }` is parsed as a function named `trait`
taking a parameter named `Shape`, and the nested `fn area` is never registered.
The `undefined function 'area'` a user sees is a downstream symptom of a silent
mis-parse.

**Fix (minimal):** add `struct TraitDef { name, methods }` and a
`traitRegistry` beside `structRegistry` at `:4680-4682`. Detect `trait`
contextually in `parseProgram` (`:2264-2355`) via `IDENTIFIER "trait"
IDENTIFIER`. Impls are ordinary functions — `fn area(s: P) -> i64` — so there is
no new declaration syntax. At the single dispatch point, resolve by
name+arity: exactly one match -> direct call; zero -> `no type in scope
implements 'area'`; more than one -> `ambiguous: candidates P, Q`. Support
`P.area(s)` qualification by rewriting to `area(p)`, mirroring the enum
qualification that already works. **Do not add a lexer keyword (2.5).**

### B6. `parallel for` — two independent blockers
Verified: `parallel for i in 0..n { total = total + i }` ->
`codegen: undefined var 'total'`.
Blocker A: the symbol table is wiped at `:9633` (2.4).
Blocker B: `runtime/runtime.c:490-513` `flint_parallel_for` returns a
hardcoded `0` at `:512`, and `:9660` discards the return value entirely. There
is no reduction channel, so even a perfectly capturing `parallel for` could not
compute a sum.

**Recommendation: REMOVE the keyword, do not fix it.** Capture-by-value across
`pthread` workers is a cross-thread use-after-free: a captured `str`/map/array
pointer can be snapshotted while the parent frame frees it. That would open a
memory-safety hole in the one property this language leads with — and the rest
of the safety story (compile-time use-after-free, bounds checks) would be
for nothing. The self-hosted compiler contains `"parallel for "` only as a
pretty-printer string literal (`stage2/flint_parse.fl:3140`), so removal breaks
nothing in the self-hosting gate. Replace the parse with a migration error and
leave the runtime symbol for ABI stability. If concurrency returns, ship
order-deterministic capture-free builtins in `runtime/rust/` instead — no
syntax sugar, no capture, no new language feature.

### B7. `specialize` latent bugs that bite the moment generics is real
`CodeGen::specialize` (`:7410-7488`):
- **No recursion guard.** `genericCache[mangled]` is written at `:7486`, AFTER
  the body is emitted. A self-recursive generic re-enters forever and hangs or
  overflows the compiler.
- **Lossy insert-point restore.** `:7484` uses `SetInsertPoint(savedBlock)`,
  which re-inserts at the END of the block. Nested monomorphization misplaces
  instructions. Must be `saveIP()`/`restoreIP()`.
- **Compound params never substituted.** `:7421-7426` does
  `subst.find(rt.structName)`, but for `[T]` the structName is `""`.
  `resolveType` (`:4645-4656`) already recurses through `elemType` correctly —
  point `specialize` at `resolveType` and the compound case falls out.

### B8. `Option<T>` in ROADMAP is not generic
`ROADMAP.md:466` claims a builtin generic `Option<T>`. FALSE.
`initStdEnums` (`:4697-4703`) defines `Option.Some(i64)` — a fixed i64
payload. There is no type-level generic enum, which is why a generic `Result`
or `Option` cannot be written today.

### B9. Array API pairing is inconsistent
`flint_array_alloc` returns a `{ptr, i64}` slice while `flint_aegis_free`
expects a plain `i8*`, so mixing them fails to link with a raw LLVM type
mismatch. Not a correctness bug (the compiler refuses) but it surfaces internal
IR at the user. Any signature guard added in B2 should be applied here too.

---

## 4. WHERE RUST SHOULD BE USED

Context respected: `runtime/rust/` is already a `#![no_std]` staticlib linked
into BOTH JIT and AOT, so Rust is a proven vehicle here, not a rewrite.
Constraint that matters: the crate has a **custom `panic_impl` lang item**, so
any dependency that links `std` breaks the build. This is not theoretical —
`fdeflate` linked `std`, the release staticlib silently failed, and `build.sh`
fell back to "no Rust stdlib" entirely, taking down every Rust builtin at
runtime while still reporting success.

| Candidate | Call | Reason |
|---|---|---|
| Bounds-checked array element access (`flint_array_get/set`) carrying an explicit element size | **R** | Lets `[T]` work without touching `llvmType(TypeKind::Array)`. Unsafe indexing is precisely the class where a Rust bound check is worth a predictable branch. Move into `runtime/rust/` next to the arena. |
| Checked arithmetic helpers (overflow, div-by-zero, index math) | **R** | Centralizes overflow policy currently scattered through C++ hot paths; `panic` + the arena give a consistent failure mode. |
| Generic array/slice storage (length, element size, data pointer) | **R** | The representation is a value type with no compiler invariants attached; it is a library concern, not an IR concern. |
| Lexer/parser for stage1/2/3 | **C** | The self-hosting ladder REQUIRES these stages to be written in Flint. Moving them to Rust destroys the property that makes this compiler unusual. |
| The two emit paths / `specialize` / `mangle` | **C++** | These construct and inspect LLVM IR and depend on compiler-internal invariants. Handing them to a second language couples the fix to an FFI boundary for no gain. |
| Module cache hashing | **C++** | It reads the compiler's own AST for the key; already inside that code. |
| LLVM `StringMap`/ADT internals | **C++** | Honestly: no. Already optimal, and replacing them adds a boundary for zero benefit. |

Practical note on B1: the cheapest correct fix is the C++ rejection above. The
Rust array work is what unlocks `[T]` later; it is not needed to stop the lie.

---

## 5. MILESTONES

Critical path M0 -> M1 -> M2 -> M3 -> M4. M5 and M6 touch disjoint code
(`:9592`/`:2820` and `:8374`/`:2333`) and can run any time.

**M0 — Stop the bleeding.** Make `mangle` injective (`:7391`). Fix `isFloat`
inference (`:7547`, `:9009`). Add `CreateCall` signature guards (`:7601`,
`:9029`). Reject non-i64 array literal elements loudly (`:8826`, `:6975`).
*Gate:* `first(2.5)` prints `2.5`; `[1.5,2.5]` is a clean error; a negative
test asserting zero LLVM verifier text on stderr.
*Rollback:* pure additions plus one error message; revert is trivial.

**M1 — One instantiation engine.** Extract `inferTypeArgs`; delete the
`NodeKind` switches; fix `specialize` (B7): cache before body, `saveIP`,
route compound substitution through `resolveType`.
*Gate:* both backends emit byte-identical IR for a generics file;
`test_differential.sh` green.
*Rollback:* this is the riskiest milestone — it touches both emit paths. If
the self-host fixed point breaks, revert wholesale rather than partially.

**M2 — Real inference.** `unifyParamType` for `FieldAccess`/index/`Call`/
`Cast`/`Binary`/`StructLiteral`; add `f::<T>(...)` explicit type arguments;
delete every silent i64 default.
*Gate:* return-only params, field-access args and nested generic calls all
work; every un-inferrable case errors by name.

**M3 — Traits (static).** `traitRegistry`, contextual parse, name+arity
resolution, ambiguity error, `P.area(s)` qualification.
*Gate:* `test_lexdiff.sh`, `test_parse_gate.sh`, `test_fixpoint.sh` all still
green and unmodified — that is the proof the token contract was not touched.

**M4 — Lambda as a value.** `fn` type in `parseType`; env-pointer
materialization in `emitExpr`; `call` as a pure LLVM indirect call.
*Gate:* `apply(|x| x + 1, 5)` works; a closure escaping its frame still errors.

**M5 — Remove `parallel for`.** Parser change plus migration error; keep the
runtime symbol.
*Gate:* `tests/t_flow.fl` migrated; `test_errors.sh` asserts the message;
self-host gate unaffected.

**M6 — Diagnostics.** Stop the cascade for the `|p| -> T {}` form at `:8374`;
tighten top-level `IDENTIFIER IDENTIFIER` at `:2333`.
*Gate:* each malformed input yields exactly one error.

**M7 — `[T]` containers.** Deferred. Requires the Rust array representation
from section 4 plus an explicit ABI decision. Only after M0-M6 are green.

---

## 6. REJECTED APPROACHES

1. **Do NOT add an element-type field to `llvmType(TypeKind::Array)` (`:1153`).**
   The single most dangerous available change. It alters the layout of every
   array value in every object file and every `.flint.bc` interface, silently
   mismatching `--use-interface` copies (`:11845-11856`), AOT/JIT parity, the
   Rust staticlib ABI and C FFI. Use the Rust array representation with an
   explicit element-size parameter instead.
2. **Do NOT add a lexer keyword.** `stableTokenKind` is a Stage-1 contract
   consumed by `stage1/flint_lex.fl`. Use contextual detection like `map`,
   `python` and `as` already do. `KW_PARALLEL` already exists at `:1175`;
   removing its USE is safe, adding a NEW token is not.
3. **Do NOT use dictionary passing or vtables for generics.** It requires the
   runtime value to carry a type descriptor, which arrays do not
   (2.1) — an ABI change rippling through the runtime, the Rust staticlib, C
   FFI and the interface format. Monomorphization is a local, deterministic
   transformation that the self-hosted emitter can reproduce byte-identically.
4. **Do NOT fix the two emit paths in different milestones.** That is how B2
   and B3 exist twice. One change, both paths.
5. **Do NOT reword existing error strings.** `tests/run.sh` matches on text.
   Add new messages; never edit existing ones inside a feature milestone.
6. **Do NOT ship `dyn Trait` before static traits exist.** A second erased
   value representation compounds the array-erasure problem for zero users.
7. **Do NOT "fix" float arrays by widening them in M0.** Reject first, widen
   deliberately later behind its own gate.

---

## 7. RISK REGISTER

| Risk | Detect with | Mitigation |
|---|---|---|
| Array layout change breaks `--use-interface` | `test_selfhost.sh` phase C | Never edit `llvmType` array case; see Rejected 1 |
| Generic change breaks the fixed point | `test_selfhost.sh` byte-identical comparison | Fix both emit paths in ONE change; revert wholesale if red |
| New lexer token breaks stage1 | `test_lexdiff.sh`, `test_parse_gate.sh` | Contextual detection only |
| Rust dependency links `std` | Build output must show `flint_std.o (Rust stdlib, both paths)`; grep the object for `std` symbols | Every new dep must be `no_std`; the gate already checks "module is no_std" |
| Rust dep breaks JIT on Android | Run `test_rust_stdlib.sh` on-device | `no_std` + no TLS; the existing stdlib is the proof |
| Silently-wrong answers like B1 | Add negative tests asserting diagnostics | Treat any silent wrong answer as P0 |
| Monomorphization compile-time blowup | `bench/compile_time/BASELINE.md` | Only one file uses generics today; revisit past ~200 instances |

---

## 8. OPEN DECISIONS FOR THE MAINTAINER

1. Is removing `parallel for` acceptable as a product decision? The analysis
   says yes (B6); it is a user-visible language-surface removal, so it is
   yours to call.
2. Should generics stay monomorphization-only, or is a `dyn`-style escape
   hatch wanted later? M0-M2 do not foreclose either.
3. Does `[T]` matter enough to justify the Rust array representation now, or
   should M7 wait until real programs need it?

---

## 9. TRAP LESSONS FROM THIS AUDIT

- Trap: trusting `memory.md`/`ROADMAP.md` as evidence. They asserted a P0
  compiler crash that does not exist. Always compile a 5-line program before
  believing a doc claim about behaviour.
- Trap: assuming a generic call's parameter position tells you about its
  return position. Struct generics WORK in parameter position; return-only `T`
  is what breaks. Test both.
- Trap: testing a language feature with syntax you guessed. `fn(x){}` lambda
  syntax does not exist; `|x: i64| x + 1` does, and captures work. A failed
  test proves nothing until the syntax is confirmed against real examples.
- Trap: an ICE that leaks LLVM IR is a missing signature guard, not just a
  type-inference bug. The guard is what stops the whole class recurring.
- Trap: a compiler that accepts wrong code silently is worse than one that
  refuses. B1 (`[1.5, 2.5]` -> 48) is the most serious defect found in this
  audit, precisely because nothing complained.
- GATE lesson: establish a green `tests/run.sh` + `tests/test_selfhost.sh`
  baseline BEFORE M0. Every milestone gate above assumes a green baseline.

---

# Addendum (2026-10-18, after M1) — two new findings

## N1. A function that matches a PAYLOAD enum hangs the compiler. PRE-EXISTING.

Found while testing M1. Confirmed on the committed baseline, so it predates
every change in this session.

```flint
enum Tree { Leaf, Node(i64, i64) }

fn sum(t: Tree) -> i64 {          // <-- a named function, not main
    match t {
        Tree.Leaf => 0,           // expression arms
        Tree.Node(a, b) => a + b,
    }
    return -1
}
fn main() { print(sum(Tree.Node(3, 4))) }
```

`./flintc` never terminates; killed by `timeout -s KILL` (exit 137).

**Bisected.** The trigger is the payload enum inside a function with a declared
return type:

| Case | Result |
|---|---|
| payload enum + match in `main` | works (`examples/enums.fl` prints `42 99`) |
| no payload (`enum E { A, B }`) + match in a function | works |
| payload enum + match in a function, expression arms | **HANGS** |
| payload enum + match in a function, block arms | **HANGS** |

So: **`match` on a payload enum only works inside `main`.** Any other function
deadlocks the compiler. That is a severe limitation, not a cosmetic one — it
means pattern matching cannot be used in real code, and it is the most likely
reason `match` has felt unfinished.

**RESOLVED — this note was STALE when read back (verified 2026-10-19).** The
root cause was NOT in the match emission path: the match-ARM parser accepted
exactly one binding name, so `Node(a, b)` was a parse error, and `consume()`
reported the error WITHOUT advancing the token cursor, so the enclosing arm
loop could not make progress and spun forever. Both are fixed: patterns bind
one name per payload field, and `consume()` always advances when recovering.
The regression fixture is `tests/t_payload_match.fl` (passes: 7, 0, 6, 9, 12,
24, 0, 10) and `tests/test_generics.sh` keeps an explicit no-hang check with
`timeout -s KILL`. No test corpus needs to avoid payload-enum matches in a
non-`main` function any more. Lesson (this file's own §9 trap list): a doc
claim about behaviour is not evidence — compile the 5-line program before
believing it, even when the doc is this file.

## N2. Rust placement that paid off, and one trap

`runtime/rust/src/array.rs` replaced the array runtime from `runtime.c`. The
value was not "fewer lines of C": it was that the length now travels with the
pointer, so `flint_array_get`/`_set` can be checked at the point of memory
access, and a negative index has no valid representation as a `usize` offset.

Three defects were caught only by running the tests, not by reading the code:

1. The crate's global allocator is `arena::BumpAllocator`, whose `dealloc` is a
   **no-op**. Routing arrays through it meant every array permanently consumed
   arena space and `flint_array_free` freed nothing. The array heap must be
   libc's `calloc`/`free`.
2. `match raw { null => ..., ptr => ... }` are **binding patterns**, so the
   first arm matched every pointer and every allocation silently failed. Use
   `if raw.is_null()`.
3. Formatting an i64 with `i128` division pulled in a compiler_builtins member
   carrying an undefined `rust_eh_personality`, which made **every JIT program
   fail to load**. Any arithmetic in this crate must stay within the target's
   native width.

**Rule to keep:** in this crate, prefer `u64`/`i64` arithmetic, avoid anything
that pulls compiler_builtins members, and never assume the global allocator is a
heap.

---

# Addendum 2 (2026-10-18, after M2) — three items found DURING M0–M2

These are not in the milestone table. Two of them outrank M3.

## P0-NEW-1 — COLLAPSE THE DUPLICATED EMIT PATHS. Above M3.

Comparison emission, call emission and generic inference each exist TWICE:
once in `Codegen` (the AST path) and once in the single-pass `Parser` path.
A fix applied to one does not reach the other.

This has produced three separate bug classes in one session:

1. The `f64` comparison abort (`ICmp` on doubles) was fixed in `Codegen`;
   `Parser` kept the broken version.
2. The call-signature check (arity + argument types) was added to `Codegen`;
   `Parser` had none, so a wrong-arity call still emitted invalid IR.
3. Aggregate equality was lowered in `Codegen`; `Parser` had none, so enum
   equality kept aborting.

Each time the symptom looked like "the AST path is broken" or "enums are
broken". It was neither. It was two implementations drifting.

**Cost of leaving it:** every codegen fix must be applied twice and verified
twice, and the differential gate does not catch it because the two paths are
never both exercised on the same construct.

**Why it is first:** it removes an entire category of bug. M3 adds a feature;
this removes a source of defects that has produced four findings so far.

## P0-NEW-2 — ENUM EQUALITY NEEDS PER-VARIANT PAYLOAD WIDTH

Enum equality is currently REFUSED with a diagnostic. That is deliberate and
correct for now: the alternative was silently wrong.

An enum's LLVM type is `{ i8, [N x i8] }` — a tag plus a packed payload array
sized to the WIDEST variant. Bytes past what a given variant writes hold
whatever was on the stack, so byte-wise comparison reports two identical
`Some2(3, 4)` values as UNEQUAL. A confidently wrong answer is worse than the
abort it replaced.

**The fix:** thread the per-variant payload width from `enumRegistry` (which
has `variants` with `payloadTypes`) into the comparison, then compare tag plus
exactly the bytes the variant writes. The width is NOT in the LLVM type, which
is the entire problem. Contained and well-defined now that the crash and the
silent-wrong-answer are both gone.

## P0-NEW-3 — B1 REGRESSION CHECK: f64 ARRAYS MUST NOT BE SILENT

B1 remains open: `[1.5, 2.5]` is claimed by `tryParseTensorEmit` as a float
TENSOR, and indexing that tensor with `a[0]` reinterprets float bits through an
i64 GEP, printing 48 with no diagnostic.

Detecting it needs tensor provenance that does not exist: `inferType` never
returns a tensor type and `TypeKind` has no `Tensor` member. Narrow fix: mark
tensor-typed values so `emitIndexEmit` can refuse with "tensor values cannot be
indexed with []; use flint_tensor_get(t, i)". Thorough fix: a real Tensor type,
which needs the Rust array representation (M7).

## Status of the milestone table after this addendum

| Item | State |
|---|---|
| M0, M1 | done |
| M2 | **done** (param matching + f64 FCmp; cast + nested-call args infer — 3f062e4; explicit `f::<T>(...)` — 3d3d914; array args — 361d810; STRICT resolver, no silent i64 default, f64 arithmetic infers f64 — 922d03a). The resolver reports an un-inferrable argument by name. |
| P0-NEW-1 path collapse | **done** (compare/call/generic cores shared, commit below) |
| P0-NEW-2 enum payload width | **done** (per-variant field compare, commit below) |
| P0-NEW-3 tensor index check | **done** (narrow stop-gap, commit below) |
| M3 traits | **done** — commit 1219665 |
| M4 lambda as a value | **done** — commit a6b89e6 |
| M5 remove `parallel for` | **done** — commit 9553f8c |
| M6 diagnostics | **done** — commit d65ed3f |
| M7 `[T]` containers | **deferred by maintainer decision (2026-10-19)** |

# Addendum (2026-10-19, after M3-M6) — milestone wave complete

All milestone work landed in ONE session with every gate green at each
commit:

- **M3 static traits (1219665).** traitRegistry beside structRegistry; `trait`
  stays contextually parsed (no lexer keyword — token contract proven intact:
  lexdiff 130, parse_gate 131, fixpoint pass); name+arity dispatch (exactly-one
  → call, zero → "no type in scope implements", >1 → "ambiguous
  (candidates:)"); `P.area(s)` qualification via M3 UFCS. Pre-existing gate
  breaks discovered and FIXED: binary string-literal fixtures (NUL truncates
  the self-host lexer) extracted to tests/fixtures/*.bin read via
  flint_read_file; flint_parse.fl spelled out `and`/`or` (pp keeps source
  spelling) and accepts multi-binding match payloads.
- **M4 lambda-as-a-value (a6b89e6).** `TypeKind::Func` with
  funcParams/funcRet (funcRet is a shared_ptr — a struct cannot hold a Type
  member of itself); `fn(T1,T2) -> Ret` in parseType (arrow required);
  uniform closure protocol: outlined fn = (env, userargs...), env box =
  {fnptr, caps...} in the defining frame; every call indirect (load field 0,
  CreateCall(fnTy, ptr, {box,...})); fn-typed params/bindings; escape rule =
  hard error at return/global store; named-fn-as-value refuses with clean
  advice (`let h = inc` — future work). Gates: apply(|x| x+1, 5) == 6
  in-run; escaping closure fails rc=1; smoke 29/29; all suites rc=0
  (differential, generics, no_hang, errors, parse_gate, lexdiff, fixpoint,
  python, ai, rust_stdlib); selfhost fixed point held.
- **M5 `parallel for` removal (9553f8c).** parseParallelForStmt AND
  parseParallelForStmtEmit replaced wholesale with the B6 migration error
  ("'parallel for' was removed: it could not capture variables and had no
  result channel. Use a plain 'for' loop, or move the work into a Rust
  runtime function such as flint_parallel_reduce"); KW_PARALLEL token and the
  flint_parallel_for runtime symbol kept (ABI); tests/t_flow.fl migrated to a
  plain for (100..104 same expectations); NEW tests/t_pfor_removed.fl with
  run.sh check_fail asserting the message (test_errors.sh only catalogs the
  stage3 emit strings, so the parse-side message lives in run.sh). Smoke
  30/30; selfhost byte-identical after the change.
- **M6 diagnostics (d65ed3f).** `|p| -> T {}` refuses at the parse ("a lambda
  has no '-> Ret' form...") — the old flow silently accepted it and codegen
  reported "empty lambda body" + "function body emission failed"; top-level
  `name (not-a-fn)` and `name name` candidates that fail the isFunc lookahead
  now error once instead of silently parseExprStmt-ing garbage (previously
  rc=0 NOTHING happened). Parser::skipToLineEndTokens() enforces the
  exactly-one-error contract. Gates: smoke 30/30; parse-gate 131; lexdiff
  130; differential 11; errors catalog 128/128; three malformed fixtures each
  produced exactly one error with a precise location.

**B-tasks of the next chat, in order:** P0-NEW-3 → P0-NEW-1 → P0-NEW-2
(user's priority). P0-NEW-3 landed as commit 67edab9.

- **P0-NEW-2 enum equality (per-variant payload width).** EmitEnumEqual keyed
  off `enumRegistry` at the top of emitAggregateEqual (both callers unchanged):
  compare the i8 tags; when they match, chain across variant blocks, and in
  each variant body bit-free GEP into `{ i8, T0..Tn }` (the IDENTICAL layout
  both constructor sites write) and compare only the fields the variant
  declared — garbage tail bytes are never read. Result accumulates in an i64
  slot (0 = unequal), tags differing or unknown tags short to bbEnd, tag-only
  enums store 1 immediately. Refusal stays for non-enum struct array fields.
  Fixture: tests/t_agg_eq.fl enum section now asserts Some2(3,4)==Some2(3,4)
  → 1 (the exact case that motivated the refusal is now pinned), cross-variant
  → 0, None==None → 1. Gates: smoke 30/30, lexdiff 130, parse_gate 131,
  fixpoint, errors 128/128, differential 11. Traps hit: a first naive
  paste-by-anchor edit left the old refusal tail duplicated (brace mismatch);
  LLVM StructType has no hasStructName() in this version — literal-struct
  detection via isLiteral() only. **CORRECTION (after P0-NEW-1):** the closing
  claim "all 16 outputs verified by run" for THIS task was careless — the
  emitEnumEqual accumulator had a real bug at commit d1c58cf (ANDing variant
  bodies against the 0-initialized slot instead of ORing), so == computed 0 on
  equal payloads and even made == disagree with != on the same operands. The
  fixture was re-verified line-by-line mapped to semantics only during
  P0-NEW-1; the fix is in the same commit as P0-NEW-1 (below). Lesson: verify
  by mapping EACH printed line to its assertion, never by eyeballing ones
  and zeros. The P0-NEW-2 commit message therefore overclaims.

- **P0-NEW-1 path collapse.** Shared Codegen cores; the Parser emit path now
  delegates instead of keeping drifting copies (the source of four findings):
  1) emitCompareValues(op, l, r) — aggregate-first, pred map, str-compare,
     null-norm, FCmp-float branch; both cmp arms now single calls.
  2) createCallChecked(name, f, args, instantiationMsg) — arity + loose-ptr
     type guard before CreateCall; AST plain tail, AST generic-instance,
     Parser functionMap branch (previously UNGUARDED — wrong-arity emitted
     invalid IR), Parser generic-branch, Parser UFCS branch all route here.
  3) emitPrintCall(v) — type-keyed println dispatch (Parser copy used to
     silently pick println_i64 for unsupported types and RETURNED nullptr).
  4) emitArrayGet/Set/Data/GetPtr/SetPtr cores — the struct guard lives in
     the core ("flint_array_get needs an array"); the Parser copy had lost
     it and extracted field 0 of a bare i64.
  5) Parser generic inference rewritten to the by-PARAMETER rule (type param
     infers from the argument filling the parameter that mentions it) like
     the AST path; the old copy indexed typeParams by arg position, so
     swap<T,U>(a: U, b: T) instantiated swapped. Un-inferrable still
     defaults to i64 in emit mode (no AST available); the guarded call
     below the inference catches misfits.
  Wait — this is intentionally left as a residual: emit-mode inference
  cannot use exprFlintType (no AST), so the type SOURCE stays path-specific;
  the RULE and the validation are shared. That is the collapse boundary —
  further unification would mean the Parser builds AST nodes, which is a
  different milestone.
  Gates on the fresh build: smoke 30/30, lexdiff 130, parse_gate 131,
  fixpoint 131, errors 128/128, differential 11/11, generics 7/7; every
  t_agg_eq line mapped to its assertion; bad-arity, enum ==/!= per-case,
  nested-struct payload compare all verified individually.

**M7 decision (2026-10-19, maintainer):** `[T]` containers stay DEFERRED
(memory.md §8 decision #3). The Rust {length, elem_size, ptr} representation
(§4) is approved IF a real program needs it later; the narrow B1 stop-gap
(tensor index refusal — P0-NEW-3) is the first B-task of the next chat and
does not pay for the ABI change.

# Addendum (2026-10-19, later) — M2 residue part 1 + a STATE pick-up guide

- **M2 residue part 1 (commit 3f062e4).** Generic argument inference learned
  two argument shapes it did not know: a CAST argument (its type is the cast
  target) and a nested CALL argument (the callee's return type, resolved with
  the same by-parameter rule, recursively). Both used to fall through to i64,
  so the program failed later with a message about the instantiation
  ("expects i64") instead of about the program. New
  `Codegen::inferGenericArgType`; `exprFlintType` gained the Cast case.
  Fixture `tests/t_generic_infer.fl` (6 smoke checks; the 1.5 line is the
  discriminator — an i64 instantiation would print 1). Gates: smoke 36/36,
  lexdiff 131, parse_gate 132, fixpoint 132, errors 128/128, differential
  11/11, generics 7/7, no-hang 88/88. Still open in M2: explicit `f::<T>(...)`
  type arguments, deleting the remaining silent i64 defaults, and arrays as
  generic arguments (`idc([1,2])` — probe whether generic instantiation over
  an array type works end-to-end before enabling inference for it).
- **STATE.md is now a pick-up guide (commit b54ae7b).** §1 build/verify
  recipes and the device traps (stale `flintc`, `/tmp` unwritable, pipes
  eating exit codes, tool timeouts killing process groups, pkill matching its
  own shell); §2 the VERIFIED open-items table (8 items) with the note that
  N1 was already fixed and the audit note was stale; §3 concrete next steps;
  §4 file anchors; §5 dead ends. The milestone history is preserved below the
  guide header.
- **Gate dependency worth knowing:** `test_differential.sh` needs the
  self-host binaries under `$TMPDIR/slipstream/self` (build with
  `bash tests/test_selfhost.sh A`). If tmp was cleaned, the gate reports
  "1 passed, 10 failed" with `self binaries` — that is a missing artifact,
  not a divergence. `test_no_hang.sh` (88 checks) is the gate that proves no
  corpus file hangs the compiler.

# Addendum (2026-10-20, later) — M2b, the named-function value, and a finding
  about the "two emit paths"

- **M2b explicit type arguments (3d3d914).** `f::<T1, T2>(...)` states the
  type arguments instead of inferring them. `::` is two COLON tokens to the
  lexer, so no lexer change was needed and the token contract is untouched
  (lexdiff 131 unchanged); nothing in the language used `::` before. Explicit
  arguments override inference and feed the same instantiation path
  (mangle + specialize + genericCache). Errors: wrong count, a non-generic
  callee, a non-concrete type argument. Both C++ paths changed (AST and the
  single-pass Parser). The fixture lives in tests_manual/ because the
  self-hosted parser (stage2/flint_parse.fl) does not implement the syntax —
  the same situation as the `trait` keyword.
- **A named function can be a value (fd8cf09).** `let h = inc` now works.
  A named function has no captures, so it joins the existing lambda protocol
  through a THUNK: a generated wrapper with the same fn(env, args...) shape
  that ignores its env argument and forwards to the real function. The call
  site needs no discriminator, and the fn-value call path is unchanged. The
  box lives in the current frame, so the escape rules apply (it cannot be
  returned or stored in a global). A generic function as a value is refused
  by name.
- **FINDING — the C++ "single-pass emit path" is UNREACHABLE (361d810).**
  The doctrine in this file (and in STATE.md) says every fix must land in
  both C++ paths, because "comparison emission, call emission and generic
  inference each exist TWICE". They DO exist twice, but the second copy
  (Parser::emitXxx) is entered only from `Codegen::emitFunction` for a
  function with no AST body — and in a normal compile every function has
  one. Measured with the FLINT_TRACE_EMITPATH probe added in 361d810: over
  all 137 corpus .fl files, ZERO hits. So:
  - fixes applied to that path are harmless but not load-bearing;
  - the REAL second implementation is the SELF-HOST Flint pipeline
    (stage2/flint_parse.fl + stage3/flint_emit.fl), which the differential
    gate compares against and which is a genuine independent oracle;
  - the self-host pipeline has NO traits, NO generics, and no `::<` — so
    features added to the C++ compiler are not yet available to the
    self-hosted toolchain. That gap is the one that matters now.

# Addendum (2026-10-20, later still) — M2c, the P4 error-code docs, and the
  self-host feature gap

- **M2c — the generic argument resolver is strict (922d03a).** The M2 spec
  asked for "an un-inferrable parameter must be an error naming the
  parameter". `Codegen::inferGenericArgType` is now a strict resolver: it
  returns false when the argument's type cannot be determined, and the
  inference loop reports the parameter by name ("cannot infer type parameter
  'T' for 'idc'; the argument's type could not be determined") instead of
  instantiating i64 and letting the program fail later with a message about
  the instantiation. Two real silent-wrong answers fixed on the way:
  a float arithmetic expression inferred i64 (`idc(1.5 + 2.5)` instantiated
  idc.i64), and a call to a USER function inferred i64 (the builtins table
  does not know user functions, so `idc(f())` instantiated idc.i64). The
  fixture caught a regression during this work: an index argument
  (`idc(arr[1])`) used to work only because the silent default happened to
  be right; the resolver named it and the index case was added.
- **P4 — the C++ compiler's diagnostics are catalogued (4214082).** The P4
  ship list asked for error-code documentation. `docs/errors.md` catalogued
  the SELF-HOST EMITTER's messages; the messages users actually see come
  from `src/main.cpp` and had no catalog. `docs/errors-cpp.md` now lists all
  135 of them as PATTERNS (literal text with `<...>` where a value is
  interpolated — a catalog of literal fragments is useless to a user
  searching for a message), with curated fix guidance by family.
  `tools/errors_cpp.py` extracts them; `tests/test_errors_cpp.sh` keeps the
  doc and the source in sync BOTH ways (a new diagnostic cannot ship
  undocumented; a removed one cannot linger) and is wired into CI.
  The rest of the P4 list: the semver freeze and the SBOM generator
  (`tools/sbom.py`) already exist; signed tags need a GPG key, which this
  device does not have — the procedure is documented in `docs/RELEASE.md`.
- **D5 — the self-host feature gap is now recorded (COMPATIBILITY.md).**
  The differential gate is green (11/11) but it cannot see what it does
  not cover: the C++ compiler now accepts programs the self-hosted pipeline
  cannot parse or emit at all — `trait`, generics, `f::<T>(...)`, enum
  equality, a named function as a value. D5 lists them with the policy that
  every feature added to the C++ compiler from now on either lands in the
  self-host pipeline too or is listed there. Porting them is a project of
  its own (stage2 is 4.1k lines, stage3 is 7.6k); it is a feature gap, not
  a miscompile in either compiler.

# Addendum (2026-10-20, assessment) — a measured language/compiler roadmap

`docs/LANGUAGE-ROADMAP.md` (30 sections) is the engineering assessment and
roadmap. Every claim in it is labelled by kind: [M] measured, [P]
published, [R] reported, [I] inference, [D] design, [S] speculation. It is
the companion to this ledger, `STATE.md` (pick-up guide) and
`COMPATIBILITY.md` (divergence log).

- **The binary-size regression was found, root-caused and fixed (46a007e).**
  A hello-world linked to 743,520 B (565,600 B stripped) — 130x C — and
  reserved 72 MB of `.bss` in every process, because the link line had no
  `-Wl,--gc-sections` (the runtime objects were already compiled with
  `-ffunction-sections`, so the capability existed) and linked sqlite3,
  curl and openblas unconditionally. After the fix: 8,048 B raw / 4,536 B
  stripped (C: 4,368 B), `.bss` 24 B, and ONE dynamic dependency (libc).
  Two traps found on the way: the compiler CACHES THE LINKED BINARY by
  source hash, so a stale artifact can survive a source fix; and
  `--as-needed` ignores WEAK undefined symbols, which silently demoted the
  BLAS matmul to the portable loop (n=200: 14.4 s instead of 2.9 s) — a
  5x regression no test caught, because the results were still correct.
- **Measured performance baseline** (this device, `tools/bench_compare.sh`):
  sum_array 10M 19.1 ms (C 10.1), primes 10M 495 ms (C 220), fib(45) 12.9 s
  (C 9.6), pi 100M 779 ms (C 783 — parity). Build 144 ms (C 439, Rust
  674); cold start 37 ms (C 33). `--unsafe` removes 13% of sum_array and
  2.5% of primes, so checks are at most 13% of the integer-loop gap; the
  remaining ~1.6x is NOT yet explained, and §4.5 of the roadmap defines
  the experiment (compare optimized IR against C's) that will explain it.
- **Where the safety cost actually is** (`--check-bce`): checks die when
  the bound is provable in the same function (100% eliminated for a
  constant-trip-count loop) and survive when the array length is a separate
  loaded value (sum_array: 3 of 6 survive). The fix is to make the length
  and the loop bound the SAME SSA value (the "reslicing" technique), which
  needs no new IR.
- **The performance gap is now MEASURED, not guessed** (IR comparison of
  the optimized modules, `flintc --emit-llvm` vs `clang -O2 -S -emit-llvm`
  on sum_array): C 52 instructions in main vs Flint 49 (Flint is
  SMALLER), but C has 16 vector operations and Flint 0, and C has 2 icmp
  vs Flint 4. So the gap is NOT code bloat: C auto-vectorizes the integer
  reduction while Flint cannot, because C's out-of-bounds and signed
  overflow are UB (assumed away) whereas Flint's are defined traps, and
  because the array length is a runtime value unrelated to the loop bound.
  The f64 benchmark is at parity exactly because clang will not
  reassociate an FP reduction either -- the gap appears precisely where
  C's UB buys it more. Consequence: the fix is ANALYSIS (range analysis to
  drop the overflow check; same-value bounds so the bounds check folds),
  after which LLVM's own vectorizer can vectorize -- Flint does not need a
  vectorizer of its own. This also disproved one of the roadmap's own
  proposals ("inline the array accessors"): the IR shows the accessor's
  fast path is ALREADY inlined.
- **The roadmap's order** (measure -> recover -> unify -> safety -> type
  system -> optimization -> size -> tooling -> ecosystem -> domains):
  Phase 0 is measurement infrastructure (CI benchmarks + size attribution
  + the safety dashboard) because two invisible regressions in this
  assessment prove the gap. Phase 1 is the now-shipped link fix plus the
  IR comparison. Phase 2 is a typed HIR — the precondition for range
  analysis, escape analysis, capability types, and a non-drifting LSP.

# Addendum (2026-10-20, deep-research pass) — the 2026 external landscape

Sources consulted in the second research pass (all 2026 unless noted), for
the comparison table and the sharpened plan. Claims from these are tagged
[P] in `docs/LANGUAGE-ROADMAP.md`:

- **Rust**: Polonius Alpha on nightly since 2026-08-04 (flow-sensitive
  borrow checking; NLL "problem case #3" — conditional borrows — now
  accepted; one known soundness issue remains; stabilization targeted for
  late 2026). Rust for Linux is integral in the kernel (mainline drivers).
- **Go**: 1.26 (Green Tea GC default, Swiss Tables maps, cgo −30 %, more
  stack-allocated slice backing stores); 1.27 adds a portable, size-agnostic
  `simd` package (AVX/AVX2/AVX-512, NEON, wasm) with emulated fallback
  everywhere, explicitly designed to be readable by LLMs.
- **Zig** 0.16: self-hosted x86 backend is the Debug default; incremental
  compilation reached millisecond rebuilds by avoiding over-analysis and
  making the dependency graph acyclic; still off by default (known bugs and
  miscompiles).
- **Swift** 6.2 (Approachable Concurrency: `Task.immediate`, isolated
  `deinit`, WebAssembly, Android SDK preview); Swift 6 mode makes
  data-race checking the default.
- **TypeScript** 7 is the native Go port: 8-10x faster builds (vscode
  89.11 s -> 8.74 s), type-checking "very nearly complete" (20,000 test
  cases, 74 known gaps); TS 6.0 is the last JS-based release; `--strict`
  becomes the default.
- **Kotlin** K2 default (~40 % faster builds); **Java** JDK 25 LTS;
  **C#** .NET 10 LTS with C# 14 (TIOBE language of the year).
- **Domain-system precedents**: Reactant.jl (MLIR+XLA: 14.8-17.3 TF/s
  GEMM, cuBLAS-level, but seconds to compile) vs JLIR (arXiv 2609.04585:
  a Julia-native dialect IR, <1.5 ms transformation overhead, 87-96 % of
  hand-written baselines, but 1.2 TF/s GEMM without tiling). MLIR's stated
  drawback for dynamic languages: strong compile-time type requirements and
  a low-level C++ extension model.
- **Size attribution tooling**: Bloaty (compile-unit / symbol / section /
  segment, with size diffs for CI), cargo-bloat (per crate), twiggy
  (monomorphization), plus a pure-Rust equivalent that flags monomorphized
  instantiations.
- **C++26 safety**: core safety profiles (P3081R2) with "Pro.Bounds";
  `std::span::at()` (P2821R5); `std::expected` in freestanding; saturation
  arithmetic (P0543R3, shipped).
- **LSP 3.18** (2026-04-06): inline completions, dynamic document
  content, folding refresh, multi-range formatting, snippets in workspace
  edits, code-action kind documentation, debug message kind. 3.17 (2022)
  added inlay hints, semantic tokens, type hierarchies, the diagnostic PULL
  model, notebook documents.
- **Ecosystem size (2025-12/2026-01)**: npm 3.75 M, Go modules 2.16 M,
  Maven 751 k, PyPI 706 k, PHP 431 k, crates.io 208 k. 9.8 trillion
  downloads in 2025 (npm +65 % YoY, PyPI +51 % YoY).
- **LSP vs grep for agents**: LSP cut tool-output tokens 31-34 % on a
  149k-line TypeScript repo, with time roughly unchanged.

Consequences already folded into the plan: SIMD via a portable stdlib layer
(Go 1.27) rather than a Flint vectorizer; domains as a data table with a
lightweight tier and one deep tier (Reactant vs JLIR); a fast compiler as
the precondition for a good LSP (TypeScript 7); flow-sensitive region
inference as the target for the ownership model (Polonius Alpha);
`--print-size` shaped like Bloaty/cargo-bloat/twiggy; `--unsafe` framed as
a C++26-style safety profile; the LSP's first features taken from LSP 3.17/3.18;
and a curated (not mirrored) registry.

# Addendum (2026-10-20, later) — Phase 0 measurement + two codegen defects

- **Phase 0 measurement infrastructure (fdbf119, 660fab3, 26920a8).**
  `flintc --print-size` now reports the artifact's own size attribution
  (file bytes, section bytes, header bytes, .bss reserved, per-class symbol
  attribution, sections, dynamic dependencies, objects offered to the
  linker); `--print-size-json=PATH` writes it as JSON. Every number is read
  from the artifact, and `tests/test_print_size.sh` checks the report is
  TRUE against the file on disk. `tools/bench_gate.sh` is the measurement
  gate: runtime > +3%, binary > +5%, compile time > +10% fails, baselines
  are per machine (a baseline from another machine is refused rather than
  silently compared), and timings use the minimum of N runs. CI runs the
  size gate as a required step; the measurement job is advisory until a
  per-runner baseline exists.
- **DEFECT 1 (660fab3): invalid IR for fn-typed returns.** The default
  terminator built its return value with `ConstantInt::get(retType, 0)` --
  an integer constant with a POINTER type, because an `fn(...)` return type
  is a pointer (M4). A valid program (`fn get() -> fn(i64) -> i64 { }`)
  failed with an internal LLVM error ("ret i0 0") instead of compiling. The
  terminator now uses the null value OF THE RETURN TYPE. **A new type can
  invalidate code that predates it** -- the fix was to look for every use
  of the return type, not just the new one.
- **DEFECT 2 (fdbf119): copying a function value lost its fn type.**
  `h = inc; g = h; g(5)` failed with "undefined function 'g'". Three
  sites answered "is this binding a function value?" with three different
  rules; the binding site used the one that predates named-function
  values. There is now ONE predicate, `Symbol::isFunctionValue`.
- **One error per program, codegen side (660fab3).** After a codegen
  error the emitter kept going and LLVM's verifier printed a second
  message about the compiler's internals. emitStmt/emitExpr now stop after
  the first error, and `tests/test_one_error.sh` enforces it.
- **Codegen diagnostics now carry a location (26920a8):**
  `codegen: 9:12: cannot return a function value: ...`, matching the parse
  errors, which always had one.

# Addendum (2026-10-20, later still) — `--unsafe` implemented, and a diagnostic that was lying

- **`--unsafe` now does what the spec says (ea9e312).** It previously
  disabled ONLY the overflow checks: bounds checks could never be turned off,
  so a system programmer who had proven an index in range had no way to say
  so. Now it removes the overflow AND the bounds checks — the two
  *performance* checks — while ownership, borrow/lease, escape, null and
  divide-by-zero rules still apply. It is manual and terminal-only (no
  domain, profile, package or tool can enable it), VISIBLE (every unsafe
  build prints what is disabled and what still applies), and AUDITABLE (the
  artifact carries `flint:safety=unsafe` or `flint:safety=safe`, kept alive
  through `--gc-sections` via `llvm.used`, so an unsafe binary identifies
  itself later). New gate: `tests/test_unsafe_flag.sh` (6 checks).
- **`--check-bce` was SILENT on a cache hit (ea9e312).** The module cache
  stores the ALREADY-OPTIMIZED module, so on a cache hit the "checks
  emitted" count was gone and the diagnostic printed nothing. A diagnostic
  whose output depends on cache state is not a diagnostic: the flag now
  bypasses the cache and reports the same numbers every time. (Found by
  writing the unsafe gate, which compiled the same file twice.)

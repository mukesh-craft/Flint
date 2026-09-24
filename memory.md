# Flint Compiler — Complete Project Memory

**Last updated:** 2026-07-08
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

1. Read `REQUIREMENTS.md` for setup
2. Read `ROADMAP.md` for phase status
3. Test with `./flintc examples/hello.fl` (JIT run) or `./flintc examples/hello.fl output.ll && clang output.ll runtime.o -o hello && ./hello` (AOT)
4. Run all examples: `for f in examples/*.fl; do echo "=== $f ===" && timeout 30 ./flintc "$f" 2>&1 | head -5; done`
5. Run benchmarks: `for f in benchmarks/*.fl; do echo "=== $f ===" && timeout 60 ./flintc "$f" 2>&1; done`

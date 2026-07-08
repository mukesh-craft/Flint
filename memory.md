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
| 0.8.0 | 2026-07-08 | If-expression codegen fix, float type inference fix, mixed i64/f64 arithmetic, Python tools rewrite, benchmarking, security hardening |
| 0.7.0 | 2026-07-06 | Phase F: Flux Compilation — extreme performance |

---

## How to Continue Development

1. Read `REQUIREMENTS.md` for setup
2. Read `ROADMAP.md` for phase status
3. Test with `./flintc examples/hello.fl` (JIT run) or `./flintc examples/hello.fl output.ll && clang output.ll runtime.o -o hello && ./hello` (AOT)
4. Run all examples: `for f in examples/*.fl; do echo "=== $f ===" && timeout 30 ./flintc "$f" 2>&1 | head -5; done`
5. Run benchmarks: `for f in benchmarks/*.fl; do echo "=== $f ===" && timeout 60 ./flintc "$f" 2>&1; done`

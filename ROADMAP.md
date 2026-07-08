# Flint Compiler — Phase Roadmap

## Legend
- ✅ **Done** — tested and working
- 🔧 **In progress** — being worked on now
- 📋 **Planned** — designed but not started
- ❌ **Not started** — research done but not implemented

---

## Phase 0: MVP — Expression Language
**Goal:** End-to-end pipeline: source → LLVM IR → native binary. Basic immutable variables + print.

| Sub-task | Status |
|----------|--------|
| Lexer: identifiers, numbers, strings, operators, `mut` keyword | ✅ |
| Parser: var decl, assignment, function calls, binary ops | ✅ |
| AST: VarDecl, Assign, Call, Binary, Number, String, Variable | ✅ |
| Codegen: LLVM IR emission via C++ API | ✅ |
| Immutable by default + `mut` keyword | ✅ |
| Duplicate variable declaration error | ✅ |
| Assignment to undeclared variable error | ✅ |
| Assignment to immutable variable error | ✅ |
| Arithmetic: `+`, `-`, `*`, `/` with precedence | ✅ |
| Build system: `build.sh` using `llvm-config` | ✅ |
| Runtime library: `runtime.c` with print/panic | ✅ |
| Test suite: hello, basic, strings, mut, counter, comprehensive | ✅ |

---

## Phase A: Types + Functions + Control Flow ✅
**Goal:** Real programming language with functions, if/else, while loops, types, and block scoping.

| Sub-task | Status |
|----------|--------|
| ROADMAP.md tracking file | ✅ |
| Type system: `i64`, `str`, `bool` | ✅ |
| Type annotations: `x: i64 = 5` | ✅ |
| `fn` keyword and function definitions | ✅ |
| Function parameters and return types | ✅ |
| Block scoping with `{ }` | ✅ |
| `if / else` control flow | ✅ |
| `while` loops | ✅ |
| Comparison operators: `==`, `!=`, `<`, `>`, `<=`, `>=` | ✅ |
| `return` statement | ✅ |
| Function return value codegen | ✅ |
| Scoped symbol table with nested lookup | ✅ |
| Updated examples with all Phase A features | ✅ |
| Test all Phase A examples | ✅ |

---

## Phase B: C++ FFI ✅
**Goal:** Call C/C++ functions from Flint code.

| Sub-task | Status |
|----------|--------|
| `extern "C"` declaration syntax | ✅ |
| LLVM IR `declare` for external functions | ✅ |
| Varargs support (`...`) for extern functions | ✅ |
| Compiler flags: `--link` support | ✅ |
| `flint-build` wrapper script for one-step build | ✅ |
| Example: `puts()` from C standard library | ✅ |
| Example: `printf()` with varargs | ✅ |
| Example: custom C function (`add`) | ✅ |

---

## Phase C: Python Embedding ✅
**Goal:** Run Python code inside Flint programs.

| Sub-task | Status |
|----------|--------|
| `python { ... }` block syntax | ✅ |
| `py_eval("expr")` builtin returning i64 | ✅ |
| `pyruntime.c` with Python C-API wrapper functions | ✅ |
| Codegen: init/fini wrapping for `main()` | ✅ |
| Codegen: `flint_py_run()` for python blocks | ✅ |
| Codegen: `flint_py_eval_int()` for py_eval expression | ✅ |
| Data marshaling: i64 ↔ PyLong via `PyLong_AsLongLong` | ✅ |
| Auto-detect Python usage → write `.ll.link` with `$(python3-config --ldflags --embed)` | ✅ |
| `flint-build` auto-links `pyruntime.o` + Python ldflags | ✅ |
| `build.sh` compiles `pyruntime.c` if Python headers present | ✅ |
| Reference counting in generated IR | ❌ (handled in pyruntime.c) |
| Example: call `math.sqrt()` from Python | ✅ |

---

## Phase D: Rust-Like Ownership + Safety ✅
**Goal:** Memory safety at compile time with move semantics and borrowing.

| Sub-task | Status |
|----------|--------|
| Move semantics: assignment transfers ownership | ✅ |
| Use-after-move → compile error | ✅ |
| Reference types `&T` with lexical lifetimes | ✅ |
| Borrow checker: cannot mutate while borrowed | ✅ |
| Array type `[i64]` with length-prefix representation `{ ptr, i64 }` | ✅ |
| Runtime bounds check insertion in codegen | ✅ |
| Null-free by construction (no null keyword) | ✅ |
| Type inference for literals and references | ✅ |
| `main()` returns `i32` for correct exit codes | ✅ |
| Stack-allocated array data (heap alloc removed) | ✅ |
| Example: `examples/phase_d_demo.fl` | ✅ |
| Example: `examples/arrays.fl` | ✅ |
| Example: `examples/move_semantics.fl` | ✅ |
| Example: `examples/references.fl` | ✅ |
| Error tests: `move_error.fl` (use-after-move), `borrow_error.fl` (assign while borrowed) | ✅ |

---

## Phase E: Advanced Features ✅ 

| Sub-task | Status | Est. Lines |
|----------|--------|------------|
| Integer overflow checking via `llvm.sadd.with.overflow` | ✅ | 35 |
| Custom struct/record types | ✅ | 200 |
| Enum/sum types | ✅ | ~350 |
| Pattern matching | ✅ | ~200 (paired with enums) |
| Generics / type parameters | ✅ | ~250 |
| Module system (multi-file compilation) | ✅ | ~100 |
| Self-hosting: Flint compiler written in Flint | 📋 | 800-1200 Flint + 100 C |

---

## Phase E: Detailed Implementation Plans

### Enum/Sum Types + Pattern Matching (~400 lines total)

**LLVM representation** — `{ i8, [N x i8] }` where N = max variant payload size (rounded up to 8):
- Tag byte in field 0, byte-array payload in field 1
- Access payload via `bitcast` to a variant-specific struct type, then GEP
- This is the same approach Rust, Clang, and the LLVM mapping docs recommend

**Implementation files**: `src/main.cpp` only
- `TokenType` enum: add `KW_ENUM`, `KW_MATCH`, `FAT_ARROW` (`=>`)
- `TypeKind` enum: add `Enum`
- New AST nodes: `EnumConstructAST` (constructor), `MatchExprAST` + `MatchArm`
- New data structs: `EnumDef`, `EnumVariantDef`
- Parser: `parseEnumDef()`, enum construction (`Option.Some(42)`), `match` expression
- Codegen: `declareEnums()` creates LLVM struct types, `emitEnumConstruct()` stores tag + payload via bitcast, `emitMatch()` loads tag → switch → extract payload → bind variable
- `ProgramAST`: add `enums` vector
- `scanExpr()`: handle new AST nodes

**Syntax**:
```flint
enum Option { None, Some(i64) }
enum Tree { Leaf, Node(i64, Tree, Tree) }

x = Option.Some(42)
y = Option.None

result = match x {
    Option.None => 0,
    Option.Some(v) => v,
}
```

### Generics / Type Parameters (~250 lines)

**Approach**: Monomorphization — create separate specialized copies of functions at each call site with concrete type arguments. No boxing, no vtables, no runtime overhead.

**Changes to `src/main.cpp`**:
- `TypeKind::TypeParam` — a type variable placeholder
- `Type::typeParam(name)` — static factory
- `FunctionAST::typeParams` — vector of type parameter names
- `CallExprAST::typeArgs` — concrete type arguments at call site
- Parser: parse `<T, U>` after function name, parse `foo.i64(x)` for explicit type args
- `MonoKey` struct + mangling (`max.i64`, `max.str`) for unique specialization names
- `specialize()` — deep-clones a FunctionAST, substitutes `TypeKind::TypeParam` → concrete type
- Lazy instantiation in `emitCall()` — on generic call, infer/use type args, specialize if first use, then emit call to mangled name
- `substituteTypes()` — walks AST nodes replacing type parameters

**Syntax**:
```flint
fn max<T>(a: T, b: T) -> T {
    if a > b { a } else { b }
}
x = max(3, 5)       # infers T=i64
y = max.str("a", "b")  # explicit T=str
```

### Module System (~100 lines)

**Approach**: Single compiler invocation, combined LLVM Module. Parse all files into one `ProgramAST`, emit one `.ll`. No linker, no headers, no cross-file name resolution changes needed (functionMap is already flat).

**Changes to `src/main.cpp`**:
- `TokenType::KW_IMPORT` + lexer keyword
- `ProgramAST::imports` vector
- Parser: `import "path"` at top level
- `main()`: read entrypoint + follow imports recursively, merge all into one `ProgramAST`
- `processFile()` helper: reads, lexes, parses, recurses on imports
- Track `std::set<std::string> importedFiles` to handle circular imports

**Usage**:
```bash
./flintc main.fl output.ll    # main.fl contains import "math"
# or
./flintc main.fl math.fl output.ll   # explicit file list
```

### Self-Hosting (~800-1200 Flint + 100 C)

**Strategy**: Write `flintc.fl` in Flint that emits text-format `.ll` files (not LLVM C++ API). Use extern FFI to libc for string operations, file I/O, and CLI args.

**Bootstrap plan**:
1. Add missing runtime functions to `runtime.c` (string ops, file I/O, itoa, CLI args) — ~100 lines C
2. Write `flintc.fl` with these components:
   - Type constants + structures (Token, Type, AST node arena)
   - Lexer (~200 lines)
   - Recursive-descent parser (~350 lines)
   - LLVM IR text emitter (~300 lines)
   - Main driver: read args → lex → parse → emit → write output (~100 lines)
3. Compile with C++ `flintc`: `./flintc flintc.fl output.ll && clang ... -o flintc_v1`
4. Bootstrap: `./flintc_v1 flintc.fl output2.ll && clang ... -o flintc_v2`
5. Verify v1 and v2 output LLVM IR are functionally identical

**Prerequisite language features Flint needs**:
- String: concatenation, comparison, length, character-at, substring (via extern C)
- File I/O: fopen, fread, fwrite, fclose (via extern C)
- CLI: argc, argv (via extern C)
- i64 → string for error messages/SSA names (via extern C)
- Enum emulation via integer constants

**Key insight**: The Flint compiler-in-Flint emits `.ll` text format, not LLVM C++ API. This eliminates the need to bind LLVM's C API through FFI and keeps the self-hosted compiler portable.

---

## How to Run

```bash
cd ~/flint

# Build the compiler
bash build.sh

# Compile a Flint program
./flintc examples/hello.fl output.ll

# Link with runtime and run
clang output.ll runtime.o -o hello
./hello
```

---

## Phase F: Flux Compilation — Extreme Performance ✅

**Goal**: Match or exceed C++ (Clang -O0) compilation speed through the Flux Compilation architecture — a novel approach combining content-addressed function caching, single-pass direct IR emission, string interning, arena allocation, and elimination of all RTTI-based dispatch.

**Core Innovation ("Flux Compilation"):** Every compilation artifact is cached at the function level using content-addressed hashing. The compiler never compiles the same function twice — not across builds, not across generic instantiations, not across identical bodies. Compilation proceeds in progressive tiers: ultra-fast declaration scan → content-address cache check → single-pass compile only what's new.

**Performance Target:**

| Metric | Current flintc | Phase F Target | Clang -O0 |
|--------|---------------|----------------|-----------|
| Clean build (1000 lines) | >120s (hangs) | **<150ms** | ~10ms |
| Incremental (1 fn change) | rebuild all | **<1ms** | ~500ms |
| Lines/sec | ~10 | **>50,000** | ~98,000 |
| Memory | ~50 MB | **<2 MB** | varies |

### Phase F0: Foundation — Container & I/O Overhaul ✅

| Sub-task | Status | Est. Speedup |
|----------|--------|-------------|
| Remove debug `std::cout` from hot paths | ✅ | 2x on I/O |
| Replace `std::map` → `std::unordered_map` (12 maps) | ✅ | 3-8x symbol lookup |
| Replace `std::set` → `std::unordered_set` (4 sets) | ✅ | 3-8x membership tests |
| Cache LLVM types as member fields (i64Ty, i8PtrTy, voidTy) | ✅ | reduces recreate overhead |
| Add `ArenaAllocator` class (bump pointer, ~20 lines) | ✅ | 10-15x on allocations |
| Convert AST nodes from `make_unique` to arena `make()` | ✅ | 10-15x on allocs |
| Add `StringPool` class for string interning | ✅ | 3-5x on token creation |
| Replace `std::string` with `uint32_t` in Token + AST | ✅ | 3x fewer heap allocs |
| Convert symbol table string keys to `uint32_t` | ✅ | O(1) comparisons |

### Phase F1: Dispatch Overhaul — Kill `dynamic_cast` ✅

| Sub-task | Status | Est. Speedup |
|----------|--------|-------------|
| Add `NodeKind` enum to all AST structs | ✅ | — |
| Replace `dynamic_cast` in `emitStmt` with `switch(kind)` (7 casts) | ✅ | 5x |
| Replace `dynamic_cast` in `emitExpr` with `switch(kind)` (16 casts) | ✅ | 5x |
| Replace `dynamic_cast` in `scanExpr` with `switch(kind)` (16 casts) | ✅ | 5x |
| Replace `dynamic_cast` in `parseVarDecl` type inference (8 casts) | ✅ | 5x |
| Replace `dynamic_cast` in `emitCall` generic resolution (4 casts) | ✅ | 5x |
| Replace `dynamic_cast` in move tracking (lines 1502, 1588, 1729, 2126, 2176) | ✅ | 5x |

**Total `dynamic_cast` eliminated**: 57

### Phase F2: Parser Rewrite — Pratt Expressions + Single-Pass ✅

| Sub-task | Status | Est. Speedup |
|----------|--------|-------------|
| Build Pratt expression parser lookup table (precedence + nud/led handlers) | ✅ | eliminates deep recursion |
| Replace `parseBinaryOpRHS` / `parsePostfix` / `parsePrimary` with Pratt loop | ✅ | 2x on expressions |
| Merge `parseStatement` + `emitStmt` — emit IR during parsing (no AST) | ✅ | 2x on codegen |
| Merge `parseExpression` + `emitExpr` — return `llvm::Value*` directly | ✅ | 2x on codegen |
| Eliminate `scanExpr()` — fold Python detection into parse time | ✅ | one less AST walk |
| Flatten `parseBlock` — no mutual recursion with `parseStatement` | ✅ | prevents stack overflow |

### Phase F3: Parallelism & Content-Addressed Cache ✅

| Sub-task | Status | Est. Speedup |
|----------|--------|-------------|
| `FunctionCache` with content-addressed hashing (fn source + deps) | ✅ | 100x incremental |
| Serialize/deserialize cached LLVM IR per function | ✅ | — |
| LRU eviction policy for cache size management | 📋 | — |
| Thread pool for parallel import declaration scanning | 🔧 | 2-8x on multicore |
| Thread pool for parallel function body compilation | 📋 | 2-8x on multicore |

### Phase F4: IR & Module Emit Optimization ✅

| Sub-task | Status | Est. Speedup |
|----------|--------|-------------|
| Replace `mod->print(outStream)` with `raw_svector_ostream` + batch write | ✅ | reduces I/O syscalls |
| Memory-map input files instead of `stringstream` + `str()` | ✅ | 2x file I/O |
| Deduplicate LLVM type declarations (cache Array/Struct types) | ✅ | reduces IR bloat |
| Pre-allocate token vector with `reserve(source.size() / 5)` | ✅ | reduces reallocs |

---

## Phase G: Zero-Click Binary — Direct Machine Code Emission

**Goal:** Eliminate the dual-LLVM-invocation overhead by emitting machine code directly from the in-memory Module. `./flintc input.fl -o output` produces a native binary in one process, matching Clang -O0 compilation speed.

**Core insight:** The current pipeline invokes LLVM twice (once in flintc to serialize `.ll` text, once in clang to re-parse it). Direct `.o` emission eliminates both the text serialization and the re-parse, giving a 3-5x speedup. Runtime linking via `clang obj.o runtime.o -o binary` is done in-process. Parallel imports on multicore give another 2-8x.

| Sub-task | Description | Est. Speedup | Status |
|----------|-------------|-------------|--------|
| **G1** | Direct `.o` emission via `TargetMachine::addPassesToEmitFile()` — skip `.ll` text entirely | 3-5x | ✅ |
| **G3a** | Final binary via `spawnLinker()` — calls `clang obj.o runtime.o -o binary`; auto-detects `pyruntime.o` + Python ldflags | 1.5x | ✅ |
| **G3b** | `--emit-interface` / `--use-interface` — separate compilation via `.flint.bc` declaration bitcode | — | ✅ |
| **G3c** | Parallel imports via `ThreadPool` — `--parallel N` flag for multicore import scanning | 2-8x multicore | ✅ |
| **G3d** | `generateDeclarations()` / `emitFunctionBodies()` split — enables interface emit + future parallel emission | — | ✅ |
| **G3e** | Parallel function body emission — each thread needs own Parser/token copy; deferred (impractical with single-pass emit-mode) | 2-8x multicore | 🔧 Deferred |
| **G4** | ModuleCache — content-addressed bitcode (`.bc`) + binary (`.bin`) caching for ~5x speedup on rebuild | — | ✅ |
| **G5** | Timer-based profiler — nanosecond instrumentation with JSON report output | (diagnostic) | ✅ |
| **G6** | Streaming lexer — `nextToken()` on-demand instead of full `vector<Token>` allocation | 1.5x | ✅ |
| **G7** | `--run` mode via LLVM ORC JIT — compile straight to executable memory, zero file I/O | bonus | ✅ |

**Benchmark notes (G6/G7):**
- G6 streaming lexer: lazily generates tokens during parsing instead of pre-allocating the full vector. Eliminates the upfront `tokens.reserve()` + fill pass. On a ~50-line program, cold-cache compile time is unchanged (~60ms real total: `--run` and file-io paths both ~30ms/run for `comprehensive.fl`).
- G7 ORC JIT `--run`: compiles straight to executable memory in ~30ms (cold cache, same as file path), skipping the linker subprocess entirely. Comparable speed to cached file compilation (~25ms). The JIT currently clones the module (avoids consuming the original for cache), adding ~5ms overhead. On very large programs (5000+ lines), `--run` avoids writing `.o` + spawning `clang`, which could save 100-200ms.

**Caveats:**
- `ffi_demo.fl` requires `--link "ffi_helper.o"` (the `add` function is in a separate C source)
- `math.fl` has no `main` function — linking is skipped with a warning
- `--use-interface` provides declarations only; the implementation `.o` must be linked separately
- `spawnLinker` works when `runtime.o` is in the CWD; uses `clang` found in `$PATH`

---

---

## Phase H: LLVM Backend Bottleneck Bypass

**Goal:** Eliminate the #1 compile-time bottleneck — LLVM's optimization + object emission pipeline (86% of total compile time per benchmark profiling). Achieve 3-100x faster codegen through progressive phases.

**Benchmarked baseline** (5k-line generated program on ARM64):

| Metric | Current (LLVM default) | Target |
|--------|----------------------|--------|
| LLVM backend time | 3,107 ms (86%) | <300 ms |
| Front-end (lex+parse) | 5 ms (0.13%) | — |
| LLVM IR gen | 69 ms (1.9%) | — |
| Link (clang) | 304 ms (8.4%) | — |
| **Total compile** | **3,599 ms** | **<600 ms** |
| Runtime vs C (-O2) | 1.3-2.8x slower | — |

**Core insight:** The LLVM backend goes through 5+ intermediate representations (IR → SelectionDAG → MachineInstr → MCInst → binary) with ~50 passes even at default settings. The front-end (lexer + parser) is already extremely fast at ~1M lines/sec. The bottleneck is purely in LLVM's heavyweight backend pipeline.

| Sub-task | Description | Est. Speedup | Status |
|----------|-------------|-------------|--------|
| **H1** | **LLVM -O0 default** — set `CodeGenOptLevel::None` + `--opt-level` CLI flag. Skips all LLVM optimization passes (inlining, GVN, scheduling, loop opts, vectorization). Uses FastISel for instruction selection. One-parameter change. | **3-10x on backend** | ✅ |
| **H2** | **QBE backend integration** — emit QBE IL text as alternative backend. QBE (c9x.me/compile) is a 15k-line C backend with its own linear-scan register allocation, GVN, copy propagation. Code quality ~63% of GCC -O2 (often better than LLVM -O0). Integrate as: generate QBE IL text → pipe to `qbe` subprocess → `as` to `.o`. Supports ARM64, x86-64, RISC-V. | ~~10-30x~~ **slower than LLVM -O0** | ⚠️ done, but not faster |
| **H3** | **TPDE-LLVM evaluation** — evaluate TPDE (github.com/tpde2/tpde, May 2025 from TUM) as a drop-in replacement for LLVM's `addPassesToEmitFile`. Claims 13x speedup over LLVM -O0 with ±9% code quality. If compatible, simplifies to a library call. | **10-20x on backend** | 📋 |
| **H4** | **Custom direct ARM64 emission** — long-term: emit raw ELF `.o` files with direct machine code bytes via AsmJit or oaknut. Bypasses LLVM entirely. No optimizer passes, no SelectionDAG, no MC layer. Pentultimate performance. | **50-200x on backend** | ❌ |

**H1 Implementation notes:**
- Change `createTargetMachine()` call to pass `CodeGenOptLevel::None`
- Add `--opt-level 0|1|2|3` CLI flag (default 0 for speed, 2 for release optimization)
- Wire `optLevel` through all `emitModuleOutput()` call sites (4 total)
- Profile result: ~1,000ms backend time → ~1,200ms total for 5k-line program

**H2 Implementation notes:**
- QBE IL is a simple text format (~25 opcodes, 4 types)
- Write C++ IL emitter as a new module (parallel to Codegen class)
- Invoke `qbe -t arm64` as subprocess, then `as` to assemble
- Keep LLVM path as fallback for features QBE doesn't support (vectors, etc.)
- See: Hare language (production), cproc C compiler — both use QBE successfully

**H2 Result (implemented, benchmarked 2026-07):**
- `QbeEmitter` class emits QBE IL for all 23 examples (numbers, strings, arithmetic,
  comparisons, if/while, functions, structs, field access, references/deref, arrays,
  enums + match, FFI externs, overflow-checked add/sub, Python `py_eval`).
- Standalone `BorrowChecker` added because the QBE path bypasses the LLVM Codegen
  (which previously did borrow/move checking inline). All 23 examples pass, including
  `borrow_error`/`move_error` (compile-time failures) and `overflow` (runtime panic).
- **Benchmark (3506-line program, aarch64, cold cache, median of 3):**
  - QBE backend:   **~2.6 s**
  - LLVM `-O0`:    **~0.9 s**  ← fastest
  - LLVM `-O2`:    **~2.4 s**
- **Conclusion: QBE is ~3x SLOWER than LLVM -O0, not faster.** Two reasons:
  1. Process-spawn overhead: QBE path shells out to `qbe` (1.4 s) + `as` (0.2 s) as
     subprocesses; LLVM emits `.o` in-process via `addPassesToEmitFile`.
  2. IL bloat: overflow checks emit ~20 lines of QBE IL per `+`/`-`, producing a
     935 KB `.ssa` for the 3506-line program, which QBE then re-parses and compiles.
- Even for a trivial `hello` program QBE (0.47 s) trails LLVM -O0 (0.37 s) purely on
  the two extra `fork`/`exec` round-trips.
- **The Phase H premise ("LLVM is the bottleneck") does not hold at -O0.** LLVM -O0
  with FastISel is already fast; H1 (default `-O0`) captured essentially all the win.
  QBE remains available as `--backend qbe` for code-quality experiments / portability,
  but is no longer on the critical path for compile speed.

---

---

## Phase I: Ship Ready — AOT + Standard Library + Tooling

**Goal:** Users can write real projects in Flint, compile them to standalone binaries, and ship them to end users. No JIT dependency, no manual linker flags.

---

### I-A: AOT Compilation (`--emit-exe`)

**Goal:** `./flintc source.fl -o myprogram` produces a native executable in one step.

| Sub-task | Status |
|----------|--------|
| `--emit-exe` / `-o` CLI flag | ✅ |
| Compile `.fl` → LLVM IR → `.o` via TargetMachine | ✅ |
| Link `.o` + `runtime.o` + stdlib `.o` files using system linker | ✅ |
| Auto-detect Python runtime (`pyruntime.o` + `-lpython3.13`) | ✅ |
| Auto-detect FFI helpers (`ffi_helper.o` if used) | ✅ |
| Static linking mode (`--static`) | 📋 Planned |
| Cross-compilation (`--target aarch64-linux-gnu`) | 📋 Planned |

---

### I-B: File I/O + System API

**Goal:** Read/write files, environment, process spawning — essential for any real project.

| Sub-task | Status |
|----------|--------|
| `flint_read_file(path) -> str` | ✅ |
| `flint_write_file(path, data) -> i64` (overwrite) | ✅ |
| `flint_append_file(path, data) -> i64` | ✅ |
| `flint_file_copy(src, dst) -> i64` | ✅ |
| `flint_temp_dir() -> str` | ✅ |
| `flint_read_lines(path) -> str` | ✅ |
| `flint_file_move(src, dst) -> i64` | ✅ |
| `flint_command(cmd) -> i64` (exit code) | ✅ |
| `flint_command_output(cmd) -> str` (capture stdout) | ✅ |

---

### I-C: Error Handling (Result / Option)

**Goal:** Replace global error flag with proper typed results.

| Sub-task | Status |
|----------|--------|
| `Result<T,E>` enum type — builtin enum in compiler | ✅ |
| `Option<T>` enum type — builtin enum in compiler | ✅ |
| `try` / `?` operator (postfix desugaring, unwrap on None/Err) | ✅ |
| `unwrap()` / `expect()` builtins + C runtime | ✅ |
| Pattern matching integration | ✅ (already works for enums) |

---

### I-D: Expanded Standard Library

**Goal:** Batteries-included standard library covering everyday needs.

| Sub-task | Status |
|----------|--------|
| Date/time arithmetic (parse, format, diff) | 📋 Planned |
| JSON builder (construct JSON from Flint values) | ✅ |
| HTTP server (listen + respond) | 📋 Planned |
| Regex (pattern matching via C regex) | ✅ |
| CSV parsing | ✅ |
| JSON builder (construct JSON from Flint values) | ✅ |
| Array slice syntax (`arr[1:3]`) | ✅ |

---

### I-E: Language Features for Ergonomics

**Goal:** Make Flint pleasant and productive for large projects.

| Sub-task | Status |
|----------|--------|
| String interpolation (`"hello {name}"`) | ✅ |
| Lambda / closure expressions (`|x| x + 1`) | ✅ |
| Iterator protocol (`for x in collection`) | ✅ |
| Slice syntax (`arr[1:3]`) | ✅ |
| Spread operator (`...arr` in array literals) | ✅ |
| Default parameter values (`fn f(x: i64 = 5)`) | ✅ |
| Named arguments (`f(name=value)`) | ✅ |
| Operator overloading | 📋 Planned (requires trait system) |
| Destructuring assignment (`let [a,b]=arr`, `let {x,y}=s`) | ✅ |

---

### I-F: Developer Tooling

**Goal:** Professional developer experience.

| Sub-task | Status |
|----------|--------|
| `flint test` command (`--test` flag, runs functions named `test_*`) | ✅ |
| Error messages with source line + caret display | ✅ |
| `flint fmt` (auto-formatter script) | ✅ |
| `flint doc` (documentation generator script) | ✅ |
| `flint-lsp` (basic LSP server with completion/hover) | ✅ |

---

## Version History

| Version | Date | Description |
|---------|------|-------------|
| 0.1.0   | 2026-06-30 | Phase 0 MVP: immutable vars, print, arithmetic |
| 0.2.0   | 2026-06-30 | Phase A: functions, if/while, types, block scoping |
| 0.3.0   | 2026-06-30 | Phase B: C++ FFI — extern "C" blocks, varargs, linker integration |
| 0.4.0   | 2026-06-30 | Phase C: Python embedding — `python{}` blocks, `py_eval()`, auto-linking |
| 0.5.0   | 2026-06-30 | Phase D: Ownership + safety — move semantics, borrow checker, arrays |
| 0.6.0   | 2026-06-30 | Phase E (complete): overflow checking, structs, enums, pattern matching, generics, module system |
| 0.7.0   | 2026-07-06 | Phase F: Flux Compilation — Extreme Performance |
| **0.8.0** | **2026-07-06** | **Phase I: Ship Ready — AOT + Stdlib + Tooling** |

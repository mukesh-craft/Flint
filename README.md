# Flint Programming Language

A native compiled language on Termux/Android with Rust-like memory safety, LLVM codegen, and Python integration.

## Quick Start

```bash
# Install dependencies (Termux)
pkg update && pkg install clang llvm python make bash

# Build the compiler
cd ~/flint
bash build.sh

# Run a Flint program (JIT mode — one command)
./flintc examples/hello.fl

# Compile to standalone executable
./flintc examples/hello.fl -o hello
./hello
```

## Documentation

- **[REQUIREMENTS.md](REQUIREMENTS.md)** — Full setup and build requirements
- **[ROADMAP.md](ROADMAP.md)** — Phase-by-phase feature roadmap
- **[memory.md](memory.md)** — Project memory for agent-assisted development

## Features

- **Compiled to native code** via LLVM IR — no interpreter, no transpiler
- **Immutable by default** — variables are immutable unless declared with `mut`
- **Memory safe** — ownership + borrow checker, no null keyword
- **C FFI** — call any C function via `extern "C"`
- **Python embedding** — run Python from Flint via `python{ }` blocks and `py_eval()`
- **Overflow checking** — integer arithmetic is checked by default (`--unsafe` to disable)
- **Generics** — monomorphized, zero runtime cost
- **Enums + pattern matching** — sum types with `match`
- **Developer tools** — `flint-lsp` (LSP), `flint-fmt` (formatter), `flint-doc` (docs)

## Syntax

```flint
# Variable declaration (immutable by default)
x = 5

# Mutable variable
mut y = 10
y = y + 1

# Function with return type
fn add(a: i64, b: i64) -> i64 {
    a + b
}

# If expression
result = if x > 3 { 42 } else { 0 }

# While loop
mut i: i64 = 0
while i < 10 {
    i = i + 1
}

# String + interpolation
name = "world"
greeting = "hello {name}!"
```

## Build

```bash
bash build.sh
```

This compiles:
- `src/main.cpp` → `./flintc` (compiler)
- `runtime/runtime.c` → `./runtime.o`
- Optional: `pyruntime.c` → `./pyruntime.o`, `ffi_helper.c` → `./ffi_helper.o`
- Stdlib: `flint_serial.o`, `flint_crypto.o`, `flint_net.o`, `flint_tensor.o`, `flint_ai.o`, `flint_ai_opt.o`

## Running

### JIT mode (fastest iteration)
```bash
./flintc examples/hello.fl           # compiles + runs via ORC JIT
./flintc examples/comprehensive.fl   # full example
```

### AOT to executable
```bash
./flintc examples/hello.fl -o hello
./hello
```

### AOT to LLVM IR
```bash
./flintc examples/hello.fl output.ll
clang output.ll runtime.o -o hello
./hello
```

## Python Developer Tools

Requires Python 3.x.

```bash
python3 flint-lsp    # LSP server (stdio)
python3 flint-fmt source.fl             # format to stdout
python3 flint-fmt source.fl -o out.fl   # format to file
python3 flint-doc source.fl             # docs to stdout
python3 flint-doc source.fl --output docs.md  # docs to file
```

## Benchmarks

Run the full benchmark suite:
```bash
for f in benchmarks/*.fl; do echo "=== $f ===" && timeout 60 ./flintc "$f" 2>&1 | head -5; done
```

| Benchmark | Flint | C (AArch64) | Ratio |
|-----------|-------|-------------|-------|
| sum_array (10M) | ~15 ms | ~11 ms | 1.38× |
| primes (10M) | ~530 ms | ~250 ms | 2.1× |
| fib(45) | ~12.7s | ~7.3s | 1.74× |
| pi (100M iters) | ~780 ms | — | f64 loop |

Binary size: Flint ~125 KB (includes runtime), C ~6 KB.

## Testing

```bash
# All examples
for f in examples/*.fl; do echo "=== $f ===" && timeout 30 ./flintc "$f" 2>&1 | head -5; done

# Specific file
./flintc examples/basic.fl
```

## Compiler Flags

```
--unsafe          Skip overflow checks (release mode)
--opt-level 0|1|2|3   LLVM optimization level (default: 2)
-o <path>         Output executable path (AOT)
--emit-llvm       Output LLVM .ll text
--emit-obj        Output .o object file
--emit-interface  Emit .flint.bc declaration file
--use-interface   Use .flint.bc for declarations
--run             JIT mode (compile + run without file output)
--link <obj>      Link additional .o files
--parallel N      Parallel import scanning (N threads)
-dump-ast         Print parsed AST (debug)
```

## Project Memory

This repo uses `memory.md` to track project context, bug fixes, and decisions for agent-assisted development. See `memory.md` for:
- How the project started
- Phase-by-phase history
- All bugs found and fixed
- Important code locations
- Key design decisions
- Known limitations

## Version History

| Version | Date | Description |
|---------|------|-------------|
| 0.8.0 | 2026-07-08 | If-expr codegen, float type inference, mixed i64/f64, Python tools |
| 0.7.0 | 2026-07-06 | Phase F: Flux Compilation — extreme performance |

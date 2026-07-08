# Flint Compiler — Requirements & Setup

## Platform

- **Primary:** Termux on Android (AArch64 / arm64-v8a)
- **Also works:** Linux AArch64 (ARM64)
- **LLVM version:** LLVM 18+ (tested with LLVM 18 on Termux)
- **Clang version:** Matching LLVM version

## Required Packages (Termux)

```bash
pkg update && pkg upgrade
pkg install clang llvm python make bash
```

Verify:
```bash
llvm-config --version   # should show 18+
clang --version
python3 --version
```

## Optional Packages

- Python 3.x headers (for JIT Python symbol resolution):
  ```bash
  pkg install python-dev
  ```

## Build

```bash
cd ~/flint
bash build.sh
```

Expected output:
```
=== Building Flint compiler (flintc) ===
 profiled compiler: ./flintc_prof   (optional)
=== Build complete ===
 compiler: ./flintc
 profiled: ./flintc_prof
 runtime: ./runtime.o
 tensor: ./flint_tensor.o
 flint_ai: ./flint_ai.o
 flint_ai_opt: ./flint_ai_opt.o
 flint_serial: ./flint_serial.o
 flint_crypto: ./flint_crypto.o
 flint_net: ./flint_net.o
```

## Build Details

The build compiles:
1. `src/main.cpp` → `flintc` (the compiler, ~575 KB)
2. `runtime/runtime.c` → `runtime.o`
3. Optional: `pyruntime.c` → `pyruntime.o` (if Python headers available)
4. Optional: `ffi_helper.c` → `ffi_helper.o`
5. Stdlib modules: `flint_serial.c`, `flint_crypto.c`, `flint_net.c`, `flint_tensor.c`, `flint_ai.c`, `flint_ai_opt.c`

## Run

### JIT Mode (default — no output file)
```bash
./flintc examples/hello.fl
```

### AOT Compile to LLVM IR
```bash
./flintc examples/hello.fl output.ll
```

### AOT Compile to Object File
```bash
./flintc examples/hello.fl output.o
```

### AOT Compile to Executable (one step)
```bash
./flintc examples/hello.fl -o hello
./hello
```

### Run with Optimization Flags
```bash
./flintc examples/hello.fl --unsafe   # skip overflow checks (release mode)
./flintc examples/hello.fl --opt-level 0  # LLVM -O0 (fastest compile)
```

### Run Tests
```bash
./flintc examples/hello.fl
for f in examples/*.fl; do timeout 30 ./flintc "$f" 2>&1 | head -5; done
```

## Python Developer Tools

Requires Python 3.x.

```bash
python3 flint-lsp   # LSP server (stdio)
python3 flint-fmt source.fl   # format to stdout
python3 flint-fmt source.fl -o formatted.fl   # format to file
python3 flint-doc source.fl   # extract docs to stdout
python3 flint-doc source.fl --output docs.md   # extract docs to file
```

## Project Structure

```
flint/
├── src/main.cpp          # Main compiler (C++/LLVM, ~7600 lines)
├── runtime/
│   ├── runtime.c         # C runtime library
│   ├── pyruntime.c       # Python C-API wrapper
│   ├── flint_ai.c        # AI engine runtime
│   ├── flint_ai_opt.c    # AI optimizer (SIMD, multi-threaded, f32)
│   ├── flint_tensor.c    # Tensor operations
│   ├── flint_serial.c    # Serial port I/O
│   ├── flint_crypto.c    # Cryptographic functions
│   └── flint_net.c       # Network functions
├── examples/             # 36 example programs
├── benchmarks/           # Performance benchmarks
├── flint-lsp             # Python LSP server
├── flint-fmt              # Python formatter
├── flint-doc              # Python documentation generator
├── build.sh              # Build script
├── memory.md             # Project memory / context for agents
├── REQUIREMENTS.md        # This file
├── README.md              # Project README
└── ROADMAP.md             # Phase-by-phase roadmap
```

## Known Limitations

1. **String concat is O(n²):** `flint_str_concat` allocates + copies full string each call. Avoid in tight loops.
2. **No `true`/`false` literals:** Use `1`/`0` for booleans.
3. **strrev at n=100K:** May panic due to memory exhaustion from O(n²) concat. n=10K works fine.
4. **LLVM ISEL edge cases:** Mixed i64/f64 arithmetic is mostly fixed but may still crash in rare unsupported cases on AArch64.
5. **No self-hosting yet:** The compiler is written in C++, not Flint.

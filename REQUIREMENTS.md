# Flint Compiler — Requirements & Setup

## Platform Support (v0.17: any device)

| Tier | Platform | Status |
|------|----------|--------|
| 1 | Android ARM64 (Termux) | ✅ primary dev platform |
| 1 | Linux x86_64 | ✅ CI-tested |
| 1 | Linux ARM64 | ✅ CI-tested |
| 2 | macOS (Apple silicon + Intel) | 🟡 builds via Xcode CLT + brew LLVM (CI) |
| 3 | Windows (MinGW/LLVM-MinGW) | 🟡 v0.20: all runtime + OS layer compile for x86_64; real `hello.exe` cross-linked from ARM; native CI runs tests |
| 3 | Windows (MSVC) | 📋 needs native threads + full-toolchain CI run |

Windows notes: link programs with `-lws2_32 -lpthread` (added automatically by
`flintc` for Windows targets); `flint_regex_*` return err-flagged stubs
(no POSIX engine); MSVC needs the remaining thread shim (PORT notes in code).

Cross-compilation: `./flintc --target <triple> program.fl -o program.o`
(e.g. `--target x86_64-unknown-linux-gnu` on ARM). JIT always runs host code.

## Requirements

- **LLVM version:** LLVM 18+ (`llvm-config` on PATH)
- **Clang version:** matching LLVM (`clang`/`clang++` on PATH)
- **Python 3.x** (headers for `pyruntime.o`, else skipped)
- **make**, **bash**

## Required Packages (Termux)

```bash
pkg update && pkg upgrade
pkg install clang llvm python make bash
```

## Required Packages (Debian/Ubuntu)

```bash
sudo apt-get update && sudo apt-get install -y clang llvm python3 make
```

## Required Packages (macOS)

```bash
brew install llvm python make
export PATH="$(brew --prefix llvm)/bin:$PATH"
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

1. **String concat is O(n²):** `flint_str_concat` allocates + copies full string each call. Avoid in tight loops (use the `flint_sb_*` builder path instead).
2. **`true`/`false` literals exist:** they evaluate to `1`/`0` (verified: `print(true)` → `1`). Older docs saying otherwise are stale.
3. **strrev at n=100K: fixed 2026-09-13 (R5).** Rebuilt on the linear `flint_sb_*` builder; now passes with correct checksum (5044012, verified 2026-09-24).
4. **LLVM ISEL edge cases:** Mixed i64/f64 arithmetic is mostly fixed but may still crash in rare unsupported cases on AArch64.
5. **Self-hosting: done.** Lexer/parser/emitter are Flint programs (`stage1-3/*.fl`); bootstrap A/B/C + stable promotion green, V1 gate 17/17 (2026-09-24).

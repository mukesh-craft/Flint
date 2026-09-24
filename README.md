# Flint Programming Language

A safe, small, fast compiled language for **every device** — phone, laptop,
server. Native binaries via LLVM, Rust-like ownership plus generational
leases, Python-style ergonomics, batteries-included tooling.

## Quick Start

```bash
# Install dependencies (Termux shown; Debian: apt install clang llvm python3
# make — see REQUIREMENTS.md for macOS/Windows)
pkg update && pkg install clang llvm python make bash

# Build the compiler
cd ~/flint
bash build.sh

# Run a Flint program (JIT mode — one command)
./flintc examples/hello.fl

# Or scaffold your own project and run it
./flintc new myapp && cd myapp && flintc run main.fl

# Compile to standalone executable
./flintc examples/hello.fl -o hello
./hello
```

New here? Work through `tutorial/01_hello.fl` … `tutorial/08_wrap.fl`
(`bash tests/run_tutorial.sh ./flintc` checks your answers), then
`flintc api` for the builtin reference.

## Documentation

- **[REQUIREMENTS.md](REQUIREMENTS.md)** — Full setup and build requirements
- **[ROADMAP.md](ROADMAP.md)** — Phase-by-phase feature roadmap
- **[memory.md](memory.md)** — Project memory for agent-assisted development

## Features

- **Compiled to native code** via LLVM IR — no interpreter, no transpiler;
  runs on Android, Linux, macOS (`--target` cross-compiles, incl. WASM objects)
- **Immutable by default** — variables are immutable unless declared with `mut`
- **Memory safe** — ownership + borrow checker, generational Aegis leases,
  no null keyword, bounds traps, overflow panics (all loud, never silent)
- **Small binaries** — hello world is ~5 KB (per-function sections + linker GC)
- **Fast builds** — partitioned parallel backend, tiers, incremental caching
- **C FFI** — call any C function via `extern "C"`; ship Flint *to* C via `--emit-header`
- **Python embedding** — run Python from Flint via `python{ }` blocks and `py_eval()`
- **Generics** — monomorphized, zero runtime cost
- **Enums + pattern matching** — exhaustive, value-yielding `match`
- **Maps, methods, lambdas** — `map{"k": v}`, `s.upper()`, `|x| x + 1` closures
- **Concurrency** — `parallel for`, threads, bounded MPMC channels
- **Packages** — `flint.toml` + `flintc fetch` + pinned `flint.lock`
- **Developer tools** — `flintc run/build/test/fmt/doc/lsp/new/api/help`, `flint test`, `flint fmt --check`

## Status (2026-09-24, verified on Termux AArch64)

- **V1 gate 17/17 green** (`bash tests/v1_gate_check.sh ./flintc --full`):
  self-host bootstrap A/B/C + stable promotion (fixed point
  byte-identical), stage-3 ladder 21/21, tutorial 8/8, differential,
  sanitizers, opt-identity, registry, driver — see `memory.md`
- **Self-hosted** — lexer/parser/emitter are Flint programs
  (`stage1-3/*.fl`); C++ `flintc` bootstraps them, then they compile
  themselves
- **Known gap (P0):** `for x in <array/str>` collection iteration
  miscompiles on the C++ path (ranges and `range()` are fine) —
  tracked as ROADMAP Phase J/A2
- Next work is ordered in **[ROADMAP.md](ROADMAP.md) Phase J** (post-V1
  plan: lock-in + P0 crash → measure + safety wins → bigger bets)

## Syntax

```flint
# Top level holds globals, structs, enums, and functions ...
x = 5
mut y = 10

struct Pt { x: i64, y: i64 }
enum Opt { None, Some(i64) }

# ... everything that RUNS lives inside a function
# (top-level statements parse but do not execute yet — script mode is next)
fn main() -> i64 {
    # Mutable globals reassign (also works at top level as a declaration site)
    y = y + 1
    y += 1   # also -= *= /= %= (same overflow checks, less writing)

    # Logic: && / || / !  (or Python-style and / or / not)
    ok = x > 3 && y < 20
    done = not ok
    print(ok)

    # For loops: ranges, range(), collections
    for i in 0..3 { print(i) }
    for i in range(3) { print(i) }
    for i in range(2, 5) { print(i) }

    # While loop (break / continue work; continue keeps for-loop increments)
    mut i: i64 = 0
    while i < 10 {
        i = i + 1
    }

    # Maps: string keys, i64 values
    scores = map{"alice": 10, "bob": 20}
    print(scores.get("alice"))
    scores.set("carol", 30)
    print(scores.len())

    # Methods (zero-cost sugar over builtins)
    name = "flint"
    print(name.len())
    print(name.upper())
    print(name.startswith("fl"))

    # Struct values and if expressions
    p = Pt { x: 3, y: 4 }
    print(p.x + p.y)
    result = if x > 3 { 42 } else { 0 }
    print(result)

    # Value matching (must cover every variant)
    v = Opt.Some(40)
    r = match v {
        Opt.None => 0,
        Opt.Some(n) => n + 2,
    }
    print(r)

    # Lambdas (by-value captures) and closures as values stay direct calls
    dbl = |n| n + n
    base = 100
    addb = |n| n + base
    print(dbl(21))

    # Error handling: Result/Option, try/?, unwrap — or the err flag
    v2 = Opt.Some(5)
    print(try v2)
    code = flint_err_occurred()

    # String + interpolation (UTF-8 bytes; str_length counts bytes,
    # \u00e9-style escapes supported, use str_codepoint_at for code points.
    # {{ and }} write literal braces)
    who = "world"
    greeting = "hello {who}!"
    print(greeting)

    # Arithmetic is C-like: % is the remainder (-7 % 3 == -1),
    # integer / and % by zero panic (use --unsafe to skip checks)
    print(-7 % 3)
    print(add(40, 2))
    0
}

# Function with return type (order does not matter)
fn add(a: i64, b: i64) -> i64 {
    a + b
}
```

# Packages: flint.toml next to your program, then import by name
#   [dependencies]
#   calc = "https://github.com/you/flint-calc"
# import "calc"   (auto-fetches on first build; flint.lock pins revisions)
# flintc fetch <git-url> [name]   # fetch manually
# flintc --offline ...             # never touch the network

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

## Tooling (v0.19: one entry point)

```bash
flintc run main.fl -- args...   # run via JIT (explicit)
flintc build main.fl -o myapp   # native binary (needs -o)
flintc test [files...]          # run test_* functions (default: tests/*.fl)
flintc fmt main.fl [--check]    # format (--check fails if unformatted)
flintc doc main.fl              # docs to stdout (--output docs.md for file)
flintc lsp                      # language server over stdio (editors)
flintc fetch <git-url> [name]   # fetch a package (records flint.toml + lock)
flintc new myapp                # scaffold main.fl + flint.toml
flintc help [cmd]               # full help, no docs re-read needed
```

Requires Python 3.x for fmt/doc/lsp (standalone `flint-fmt`, `flint-doc`,
`flint-lsp` scripts still work directly).

## Learn (v0.21: tutorial + API + agent bench)

```bash
bash tests/run_tutorial.sh ./flintc  # 8 runnable lessons in tutorial/
flintc api                            # builtin reference (llms.txt style)
flintc api --format md | flintc api map
bash agent-bench/check.sh ./flintc    # 5 scored tasks (add solution.fl first)
```

## Concurrency (v0.22: share by communicating)

All of this runs inside functions (top-level statements do not execute yet).

```flint
fn dbl(x: i64) -> i64 { x + x }

fn worker(a: i64) -> i64 {
    ch: chan = flint_int_to_ptr(a)
    ch.send(41 + 1)
    0
}

fn main() -> i64 {
    # Data-parallel loops (work-stealing pool under the hood).
    # Bodies see the loop variable + functions only: outer variables
    # and globals are NOT captured yet (worker functions take just `i`).
    parallel for i in 0..10 {
        print(dbl(i))
    }

    # Threads + channels (bounded MPMC queues of i64 — the safe handoff)
    ch = flint_chan_new(4)            # capacity >= 1
    t = flint_thread_create(&worker, flint_ptr_to_int(ch))
    print(ch.recv())                  # blocks; panics if closed+empty
    flint_thread_join(t)
    ch.close()                        # wakes all; sends fail, buffered recvs drain
    flint_chan_free(ch)
    0
}
```

Rules: values crossing threads are copied (i64) or passed by handle
(`flint_ptr_to_int`); join threads before freeing what they touch; Aegis
leases stay memory-safe across threads (generational checks) but do NOT
prevent data races — use channels for handoff, borrows only within one
thread. `parallel for` bodies must be independent per iteration.

## Flint as a library (v0.22: C ABI experiment)

```bash
flintc lib.fl --emit-header lib.h -o lib.o   # prototypes for every fn
# incl. lib.h from C, link lib.o + runtime.o: C calls Flint directly
```

WASM: `flintc --target wasm32-unknown-unknown prog.fl -o prog.o` emits
valid WASM objects today; full WASI execution (libc + `_start`) is next.

## Benchmarks

Run the full benchmark suite (19 `.fl` files: 6 shootout workloads with
C/C++/Python mirrors + 13 `test_*` if/call micro-probes):
```bash
for f in benchmarks/*.fl; do echo "=== $f ===" && timeout 60 ./flintc "$f" 2>&1 | head -5; done
```

Measured 2026-09-06, Termux AArch64, JIT default (in-program timers,
`clang -O2` for the C column):

| Benchmark | Flint | C (AArch64) | Ratio |
|-----------|-------|-------------|-------|
| sum_array (10M) | ~17 ms (~15 ms `--unsafe`) | ~8 ms | ~2.1× |
| primes (10M) | ~459 ms | ~202 ms | ~2.3× |
| fib(45) | ~11.7 s | ~8.6 s | ~1.4× |
| pi (100M iters) | ~714 ms | ~718 ms | ~1.0× |

Binary size: `flintc build examples/hello.fl -o hello` → ~4 KB
(stripped + section GC; runtime included). Known gap: `strrev` on large
inputs panics (codegen string-concat issue, tracked in `memory.md`).

Measured 2026-09-12, Termux AArch64, same session (steady-state runs;
in-program timers for runtime, wall clock for compile):

### Runtime — large workloads (Flint JIT vs C/C++ `-O2` vs Python)

| Benchmark | Flint default | Flint `--unsafe` | C | C++ | Python |
|-----------|---------------|------------------|---|-----|--------|
| sum_array 10M | 19.3 ms | 16.4 ms | 9.2 ms | 9.8 ms | 175.7 ms |
| pi 100M iters | 777.5 ms | n/m | 782.8 ms | 778.8 ms | >60 s (timeout) |
| primes 10M | 542.9 ms | 481.4 ms | 199.1 ms | 203.9 ms | 2,092 ms |
| fib(45) | 12,735 ms | 8,483 ms (beats C ~9%) | 9,328 ms | 9,333 ms | skipped (est. 10+ min) |
| strrev | CRASH (SIGABRT) | CRASH | 5.5 ms | 32.0 ms | 444.9 ms |

`--unsafe` skips overflow + bounds checks: faster, but drops the safety
that justifies Flint vs C. Failures and skips reported as-is, not hidden.

### Compile speed — large files (wall clock, `emit-llvm` unless noted)

| Compiler | Input (size) | Time | Throughput |
|----------|--------------|------|------------|
| `clang++ -O2 -c` | `src/main.cpp` (495 KB, 10,542 lines) | ~74,400 ms | 6.5 KB/s, 142 lines/s |
| `clang++ -O0 -emit-llvm` | `src/main.cpp` | ~35,400 ms | ~14 KB/s |
| `clang -O2 -c` | `runtime/runtime.c` (44 KB) | ~1,162 ms | 38 KB/s |
| `flintc` default | `stage2/flint_parse.fl` (112 KB, 3,845 lines) | ~909 ms | 123 KB/s, 4,230 lines/s |
| `flintc --fast` (Slipstream) | `stage2/flint_parse.fl` | ~200 ms | 560 KB/s, 19,225 lines/s |
| `flintc` default | `stage1/flint_lex.fl` (48 KB) | ~277 ms | 174 KB/s |
| `flintc --fast` | `stage1/flint_lex.fl` | ~123 ms | 394 KB/s |

Same-size comparison: `--fast` 123 ms vs C 1,162 ms (~9.5x).
O0-to-O0 (no optimization either side): Flint still leads ~38x wall
(921 ms vs 35,400 ms) — the remainder is LLVM header parsing and C++
language complexity, not optimization passes. `main.cpp` emits 16.3 MB
of IR (33x expansion) vs Flint's 628 KB from 112 KB (5.6x).

Caveats: different input languages (headers are C++'s real cost);
`emit-llvm` stops before backend/link while `-c` emits full objects;
Termux-class devices throttle ±2x across sessions — compare same-session
medians. `stage3/flint_emit.fl` (235 KB) is rejected by both modes
(`codegen: undefined function 'fa_set'`, same gap family as D3 in
`COMPATIBILITY.md`) — fixing it unlocks a bigger showcase.

### Slipstream trade-off

`--fast` compiles faster (2.2x at 48 KB → ~4.5x at 112 KB) but emits
larger, less-optimized code: `flint_parse.fl` IR is 828 KB vs 628 KB
default (+32%), and `sum_array` runs 42.5 ms vs 19.3 ms (2.3x slower).
Use `--fast` for iteration, `--opt-level 3 --unsafe` for speed demos.

## Testing

What exists and how to run it:

| Suite | Files | Lines | Run |
|-------|-------|-------|-----|
| `tests/` smoke tests | 5 `.fl` (`t_hello` 8, `t_arith` 14, `t_flow` 27, `t_funcs` 15, `t_types` 23) | 87 | `bash tests/run.sh ./flintc` |
| `tests/` runners | `run.sh` 26, `run_tutorial.sh` 18, `test_registry.sh` 43 | 87 | `bash tests/test_registry.sh` |
| `benchmarks/` workloads | 6 shootout `.fl` (`fib`, `fib2`, `pi`, `primes`, `strrev`, `sum_array`) + 13 `test_*` probes | 229 | loop above |
| `benchmarks/` mirrors | same 6 workloads in C, C++, Python | 275 | `clang -O2` / `g++` / `python3` |
| `tutorial/` lessons | 8 (`01_hello`–`08_wrap`), each with `EXPECT` checks | 198 | `bash tests/run_tutorial.sh ./flintc` |
| `stage1/` Flint lexer | `flint_lex.fl` + edge corpora, byte-identical to `--dump-tokens` | — | `bash tests/test_lexdiff.sh ./flintc` |
| `agent-bench/` tasks | 5 tasks, `reference.fl` each (checker uses your `solution.fl`) | 69 | `bash agent-bench/check.sh ./flintc` |

```bash
# Unit-style: run test_* functions (default: tests/*.fl + ./test_*.fl)
flintc test
flintc test foo.fl --filter parse

# All examples (each must exit 0)
for f in examples/*.fl; do echo "=== $f ===" && timeout 30 ./flintc "$f" 2>&1 | head -5; done

# Specific file
./flintc examples/basic.fl
```

## Compiler Flags

From `flintc --help` (v0.22.0):

```
--opt-level 0|1|2|3   LLVM optimization level (default: 2)
--fast                Quick JIT tier (fast iteration; EXPERIMENTAL — miscompiles
                    some heap-string programs, see memory.md failure audit)
--unsafe / --safe     Skip / keep overflow + bounds checks (default: safe)
--backend llvm|qbe    Codegen backend (default: llvm)
--target <triple>     Cross-compile target (e.g. wasm32-unknown-unknown)
--cgu N               Compiler worker threads for big files (Slipstream)
--parallel N          Parallel import scanning (N threads)
--link <flags>        Extra linker flags / objects
--offline             Never touch the network (registry)
--no-strip            Keep symbols in AOT binaries
--lib-path <dir>      Extra library search path
--emit-llvm           Output LLVM .ll text
--emit-obj            Output .o object file
--emit-interface      Emit .flint.bc declaration file
--use-interface       Use .flint.bc for declarations
--emit-header <f.h>   C prototypes for every fn (call Flint from C)
--test / --run        Test mode / JIT-run mode (also subcommands)
-o <path>             Output executable path (AOT, needs `build`)
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
| Unreleased | 2026-09-24 | V1 gate 17/17 (self-host A/B/C+stable, ladder 21/21, tutorial 8/8, registry, driver); exact i64 literals; lambda captures; AEGIS/chan runtime wiring; `flint.toml` fetch/cache/offline; pushed to `mukesh-craft/Flint` |
| 0.22.0 | 2026-09-06 | Concurrency: channels, parallel-for fix, --emit-header, WASM objects |
| 0.21.0 | 2026-09-06 | Tutorial track (8 runnable lessons) + match-value/print-f64 fixes |
| 0.20.0 | 2026-09-06 | Windows port: Winsock, MinGW-verified runtime, cross-linked hello.exe |
| 0.19.0 | 2026-09-06 | Tooling: run/build/test/fmt/doc/lsp/new/version/help, --filter |
| 0.18.0 | 2026-09-06 | Registry: flint.toml/lock, fetch, manifest imports + auto-fetch |
| 0.17.0 | 2026-09-06 | Any-device P1: host triple, --target, portable build, CI, smoke tests |
| 0.16.0 | 2026-09-06 | Zero-bug sweep: exact literals, brace escapes, sanitizers, JIT/AOT differential |
| 0.15.0 | 2026-09-06 | P2: map{...} literals + typed methods, error-recovery hardening |
| 0.14.0 | 2026-09-06 | P1 UX: no-main message, undef exit, \u escapes, continue everywhere |
| 0.13.0 | 2026-09-05 | P0 soundness: div-zero, annotations, error exits, match, bounds, lambdas |
| 0.12.0 | 2026-09-05 | Write less: &&/!/and/or/not, += etc., range() — zero-cost sugars |
| 0.11.0 | 2026-09-05 | Diet binaries (−77…−96%: sections + linker GC + strip) |
| 0.10.0 | 2026-09-05 | Slipstream: tiered + partitioned compiler speed (--fast, --cgu) |
| 0.9.0 | 2026-09-05 | Aegis leases: hybrid static+runtime memory safety |
| 0.8.0 | 2026-07-08 | If-expr codegen, float type inference, mixed i64/f64, Python tools |
| 0.7.0 | 2026-07-06 | Phase F: Flux Compilation — extreme performance |

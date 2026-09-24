# Flint Programming Language

A safe, small, fast compiled language for **every device** — phone, laptop,
server. Native binaries via LLVM, Rust-like ownership plus generational
leases, Python-style ergonomics, batteries-included tooling.

> Every command below was executed against this tree on Termux AArch64
> on 2026-09-24. Anything not verified is marked as such — see Status.

## Quick Start

```bash
# Install dependencies (Termux shown; Debian: apt install clang llvm python3
# make — see REQUIREMENTS.md for macOS/Windows)
pkg update && pkg install clang llvm python make bash

# Build the compiler
cd ~/Flint
bash build.sh

# Run a Flint program (JIT mode — one command)
./flintc examples/hello.fl

# Compile to standalone executable
./flintc examples/hello.fl -o hello
./hello
```

New here? Work through `tutorial/01_hello.fl` … `tutorial/08_wrap.fl`
(`bash tests/run_tutorial.sh ./flintc` checks your answers — 8/8 green).

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
- **Known crash:** `examples/enums.fl` — `match` arms written as
  `{ ... }` blocks abort the compiler (LLVM PHI type assertion).
  Single-expression arms (as in the Syntax section above and
  `tutorial/03_flow.fl`) work; block arms do not yet
- Next work is ordered in **[ROADMAP.md](ROADMAP.md) Phase J** (post-V1
  plan: lock-in + P0 crash → measure + safety wins → bigger bets)

## Documentation

- **[REQUIREMENTS.md](REQUIREMENTS.md)** — Full setup and build requirements
- **[ROADMAP.md](ROADMAP.md)** — Phase-by-phase feature roadmap
- **[memory.md](memory.md)** — Project memory for agent-assisted development

## Features (all verified below)

- **Compiled to native code** via LLVM IR — JIT run, AOT binary, or `.ll`
  text; default target is the host (Termux: `aarch64-unknown-linux-android24`)
- **Immutable by default** — variables are immutable unless declared with `mut`
- **Memory safe** — ownership + borrow checker, generational Aegis leases,
  no null keyword, bounds traps, overflow panics (all loud, never silent)
- **Small-ish binaries** — hello world AOT is ~141 KB (~111 KB stripped);
  shrinking this is tracked work, not a current property
- **Faster builds** — parallel import scanning (`--parallel`), `--fast`
  iteration tier, content-addressed module cache
- **C FFI** — call any C function via `extern "C"` plus `--link` objects
  (verified: `examples/ffi_demo.fl --link "ffi_helper.o"`)
- **Python embedding** — `python { }` blocks and `py_eval()` work in
  **AOT builds** (`-o`); JIT mode cannot resolve Python symbols
- **Generics** — monomorphized, zero runtime cost (verified:
  `examples/generics.fl`)
- **Enums + pattern matching** — exhaustive, value-yielding `match`
- **Maps, methods, lambdas** — `map{"k": v}`, `s.upper()`, `|x| x + 1`
  closures with by-value captures
- **Threads + channels** — `flint_thread_create(&fn, arg)` /
  `flint_thread_join`, `flint_chan_new/send/recv/free` (verified below).
  `parallel for` bodies currently fail to compile; `chan`-typed
  annotations and method-call syntax (`.send()`) do not exist
- **Packages** — `flint.toml` next to your program, `import "name`,
  auto-fetch on first build into `~/.cache/flint_pkgs`, `flint.lock`
  pins revisions, `--offline` uses cache only (verified: registry gate)
- **Developer tools** — `python3 flint-fmt` (format/`--check`),
  `python3 flint-doc` (docs), `python3 flint-lsp` (basic LSP);
  direct `./flint-*` execution fails on Termux (shebang), always
  invoke via `python3`

## Syntax

The program below was executed verbatim (`rc=0`) to verify this section:

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
# flintc --offline ...             # never touch the network

## Build

```bash
bash build.sh
```

This compiles (all verified present after a clean build):
- `src/main.cpp` → `./flintc` (compiler)
- `runtime/runtime.c` → `./runtime.o`
- Optional: `pyruntime.c` → `./pyruntime.o`, `ffi_helper.c` → `./ffi_helper.o`
- Stdlib: `flint_tensor.o`, `flint_ai.o`, `flint_ai_opt.o`,
  `flint_serial.o`, `flint_crypto.o`, `flint_net.o`,
  `flint_aegis.o`, `flint_chan.o`

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
(Note: `-o` output may need `chmod +x` on some systems before running.)

### AOT to LLVM IR
```bash
./flintc examples/hello.fl output.ll
clang output.ll runtime.o -o hello
./hello
```

## Tooling (verified flags only)

There is no `flintc --help` and no `flintc <subcommand>` — bare
`./flintc` with no usable input prints usage. The real interface is
flags plus standalone Python scripts:

```bash
./flintc main.fl -o myapp            # native binary (AOT)
./flintc main.fl out.ll              # LLVM IR text (by output extension)
./flintc --opt-level 0 main.fl       # 0|1|2|3, default 2
./flintc --fast main.fl              # accepted anywhere; currently a no-op tier flag
./flintc --unsafe main.fl            # skip overflow/bounds checks (--safe is default)
./flintc --backend llvm main.fl      # llvm (default) or qbe (experimental, slower)
./flintc --parallel 4 main.fl        # parallel import scanning
./flintc --lib-path DIR main.fl      # extra library search path
./flintc --link "ffi_helper.o" main.fl  # extra linker objects/flags
./flintc --offline main.fl           # registry: cache only, fail if missing
./flintc --emit-interface a.fl b.flint.bc  # declarations only
./flintc --use-interface b.flint.bc main.fl -o app  # build against them
./flintc --dump-tokens main.fl       # stable token dump (matches stage1)
python3 flint-fmt main.fl [--check]  # format (--check fails if unformatted)
python3 flint-doc main.fl            # docs to stdout
python3 flint-lsp                    # basic LSP server over stdio
```

Known tooling gaps (do not rely on these):
- `flintc run/build/test/fmt/doc/lsp/fetch/new/api/help` subcommands
  do not exist; `flintc --help` does not exist.
- `--test` exists as a flag but does not run tests (silently builds on
  cache hit, `JIT creation failed` on miss).
- `--target`, `--cgu`, `--no-strip`, `--emit-llvm`, `--emit-obj`,
  `--emit-header` do not exist.

## Learn

```bash
bash tests/run_tutorial.sh ./flintc  # 8 runnable lessons in tutorial/ (8/8 green)
bash agent-bench/check.sh ./flintc    # 5 tasks; add solution.fl per task first
                                      # (checker SKIPs tasks without one)
```

There is no `flintc api` command and no `llms.txt`; the builtin
reference today is `docs/errors.md` (116 codes) plus the tutorial.

## Concurrency

The program below was executed verbatim (`42`, `42`, `rc=0`):

```flint
fn worker(a: i64) -> i64 {
    print(a + 1)
    0
}

fn main() -> i64 {
    t = flint_thread_create(&worker, 41)
    flint_thread_join(t)
    ch = flint_chan_new(4)
    flint_chan_send(ch, 42)
    print(flint_chan_recv(ch))
    flint_chan_free(ch)
    0
}
```

Rules: values crossing threads are copied (i64) or passed by handle;
join threads before freeing what they touch. Not working today:
`parallel for` bodies (`undefined var '__pfor_0'`), `chan`-typed
variable annotations, and channel method-call syntax — use the plain
`flint_chan_*` functions above. Aegis leases stay memory-safe across
threads (generational checks) but do NOT prevent data races — use
channels for handoff.

## Libraries and WASM (not available)

There is no `--emit-header` (C cannot currently consume Flint
prototypes) and no `--target` (no cross-compilation, no WASM output —
the default target is always the host triple). These are tracked future
work, not current features.

## Benchmarks

Run the full benchmark suite (19 `.fl` files: 6 shootout workloads with
C/C++/Python mirrors + 13 `test_*` if/call micro-probes):
```bash
for f in benchmarks/*.fl; do echo "=== $f ===" && timeout 60 ./flintc "$f" 2>&1 | head -5; done
```

Historical snapshots below are labeled with their session — numbers move
with device thermal state (today: little cores at 691 MHz, big at
2.2 GHz; `pi` reproduced exactly, `sum_array` did not — see note).

Measured 2026-09-06, Termux AArch64, JIT default (in-program timers,
`clang -O2` for the C column):

| Benchmark | Flint | C (AArch64) | Ratio |
|-----------|-------|-------------|-------|
| sum_array (10M) | ~17 ms (~15 ms `--unsafe`) | ~8 ms | ~2.1× |
| primes (10M) | ~459 ms | ~202 ms | ~2.3× |
| fib(45) | ~11.7 s | ~8.6 s | ~1.4× |
| pi (100M iters) | ~714 ms | ~718 ms | ~1.0× |

Binary size: hello world AOT is ~141 KB (~111 KB stripped) as produced
by the documented `./flintc examples/hello.fl -o hello` command
(measured 2026-09-24). Smaller binaries are tracked work, not current.

Measured 2026-09-12, Termux AArch64, same session (steady-state runs;
in-program timers for runtime, wall clock for compile):

### Runtime — large workloads (Flint JIT vs C/C++ `-O2` vs Python)

| Benchmark | Flint default | Flint `--unsafe` | C | C++ | Python |
|-----------|---------------|------------------|---|-----|--------|
| sum_array 10M | 19.3 ms | 16.4 ms | 9.2 ms | 9.8 ms | 175.7 ms |
| pi 100M iters | 777.5 ms | n/m | 782.8 ms | 778.8 ms | >60 s (timeout) |
| primes 10M | 542.9 ms | 481.4 ms | 199.1 ms | 203.9 ms | 2,092 ms |
| fib(45) | 12,735 ms | 8,483 ms (beats C ~9%) | 9,328 ms | 9,333 ms | skipped (est. 10+ min) |
| strrev (100K builder) | ~1.0 ms | ~0.95 ms | 5.5 ms | 32.0 ms | 444.9 ms |

`--unsafe` skips overflow + bounds checks: faster, but drops the safety
that justifies Flint vs C. Failures and skips reported as-is, not hidden.

Note (2026-09-24): the `strrev` Flint cells were re-measured (4 stable
runs, checksum 5044012 each, exit 0) — the old CRASH is gone since the
R5 builder rewrite. Workloads differ by column: Flint builds + checksums
a 100K digit-string while the C/C++/Python mirrors reverse 10M chars,
so cross-column ratios on this row are rough, not exact. C cell
re-verified today at ~5.5 ms. Spot-checks today: `pi` reproduced
(~780 ms vs 777.5 ms); `sum_array` did not (~90 ms vs 19.3 ms, both JIT
and AOT agree with each other — scheduler/thermal variance, under
investigation, not presented as a new number).

### Compile speed — large files (wall clock; `.ll` output unless noted)

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
`.ll` output stops before backend/link while `-c` emits full objects;
Termux-class devices throttle across sessions — compare same-session
medians (see note above: `sum_array` varied ~5x between sessions while
`pi` reproduced, so treat single-session numbers as approximate).

### Slipstream trade-off

`--fast` is currently accepted as a no-op tier flag (kept for CLI
compatibility while tiers are reworked): it does not change codegen
today, which is why every opt level agrees in `test_opt_identity.sh`.
Historical `--fast` measurements below predate that change.

Historical (pre-no-op `--fast`, kept for reference): `--fast` compiled
faster (2.2x at 48 KB → ~4.5x at 112 KB) but emitted larger,
less-optimized code: `flint_parse.fl` IR was 828 KB vs 628 KB default
(+32%), and `sum_array` ran 42.5 ms vs 19.3 ms (2.3x slower).

## Testing

What exists and how to run it (file lists and line counts verified
2026-09-24 with `wc -l`):

| Suite | Files | Lines | Run |
|-------|-------|-------|-----|
| `tests/` smoke tests | 5 `.fl` (`t_hello` 8, `t_arith` 14, `t_flow` 43, `t_funcs` 15, `t_types` 23) | 103 | `bash tests/run.sh ./flintc` |
| `tests/` runners | `run.sh` 30, `run_tutorial.sh` 18, `test_registry.sh` 43 | 91 | `bash tests/test_registry.sh` |
| `benchmarks/` workloads | 6 shootout `.fl` (`fib` 15, `fib2` 11, `pi` 23, `primes` 33, `strrev` 34, `sum_array` 31) + 13 `test_*` probes (86) | 233 | loop above |
| `benchmarks/` mirrors | same 6 workloads in C, C++, Python | 275 | `clang -O2` / `g++` / `python3` |
| `tutorial/` lessons | 8 (`01_hello`–`08_wrap`), each with `EXPECT` checks | 198 | `bash tests/run_tutorial.sh ./flintc` |
| `stage1/` Flint lexer | `flint_lex.fl` + edge corpora, byte-identical to `--dump-tokens` | — | `bash tests/test_lexdiff.sh ./flintc` |
| `agent-bench/` tasks | 5 tasks (`TASK.md` + `expected.txt` + `reference.fl` each, 136 lines) + `check.sh` 24 | 160 | `bash agent-bench/check.sh ./flintc` |

```bash
# Sweep every example; exit code is main's return value, so nonzero is
# EXPECTED for demos that prove loud failure (panics, compile errors):
#   aegis_* (use-after-free/double-free panics), borrow_error (compile
#   error), overflow (overflow panic), unwrap_panic (unwrap panic),
#   try_demo/unwrap_demo (return 42), ffi_printf (returns printf's 20).
# python_demo needs AOT (-o): JIT cannot link Python symbols.
# enums.fl currently aborts the compiler (match block arms — see Status).
for f in examples/*.fl; do echo "=== $f ===" && timeout 30 ./flintc "$f" 2>&1 | head -5; done

# FFI demo needs its helper object; Python demo needs an AOT build:
./flintc examples/ffi_demo.fl --link "ffi_helper.o"
./flintc examples/python_demo.fl -o pydemo && ./pydemo
```

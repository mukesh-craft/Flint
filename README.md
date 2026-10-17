# Flint Programming Language

A safe, small, fast compiled language for **every device** — phone, laptop,
server. Native binaries via LLVM, Rust-like ownership plus generational
leases, Python-style ergonomics, batteries-included tooling.

**License:** Apache-2.0 — see [LICENSE](LICENSE).

> Every command below was executed against this tree on Termux AArch64
> on 2026-10-10. Anything not verified is marked as such — see Status.

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

## Status (2026-10-10, verified on Termux AArch64)

- **V1 gate 12/12 green** (`bash tests/v1_gate_check.sh ./flintc`):
  self-host bootstrap A/B/C + stable promotion (fixed point
  byte-identical), stage-3 ladder 21/21, tutorial 8/8, parse-gate 104,
  sanitizers 10, opt-identity 11, differential, driver, registry, aegis,
  stdlib — see `memory.md`
- **Standard library + AI gate 17/17 green**
  (`bash tests/test_ai.sh ./flintc`): BLAS correctness on both paths,
  gradient check (f64 + f32 vs finite differences), TOML, YAML, tensor,
  autograd, optimizers, per-rule gradients, a trained 2-layer net, XOR, NN
  layers, f32, the utility layer, formatting, and the standard library
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
  *and the reverse*: a Flint module with no `main` compiles to an object
  exporting unmangled functions, so C or Rust can link Flint and call it
  directly (gate: `tests/test_rust_ffi.sh`, both directions).
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
  `python3 flint-doc` (docs), `flint-lsp` (Rust language server);
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

## Machine learning

Tensors, a tape-based autograd, optimizers and layers, in f64 **or f32**.
All reachable from Flint. Verified by `bash tests/test_ai.sh ./flintc`.

### Tensors — `flt_*`

`flt_create` / `flt_create_1d` / `_2d` / `_3d` / `_4d` / `_5d` ·
`flt_zeros_like` `flt_clone` `flt_destroy` · `flt_size` `flt_ndim` `flt_shape`
`flt_get` `flt_set` `flt_fill` `flt_copy` `flt_set_all` `flt_get_at`
`flt_set_at` `flt_data` `flt_randn` `flt_uniform`.

```flint
a = flt_create_2d(2, 3)          # f64
b = flt_create_f32_2d(2, 3)       # f32: half the memory, ~2x the matmul
print(flt_bytes(a))              # 48
print(flt_bytes(b))              # 24
```

Elementwise `flt_neg exp log sqrt pow relu sigmoid tanh` ·
`flt_add_tt sub_tt mul_tt div_tt` · `flt_matmul` `flt_matmul_blas` ·
`flt_sum` `flt_mean` `flt_reshape` `flt_transpose` · in-place
`flt_relu_inplace` `flt_sigmoid_inplace` `flt_tanh_inplace`.

> `flt_matmul_blas` is the **deterministic reference** (portable loop, never
> BLAS). It is what the gate compares the live BLAS path against, and it returns
> the same answer with no floating-point variance. `flt_matmul` is the fast one.

> `flint_blas_available()` and `flint_blas_backend()` report which path is live.
> Both AOT and JIT use OpenBLAS when it is installed. (JIT did not, for a
> while: `flint_blas.o` reaches `cblas_dgemm` through a weak symbol, and the JIT
> never loaded the library, so every matmul silently took the portable loop —
> correct answers, ~11x slower. Back then, `flint_blas_available()` returning 0
> in JIT mode was the tell; it now returns 1 with OpenBLAS installed.)

### f32

`flt_to_f32` / `flt_to_f64` / `flt_is_f32` convert. A tensor is one of the two
widths; `flt_data` returns NULL for an f32 tensor so a `double*` cannot be
forced onto f32 storage (`flt_data_f32` is the f32 accessor).

Measured on this device, N=500: **f64 41.7 ms → f32 22.3 ms (1.86×)**, and
exactly half the footprint. f32 **computes in double and narrows on store**, so
`exp`/`tanh`/`sigmoid` keep double precision in the last place it matters.
Mixed-width binary ops promote to f64.

### Autograd — `ag_*`

`ag_init` `ag_cleanup` `ag_track_tensor` `ag_grad_id` `ag_grad_at`
`ag_grad_len` `ag_zero_grad` · `ag_matmul` `ag_add` `ag_sub` `ag_mul` `ag_sum`
`ag_mean` `ag_relu` `ag_sigmoid` `ag_tanh` `ag_exp` `ag_log` `ag_neg` ·
`ag_backward` `ag_seed` `ag_seed_scalar`.

```flint
ag_init()
x = flt_create_1d(3)
xid = ag_track_tensor(x)
a = ag_mul(x, x)
ag_seed_scalar(ag_grad_id(a), 1.0)
ag_backward(ag_grad_id(a))
print(ag_grad_at(xid, 0))    # 0 (d(x*x)/dx at 0)
```

> Two things to know. `ag_grad_id(a)` names the node the op recorded —
> re-tracking with `ag_track_tensor` allocates a *second* id and orphans the
> node, so `ag_backward` finds nothing. And a loss gradient that is not itself
> on the tape must be attached with `ag_seed`, or `ag_backward` begins at ones
> and trains a fabricated objective.

### Layers — `flint_nn.c`

`flt_conv2d` (NCHW, stride, padding, optional bias) · `flt_maxpool2d`
`flt_avgpool2d` `flt_global_avgpool` · `flt_batchnorm` `flt_layer_norm`
`flt_rms_norm` · `flt_softmax` `flt_dropout` `flt_l2_reg` `flt_flatten` ·
`flt_xavier_uniform` `flt_he_uniform`.

### Optimizers and loss

`adam_create` `_add_param` `_step` `_destroy` · `sgd_create` (with momentum)
`_add_param` `_step` `_destroy` · `mse_loss` `mse_loss_grad`.

### Verified models

- `tests/nn_two_layer.fl` — trains on `f(x) = 2x+1`; predicts exactly
  **3, 5, 7, 9** (an exact fit, so the run is checkable by hand rather than
  "the loss went down")
- `tests/xor_mlp.fl` — **XOR solved**, loss 0.148 → 1e-25. The standard check
  that a network is more than a perceptron
- `tests/gradcheck.c` — analytic gradients vs **central finite differences**,
  for f64 and f32. This is what proves the tape is right: a gradient can pass
  every type and shape check and still be numerically wrong. It found a 2× error
  in mul, a 4× double-count from processing a repeated input twice, and a seed
  that was silently discarded.

> Not present: model serialisation. Saving weights means writing the tensor
> yourself.

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

`./flintc --help` lists every flag; `./flintc --version` prints the
version. There are no `flintc <subcommand>` verbs — the interface is
flags plus standalone Python scripts:

```bash
./flintc --help                       # every flag, grouped
./flintc --version                    # flintc 0.22.0
./flintc main.fl -o myapp            # native binary (AOT)
./flintc main.fl out.ll              # LLVM IR text (by output extension)
./flintc main.fl out.o               # object file (by output extension)
./flintc --test main.fl              # run every test_ function, prints a summary
./flintc --opt-level 0 main.fl       # 0|1|2|3, default 2
./flintc --fast main.fl              # accepted anywhere; currently a no-op tier flag
./flintc --unsafe main.fl            # skip overflow/bounds checks (--safe is default)
./flintc --backend llvm main.fl      # llvm (default) or qbe (experimental, slower)
./flintc --parallel 4 main.fl        # parallel import scanning
./flintc --lib-path DIR main.fl      # extra library search path
./flintc --link "ffi_helper.o" main.fl  # extra linker objects/flags
./flintc --offline main.fl           # registry: cache only, fail if missing
./flintc --check-bce main.fl         # diagnostic: safety checks emitted
                                     # vs surviving O2 (changes nothing)
./flintc --emit-interface a.fl b.flint.bc  # declarations only
./flintc --use-interface b.flint.bc main.fl -o app  # build against them
./flintc --target x86_64-pc-linux-gnu main.fl -o main.x86.ll
                                     # cross-target emit (.ll text or .o
                                     # object; JIT execution stays host-only)
./flintc --debug main.fl -o prog     # DWARF line tables in the artifact
                                     # (llvm-dwarfdump/lldb); `--debug` needs
                                     # -o, bare run refuses (JIT can't carry
                                     # debug metadata)
./flintc --dump-tokens main.fl       # stable token dump (matches stage1)
python3 flint-fmt main.fl [--check]  # format (--check fails if unformatted)
python3 flint-doc main.fl            # docs to stdout
lsp/target/release/flint-lsp         # Rust language server (stdio):
                                     # diagnostics, completion, hover,
                                     # definition, references, outline
./flintc_prof main.fl -o app         # profiled build, writes profile_report.json
python3 tools/flamegraph.py profile_report.json flame.svg  # tree SVG view
python3 tools/flamegraph.py profile_report.json --folded   # folded stacks
```

Known tooling gaps (do not rely on these):
- `flintc run/build/test/fmt/doc/lsp/fetch/new/api` subcommands do not
  exist — use the flags above plus `flint-fmt`/`flint-doc`/`flint-lsp`.
- `--cgu`, `--no-strip`, `--emit-obj`, `--emit-header`, `--emit-llvm` are
  not flags: the **output extension** picks the artifact form
  (`.ll` = IR text, `.o` = object, other = linked executable), so
  `--emit-llvm` in older docs is wrong — drop it.
- `flint-lsp` covers diagnostics, completion, hover, definition, references
  and outline; it has no semantic tokens, no formatting request, and refuses
  `rename` with `-32601` rather than half-doing it. VS Code users install the
  extension in `editors/vscode/` instead of wiring the binary by hand.

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

## Standard library

18 runtime modules, ~12,900 lines of C, all reachable from Flint as `flint_*`
builtins. Verified by `bash tests/test_ai.sh ./flintc` (17/17) and the
`stdlib` gate in `tests/v1_gate_check.sh`.

### Maths and statistics — `flint_math.c`

`sin cos tan asin acos atan atan2 sinh cosh tanh sqrt exp log log10 pow`
`cbrt expm1 log1p log2 hypot copysign` · `fmod trunc signum ldexp frexp`
`ulp nextafter` · `isfinite isinf isnan isclose` · `erf erfc gamma lgamma` ·
constants `pi e tau inf nan`.

```flint
print(flint_factorial(10))      # 3628800
print(flint_gcd_i64(12, 18))     # 6
print(flint_degrees(flint_pi()))  # 180
print(flint_gamma(5.0))         # 24
```

Descriptive statistics take a `vec` of i64 and return a scalar:
`mean median min max mode percentile` · `variance pvariance stdev pstdev`
`geometric_mean harmonic_mean skewness kurtosis covariance correlation`
`linear_regression`. `flint_accum_*` is a Welford streaming accumulator for
statistics that arrive one value at a time.

### Formatting — `flint_fmt.c`

`flint_format` is printf-style: flags `- + space 0`, width, precision, `*`,
and `%d %i %u %f %F %e %E %g %G %s %c %p %%`.

```flint
r = flint_formatd2("n=%d x=%.2f", 3.5, 42)
print(r)   # n=42 x=3.50
flint_str_free(r)
```

> **Note:** an LLVM variadic function needs `isVarArg` plus every call site
> lowered to the platform's variadic ABI, which the compiler does not do. Rather
> than register a fixed `(char*)` signature and read the arguments after the
> format string as **garbage**, the compiler exposes fixed-arity shims
> (`flint_format1..4`, `flint_formatd1..4`, `flint_formats1..3`) whose
> signatures the verifier checks.

### Collections — `flint_coll.c` (new) and `runtime.c`

Existing: `vec` (dynamic array), `sb` (string builder), `map`, `chan`.

Added: `flint_sort_i64` / `flint_sort_desc_i64` (quicksort with median-of-three,
depth-limited, heapsort fallback) · `flint_sort_i64_stable` (merge sort) ·
`flint_bsearch_i64` · `flint_set_*` (open-addressed hash set) ·
`flint_heap_*` (binary min-heap / priority queue) · `flint_deque_*` (ring
buffer, O(1) both ends).

### Synchronisation and randomness — `flint_util.c`

`flint_mutex_*` (plain and recursive) · `flint_cond_*` · `flint_rwlock_*` ·
`flint_atomic_*` (get/set/add/sub/CAS on a heap cell, so a counter cannot be
copied by value and lose the lock) · `flint_rng_*` (xoshiro256\*\*, seeded from
`/dev/urandom`).

> `flint_rand_f64` is a plain LCG: fine for jitter, **wrong for keys, tokens
> and nonces**. `flint_rng_below` rejection-samples, because `u64 % n` biases
> toward low values.

### Command-line parsing — `flint_util.c`

`flint_cli_new` / `_flag` / `_opt` / `_parse` / `_get_str` / `_get_i64` /
`_has` / `_positional` / `_usage`. Supports `--name=v` and `--name v`, bundled
short flags, and `--`. An **unknown option is an error**, not a silent default —
a typo must not become a default.

```flint
cli: ptr = flint_cli_new("prog")
flint_cli_flag(cli, "verbose", 118, 0)     # 'v'
flint_cli_opt(cli, "output", 111, 0)       # 'o'
flint_cli_parse(cli, flint_arg_count(), flint_get_argv())
print(flint_cli_get_str(cli, "output"))
```

### Config formats — `flint_value.c`, `flint_toml.c`, `flint_yaml.c`

One dynamic-value model (`flint_value_*`) shared by every format, so TOML and
JSON are walked with the same code: `flint_value_obj_get`, `flint_value_path`
(`"server.http.port"`), typed accessors that return a default on a type
mismatch rather than coercing, `flint_value_encode`, `flint_value_clone`,
`flint_value_free`.

`flint_toml_parse` — TOML v1.0.0: tables, arrays of tables, dotted keys, inline
tables, all four integer forms with underscores, floats with `inf`/`nan`,
basic/literal/multi-line strings with line-continuation folding, and the four
date/time forms normalised to RFC 3339.

`flint_yaml_parse` — block mappings and sequences (including a sequence at the
same indent as its key), flow collections, literal `|` and folded `>` block
scalars with chomping, plain/quoted scalars, comments, document marker. Tags,
directives, anchors and merge keys are **rejected with a line and column**
rather than silently mis-parsed.

> Keys written after a `[[table]]` header belong to that table until the next
> header. That is TOML's rule, and it surprises people.

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
| matmul 500² (10 iters, AOT) | 42.6 ms | n/m | 428.4 ms (naive) | n/m | 42.7 ms (numpy 2.4.4) |

`--unsafe` skips overflow + bounds checks: faster, but drops the safety
that justifies Flint vs C. Failures and skips reported as-is, not hidden.

Note (2026-10-10): the `matmul` row is new — same-session, AOT Flint
(OpenBLAS) vs naive C vs numpy 2.4.4 (`benchmarks/matmul_np.py`;
`benchmarks/matmul.py` stays pure-Python and takes ~60 s at 500²).
Flint ≈ numpy to within noise (both OpenBLAS-backed; value checksums
agree: 4799.09 vs 4799.0857 at 8²), ~10× the naive C triple loop.

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
| `tests/` smoke tests | 5 `.fl` (`t_hello` 8, `t_arith` 26, `t_flow` 48, `t_funcs` 15, `t_types` 23) | 120 | `bash tests/run.sh ./flintc` |
| `tests/` runners | `run.sh` 63, `run_tutorial.sh` 18, `test_registry.sh` 43 | 124 | `bash tests/test_registry.sh` |
| `benchmarks/` workloads | 6 shootout `.fl` (`fib` 15, `fib2` 11, `pi` 23, `primes` 33, `strrev` 34, `sum_array` 31) + 13 `test_*` probes (86) | 233 | loop above |
| `benchmarks/` mirrors | 7 workloads in C, C++, Python (+numpy matmul mirror) + 2 Rust oracle mirrors | 386 | `clang -O2` / `g++` / `python3` / `rustc` |
| `tests/rust/` FFI + oracle | 4 Rust sources, 2 Flint FFI programs; both ABI directions + 2 result oracles | — | `bash tests/test_rust_ffi.sh ./flintc` · `bash tests/test_rust_oracle.sh ./flintc` |
| `lsp/` language server | Rust, `serde_json` only; 21 unit tests + 31 protocol checks + keyword/builtin drift gates | — | `bash tests/test_lsp.sh ./flintc` |
| `editors/vscode/` extension | manifest, TextMate grammar (26 patterns), language config, stdio client | — | `python3 tools/check_vscode_extension.py` |
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

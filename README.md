# Flint Programming Language

A safe, small, fast compiled language for **every device** — phone, laptop,
server. Native binaries via LLVM, Rust-like ownership plus generational
leases, Python-style ergonomics, batteries-included tooling.

**License:** Apache-2.0 — see [LICENSE](LICENSE).

> The Quick Start and Tooling commands were executed against this tree on
> Termux AArch64 on 2026-10-25 (the dependency-install line was already
> satisfied on this device). Benchmark rows keep their own session dates.
> Anything not verified is marked as such — see Status.

## Quick Start

```bash
# Install dependencies (Termux shown; Debian: apt install clang llvm python3
# make — see REQUIREMENTS.md for macOS/Windows)
pkg update && pkg install clang llvm python make bash

# Build the compiler
cd ~/Flint
bash build.sh

# Compile and run in one command (ORC JIT; no artifact on disk)
./flintc examples/hello.fl

# Build, run, and keep a standalone executable
./flintc examples/hello.fl -o hello

# Build it without running (the explicit artifact-only path)
./flintc examples/hello.fl -o hello --no-run
./hello
```

A bare `flintc file.fl` is the fast one-command path: it compiles the
current source and runs it in memory. `-o` compiles, links, and then runs
the native executable, leaving that exact file on disk. `--no-run` keeps
the artifact without executing it; `.ll`/`.o` outputs are emit-only, and
`--target` never runs a cross-target artifact.

New here? Work through `tutorial/01_hello.fl` … `tutorial/08_wrap.fl`
(`bash tests/run_tutorial.sh ./flintc` checks your answers — 8/8 green).

## Status (2026-10-27, verified on Termux AArch64)

- **Working end to end** — compile, JIT, LLVM IR, interface files,
  content-addressed cache, generics (monomorphization), traits, closures,
  enums + `match`, `for x in <array>`, the scoped `bytes` type, native
  string methods, Rust-style unused-variable warnings, C FFI, Python
  embedding, LSP, formatter, and the standard-library/AI stack. The v1
  battery and the full gate suite are green; the counts and the per-gate
  table live in [`STATE.md`](STATE.md) §2.2 (one home for status counts).
- **Self-hosted** — the lexer/parser/emitter are Flint programs
  (`stage1-3/*.fl`); the C++ `flintc` bootstraps them and the bootstrap
  fixed point is byte-identical. The pipeline compiles generics and traits,
  consumes the C++ compiler's resolved signatures
  (`flintc --emit-signatures`), and implements the explicit `::<T>` call
  syntax (2E; gate: `tests/test_explicit_targs.sh`, differential
  `tests/differential/t_explicit_targs.fl`). See
  [`COMPATIBILITY.md`](COMPATIBILITY.md) D5 and
  [`docs/PIPELINE-GENERICS.md`](docs/PIPELINE-GENERICS.md).
- **Closures that escape** — a capturing closure may be returned, stored in
  a global, or passed to a callee that retains it. Its environment box is
  promoted to the heap at the escape site; a closure that stays inside its
  defining frame keeps a stack box and allocates nothing (Phase 2 / 2D;
  gate: `tests/test_closure_escape.sh`, which also checks the emitted IR).
- **Type checking (Phase 2A, landed)** — a compiler phase rejects provable
  type errors with structured `E0290`–`E0294` diagnostics before codegen,
  may never reject a program the compiler accepts (181/181 differential over
  every `.fl` in the tree), and `--type-audit` reports **0 contract
  violations / 0 drift** plus the call facts (instantiated type arguments,
  resolved trait impls). `--no-type-check` is the differential oracle. The
  remaining Phase 2 items (typed HIR, full regions, capability types) are in
  [`STATE.md`](STATE.md) §4.2.
- **Bounded line input** — `flint_input`, `flint_input_i64` and
  `flint_input_f64` consume exactly one line, cap it at 1 MiB, drain any
  excess, strip CRLF, and report EOF / too-long / invalid / out-of-range /
  embedded-NUL through `flint_input_status()` instead of guessing. Gate:
  `tests/test_input.sh` (8/8). See **Reading input** below.
- **Honest gaps** — escaped closure boxes are allocated with `flint_malloc`
  and are **not freed** (there is no closure destructor yet); `[T]`
  containers (M7) are deferred by decision; `--target` emits cross-target
  `.ll`/`.o` but cannot link or run a cross-target executable; Windows and
  macOS are not executed from this device; and the self-host pipeline is
  complete for the bootstrap, **not feature parity** with the authoritative
  C++ compiler: the function table holds one entry per method name (two
  trait impls of the same name cannot coexist), lambda-as-a-value parsing is
  partial, enum equality is absent, and a named function cannot yet be used
  as a value. The full table is [`COMPATIBILITY.md`](COMPATIBILITY.md) D5.

## Project provenance and AI assistance

Flint is maintained by **Mukesh** as a solo project; the repository history
starts on 2026-07-02. AI coding assistants, including **OpenCode**, are used
for implementation drafts, test scaffolding, and documentation drafts. The
architecture, the phase order, release decisions, and the final review are
the maintainer's, recorded in [`STATE.md`](STATE.md),
[`docs/LANGUAGE-ROADMAP.md`](docs/LANGUAGE-ROADMAP.md), and
[`memory.md`](memory.md). The full disclosure is in [`CREDITS.md`](CREDITS.md);
the design philosophy, non-goals, and known trade-offs are in
[`docs/DESIGN-PHILOSOPHY.md`](docs/DESIGN-PHILOSOPHY.md).

The project treats a documentation claim like a test: if no command in this
tree produces the result, the claim is wrong. The status counts are checked
by `tools/docs_check.sh`, the diagnostic counts by the two catalog gates,
and every benchmark row carries its session date. If you find a claim that
is not reproducible, please open an issue with the file and the claim.

## Documentation

- **[STATE.md](STATE.md)** — the live state, the queue, the gates, the traps
- **[docs/INDEX.md](docs/INDEX.md)** — which document owns which fact
- **[docs/DESIGN-PHILOSOPHY.md](docs/DESIGN-PHILOSOPHY.md)** — the why, the non-goals, the trade-offs
- **[REQUIREMENTS.md](REQUIREMENTS.md)** — setup and build requirements
- **[ROADMAP.md](ROADMAP.md)** — the historical phase log (not the live queue)
- **[docs/LANGUAGE-ROADMAP.md](docs/LANGUAGE-ROADMAP.md)** — research assessment and forward phases
- **[benchmarks/README.md](benchmarks/README.md)** — device, method, and reproduction commands
- **[CREDITS.md](CREDITS.md)** — maintainer, AI-assistance disclosure, third-party components
- **[memory.md](memory.md)** — the project ledger for agent-assisted development

## Features (all verified below)

- **Compiled to native code** via LLVM IR — JIT run, AOT binary, or `.ll`
  text; default target is the host (Termux: `aarch64-unknown-linux-android24`)
- **Immutable by default** — variables are immutable unless declared with `mut`
- **Memory safety mechanisms** — ownership + borrow checker, generational
  Aegis leases, no null keyword, bounds traps, overflow panics (all loud,
  never silent). These checks cover the safe-code model the compiler
  implements; this is not a proof that every path is memory-safe. Escaped
  closure environments are not freed yet, Aegis leases do not establish
  static data-race freedom, full region inference and capability types are
  future work, and C FFI plus `--unsafe` sit outside the checked model. See
  **Honest gaps** below and [`docs/DESIGN-PHILOSOPHY.md`](docs/DESIGN-PHILOSOPHY.md).
- **Small binaries** — hello-world AOT is **8,048 B raw / 4,160 B
  stripped** (re-measured 2026-10-25; `strip` from the Termux toolchain).
  `libc` only; the link fix that removed 743,520 B is `STATE.md` §6.1.
  `--print-size` attributes a freshly linked artifact.
- **Faster builds** — parallel import scanning (`--parallel`) and a
  content-addressed module cache that skips unchanged modules
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
- **Packages** — `flint.toml` next to your program, `import "name"`,
  auto-fetch on first build into `~/.cache/flint_pkgs`, `flint.lock`
  pins revisions, and `--offline` (a **flag**, not a subcommand) uses the
  cache only (verified: registry gate)
- **Developer tools** — `python3 flint-fmt` (format/`--check`),
  `python3 flint-doc` (docs), `flint-lsp` (Rust language server);
  direct `./flint-*` execution fails on Termux (shebang), always
  invoke via `python3`

## Strings: escapes and interpolation

Flint strings are `"..."` with C-style escapes **and** `{identifier}`
interpolation. The two interact, which is worth stating plainly because it is
the single most common surprise (it surprised this project twice while writing
the SQLite and FTS5 fixtures):

```flint
name = "world"
print("hello {name}")                  # -> hello world
print("json: {{\"a\": 42}}")           # -> json: {"a": 42}
print("tab:\there hex:\x41 uni:\u00e9")  # escapes: \n \t \r \\ \" \xHH \uHHHH
```

Rules:

- `{{` and `}}` are the escapes for a **literal** brace. Anything else after a
  single `{` is treated as an interpolation of an identifier, so `{{1}}` (not
  `{1}`) is how you write the number 1 in braces — SQL, JSON and printf-style
  text all need this.
- Interpolation takes an **identifier**. `{name}` works; `{1}` and
  `{a + b}` are not interpolated expressions and fail loudly.
- A JSON document with quoted keys is written `{{\"key\": value}}`.

`tests/t_strings.fl` pins all of this, and `tests/run.sh` gates it.

### String methods

The common operations are native runtime calls, rewritten at parse time
(`s.upper()` is exactly `flint_str_to_upper(s)`), so they work in JIT and
AOT builds with no Python detour. Multi-word names accept both spellings
(`startswith` / `starts_with`). Lengths and indices count **bytes**, not
Unicode code points; `upper`, `lower`, `capitalize`, and `title` map ASCII
bytes and leave multi-byte UTF-8 sequences unchanged.

| group | methods |
|---|---|
| length / emptiness | `len()`, `is_empty()` / `isempty()` |
| case | `upper()`, `lower()`, `capitalize()`, `title()` |
| whitespace | `trim()` / `strip()`, `lstrip()` / `ltrim()`, `rstrip()` / `rtrim()` |
| order | `reverse()` |
| indexing | `get(i)`, `charat(i)` / `char_at(i)` — the **byte value** (0–255); `codepointat(i)` — the UTF-8 code point, or -1 |
| search | `startswith(p)` / `starts_with(p)`, `endswith(s)` / `ends_with(s)`, `indexof(sub)` / `find(sub)`, `rfind(sub)` / `last_index_of(sub)`, `contains(sub)`, `count(sub)` |
| edit | `repeat(n)`, `replace(old, new)`, `substring(start, end)` / `slice(start, end)` |
| inspect / convert | `is_ascii()` / `isascii()`, `to_i64()` / `toi64()`, `to_f64()` / `tof64()`, `to_bool()` / `tobool()` |

`capitalize()` lowercases everything and uppercases the first byte;
`title()` does that per whitespace-separated word. `count("")` returns
`len + 1` (Python's convention), and the search methods return `-1` when
there is no match. Returned strings are heap-allocated by the runtime, as
the existing `flint_str_*` functions are. The gate is
`tests/test_string_methods.sh` (11 checks, JIT + AOT).

### Bytes: the scoped i8 buffer

`bytes` is a checked buffer of bytes, distinct from `[i64]` even though
both use the runtime's `{ptr, i64}` slice. Allocate it with
`flint_bytes_alloc(n)` (zero-initialised), index it with `b[i]` (reads
zero-extend to 0–255), assign with `b[i] = v` (stores the low 8 bits),
and release it with `flint_bytes_free(b)`. `len(b)`, `flint_bytes_len`,
`flint_bytes_data`, `flint_bytes_get`, and `flint_bytes_set` are also
available.

```flint
fn main() -> i64 {
    b: bytes = flint_bytes_alloc(4)
    b[0] = 300          # stores 44 (the low 8 bits)
    print(b[0])         # 44
    print(len(b))       # 4
    flint_bytes_free(b)
    0
}
```

Reads and writes are bounds-checked by default; the check disappears only
under `--unsafe`, exactly as for `[i64]`. The checker rejects mixing the
two types (`E0290`), because an i64 GEP would read eight bytes per
element. `bytes` is not a generic array and does not support
`for x in b`; use an indexed loop. The gate is `tests/test_bytes.sh`
(18 checks, JIT + AOT).

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

## Packages

A package is a `flint.toml` next to the directory you build. There are no
subcommands: dependency fetch is a build step, and `--offline` is a flag.

```toml
# flint.toml
[dependencies]
calc = "https://github.com/you/flint-calc"
```

```flint
import "calc"
```

```bash
./flintc main.fl                 # first build fetches "calc" into ~/.cache/flint_pkgs
./flintc --offline main.fl       # cache only; fail if the package is missing
```

`flint.lock` pins the resolved revisions. The registry gate is
`tests/test_registry.sh`.

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

### One command: compile + run (JIT, fastest iteration)
```bash
./flintc examples/hello.fl           # compiles + runs via ORC JIT
./flintc examples/comprehensive.fl   # full example
./flintc main.fl -- arg1 arg2        # forward program arguments
```

The bare form always compiles the current source and then runs it in
memory; it never reuses a stale executable.

### AOT: build + run in one command
```bash
./flintc examples/hello.fl -o hello   # builds ./hello, runs it, keeps it
./hello                               # the artifact is still there
```
`-o` compiles, links, and then executes the native executable it just
produced. The child inherits stdin/stdout/stderr, receives arguments after
`--`, and its exit status becomes `flintc`'s exit status. If compilation or
linking fails, nothing is executed. The output path is checked against the
input source (device+inode), so `flintc bench.fl -o bench.fl` is rejected
instead of replacing the program with the linked binary.

The extension selects an emit-only form instead: `.ll` = LLVM IR text,
`.o` = object file. Those are never run. `--no-run` (or `--build-only`)
keeps a native artifact without executing it:
```bash
./flintc examples/hello.fl -o hello --no-run
./hello
```
`--target` also emits without running, because a cross-target artifact is
not a host executable.
(Note: `-o` output may need `chmod +x` on some systems before running.)

### AOT to LLVM IR
```bash
./flintc examples/hello.fl output.ll
clang output.ll runtime.o -o hello
./hello
```

## Reading input

`flint_input` is the language's line-input primitive — the `input()`/`scanf`
slot, but bounded and explicit about failure:

```flint
fn main() -> i64 {
    name = flint_input("Name: ")
    if flint_input_status() != 0 {
        print("could not read a line")
        return 1
    }
    print("hello {name}")
    flint_str_free(name)

    n = flint_input_i64("How many: ")
    if flint_input_status() != 0 {
        print("that was not a whole number")
        return 2
    }
    print(n * 2)

    x = flint_input_f64("Scale: ")
    if flint_input_status() != 0 {
        print("that was not a number")
        return 3
    }
    flint_println_f64(x)
    0
}
```

Verified transcript (`printf 'Alice\n21\n0.5\n' | ./readme_input`):

```
Name: hello Alice
How many: 42
Scale: 0.5
```

The contract:

| call | result |
|---|---|
| `flint_input(prompt)` | one line as a heap `str`; the caller owns it (`flint_str_free`) |
| `flint_input_i64(prompt)` | whole line parsed as `i64`; `0` on any error |
| `flint_input_f64(prompt)` | whole line parsed as `f64`; `0.0` on any error |
| `flint_input_status()` | status of the most recent input call |

Status codes: `0` ok · `1` EOF before any byte · `2` line longer than 1 MiB
(the first 1 MiB is returned, the rest is drained) · `3` read error · `4` out
of memory · `5` not a number / trailing junk · `6` number out of range ·
`7` embedded NUL bytes were skipped (Flint strings are NUL-terminated).

Safety properties, all covered by `tests/test_input.sh`: one call consumes
exactly one line (a short buffer cannot desynchronise the next call); the
buffer is capped at 1 MiB and grown only up to the cap; an over-long line is
drained rather than left in the stream; CRLF is stripped; EOF is a status,
not a silent empty string; numeric helpers require the whole line to parse, so
`"12abc"` is an error rather than `12`; and a failed numeric parse never
returns a partial value.

## Tooling (verified flags only)

`./flintc --help` lists every flag; `./flintc --version` prints the
version. There are no `flintc <subcommand>` verbs — the interface is
flags plus standalone Python scripts:

```bash
./flintc --help                       # every flag, grouped
./flintc --version                    # flintc 0.22.0
./flintc main.fl -o myapp            # native binary (AOT); builds and runs
./flintc main.fl -o myapp --no-run   # native binary (AOT); build only
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
./flintc --emit-signatures a.fl b.sig      # resolved fn signatures (query
                                     # mode; feeds the self-host emitter)
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

## Diagnostics

There is no `flintc api` command and no `llms.txt`. The diagnostic catalogs
are the reference for what the compiler will reject:

- [`docs/errors-cpp.md`](docs/errors-cpp.md) — the C++ `flintc` you run:
  **149 fixed-pattern diagnostics over 157 stable codes**. Eight codes
  (E0137–E0144) build their message at runtime and appear as
  `<variable message>` in the code table. Four codes retired by Phase 2 / 2D
  (E0041–E0044) are not reused. The two warning codes (`W0001`, `W0002`)
  share the catalogue and the renderer but never fail a build.
- [`docs/errors.md`](docs/errors.md) — the self-hosted emitter:
  **149 codes**.
- [`docs/error-codes.csv`](docs/error-codes.csv) and
  [`docs/error-codes-pipeline.csv`](docs/error-codes-pipeline.csv) — the
  machine-readable code → message tables.

`tests/test_errors_cpp.sh`, `tests/test_errors.sh`, and
`tests/test_diagnostic_codes.sh` keep the catalogs and the code tables in
sync. The tutorial is the language tour; it is not a diagnostics reference.

### Warnings: unused variables

A local binding that is never **read** is reported like Rust's
`unused_variables` lint. A write is not a use, so `mut x = 0; x = 1` reports
`W0002` ("assigned but never read"), while a binding that is never touched
at all reports `W0001` ("unused variable"). Parameters, match bindings, and
destructured names are included; top-level globals are not (another
imported file may use them).

```flint
fn main() -> i64 {
    x = 1          # warning[W0001]: unused variable 'x'
    _scratch = 2   # silent: the underscore opts out
    0
}
```

Warnings never change the exit status, and they are printed for cached
builds too (the cache stores artifacts, not diagnostics). Silence them with
`--no-warn-unused` or `-Wno-unused`. The gate is
`tests/test_unused_warnings.sh` (20 checks).

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

18 `flint_*` runtime modules plus the core runtime (`runtime.c`) and the
Python bridge (`pyruntime.c`), 12,454 lines of C in total (`wc -l
runtime/*.c`, 2026-10-25), all reachable from Flint as `flint_*` builtins
(except the Python bridge). Verified by `bash tests/test_ai.sh ./flintc`
(17/17) and the `stdlib` gate in `tests/v1_gate_check.sh`.

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

## Cross-target output (emit-only)

`--target <triple>` exists and writes `.ll` text or `.o` for that triple.
A real WASM object was verified on 2026-10-25:

```bash
./flintc --target x86_64-pc-linux-gnu examples/hello.fl -o hello.x86.ll
./flintc --target wasm32-unknown-wasi examples/hello.fl -o hello.wasm.o
file hello.wasm.o     # WebAssembly (wasm) binary version 0x1 (MVP module)
```

What does **not** exist: cross-target linking or execution (asking for an
executable target fails in the host linker), a WASI sysroot/runner on the
device, and `--emit-header` (a C compiler cannot consume Flint prototypes
today). `--target` without `-o` refuses. These are tracked future work
([`docs/tiers.md`](docs/tiers.md)), not current features.

## Benchmarks

The suite has two harnesses: `tools/bench_compare.sh` for runtime against
C, C++, Rust, and Python, and `tools/bench_tooling.sh` for binary size,
cold build time, and startup. The directory holds 6 shootout programs,
`matmul.fl` (tensor/BLAS path), and 13 `test_*` if/call micro-probes —
20 `.fl` files total.

```bash
bash tools/bench_compare.sh sum_array primes pi strrev
bash tools/bench_tooling.sh
./flintc benchmarks/matmul.fl -o matmul   # builds, runs, keeps ./matmul
```

The device, the exact commands, and the "what is not measured" caveats are
in [`benchmarks/README.md`](benchmarks/README.md). The harness builds Flint
AOT; the large table below is a **2026-09-12 JIT snapshot**, so its numbers
differ slightly from a fresh harness run.

Historical snapshots are labeled with their session — numbers move with
device thermal state (in that session: little cores at 691 MHz, big at
2.2 GHz; `pi` reproduced exactly, `sum_array` did not — see note).

Measured 2026-09-12, Termux AArch64, JIT (in-program timers; `clang -O2`
for the C/C++ columns):

### Runtime — large workloads (Flint JIT vs C/C++ `-O2` vs Python)

| Benchmark | Flint default | Flint `--unsafe` | C | C++ | Python |
|-----------|---------------|------------------|---|-----|--------|
| sum_array 10M | 19.3 ms | 16.4 ms | 9.2 ms | 9.8 ms | 175.7 ms |
| pi 100M iters | 777.5 ms | n/m | 782.8 ms | 778.8 ms | >60 s (timeout) |
| primes 10M | 542.9 ms | 481.4 ms | 199.1 ms | 203.9 ms | 2,092 ms |
| fib(45) | 12,735 ms | 8,483 ms (single run; not a stable speedup) | 9,328 ms | 9,333 ms | skipped (est. 10+ min) |
| strrev (100K builder) | ~1.0 ms | ~0.95 ms | 5.5 ms | 32.0 ms | 444.9 ms |
| matmul 500² (10 iters, AOT) | 42.6 ms | n/m | 428.4 ms (naive) | n/m | 42.7 ms (numpy 2.4.4) |

`--unsafe` skips overflow + bounds checks: faster, but drops the safety
that justifies Flint vs C. Failures and skips reported as-is, not hidden.

Note (2026-10-26): the `fib(45)` default cell is history. A closed-world
bounded-range analysis (`STATE.md` §4.3) proves the
`fib(n-1) + fib(n-2)` overflow check safe and stops emitting it; that
check's branch was blocking LLVM's accumulator-recursion transform, not
just costing an instruction. Same-session AOT, three alternating runs:
Flint **8.60–9.17 s** vs `clang -O2` **8.54–8.76 s** (within a few percent;
the earlier-session default was 11.6–12.6 s). The safety model is unchanged:
only whole-program executables get the facts; `.ll`, `.o`, interface,
`--link`, `--target`, and `--check-bce` keep every check.
`./flintc --check-bce benchmarks/fib.fl` prints the three fact counts.

Note (2026-10-26): the `sum_array` row is also history. The middle-end
pipeline built its `PassBuilder` without a `TargetMachine`, so
`TargetTransformInfo` reported no vector registers and the loop vectorizer
declined **every** loop; `opt -O2` on the same module vectorized it. With a
TM for the module's triple, `--unsafe sum_array 10M` runs at
**7.49–7.88 ms** vs `clang -O2` **7.60–7.91 ms** (C parity, same session).
The default build is **11.0–11.1 ms**: its accumulator check is still
emitted, and a checked reduction cannot be reassociated/vectorized. The
safety model is unchanged; `tests/test_vectorize.sh` pins the vectorizer
metadata, the retained check, and the exact sum.

Note (2026-10-27): the `primes` row compares different element sizes, and
Flint now has both representations. `[i64]` is 8-byte elements (80 MB
sieve); `bytes` is the scoped i8 buffer (10 MB, same as the C/C++/Rust
mirrors). Same-session AOT control (`tools/bench_primes_i64.sh`, three
rounds, medians): Flint i64 **509.5 ms** vs C i64 **423.5 ms**, C++ i64
**386.7 ms**, Rust i64 **418.3 ms** (this session's i64 rows were noisy:
419.8–542.3 ms; the 2026-10-26 session measured 422.5 ms, within 7% of
C). The point of the control is the byte row: Flint `bytes` is
**185.8 ms** vs the 1-byte C sieve **195.5 ms** — the element-size gap is
closed by the new type, not by dropping checks. Generic element-size
arrays (M7) remain future work; the scoped buffer is what sieve-like code
needs. The control sources are in `benchmarks/controls/`.

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

### Binary size (current)

Measured 2026-10-25 with the documented command
(`./flintc examples/hello.fl -o hello`):

| Artifact | Size |
|---|---|
| raw executable | **8,048 B** |
| `strip`ed | **4,160 B** |
| `.text` | 320 B |
| `.bss` reserved | 24 B |
| dynamic dependencies | `libc.so` only |

The 141 KB / 111 KB figure from 2026-09-24 predates the
`-Wl,--gc-sections` link fix (`STATE.md` §6.1). It is history, not the
current size; `tools/bench_tooling.sh` reproduces the current number by
compiling the same program with a unique comment (the cache is keyed by
source content, so a fresh path alone is not enough).

### Compile speed — large files (measured 2026-10-25; wall clock)

Input sizes are today's tree. The `flintc` rows are **cache-miss builds**:
each run compiles a same-directory copy whose bytes differ (a unique
comment), so the content-addressed cache cannot serve a previous artifact;
median of three runs on an idle device. The `clang` rows are single runs on
the same device (an earlier, hotter run measured 103,939 ms for `-O2` and
43,935 ms for `-O0` — thermal spread, not a code change). `flintc` rows
write `.ll` (text), `clang -c` rows write an object; the caveat below the
table matters.

| Compiler | Input (size) | Time | Throughput |
|----------|--------------|------|------------|
| `clang++ -O2 -c` | `src/main.cpp` (787,276 B, 16,225 lines) | 79,700 ms | 9.6 KB/s, 204 lines/s |
| `clang++ -O0 -emit-llvm` | `src/main.cpp` (same) | 32,373 ms | 23.7 KB/s, 501 lines/s |
| `clang -O2 -c` | `runtime/runtime.c` (48,525 B, 1,454 lines) | 1,447 ms | 32.7 KB/s, 1,005 lines/s |
| `flintc` default | `stage2/parser.fl` (135,513 B, 4,379 lines) | 896 ms | 147.7 KB/s, 4,887 lines/s |
| `flintc` default | `stage1/lexer.fl` (71,594 B, 904 lines) | 328 ms | 213.2 KB/s, 2,756 lines/s |

A **cache hit** (identical source bytes) skips lex/parse/codegen and is not
a compile, so it is not a row here; `--print-size` prints nothing on a hit.
The first compile in a fresh session can be several times slower — 3,201 ms
for `parser.fl` and 861 ms for `lexer.fl` were observed on the same tree
before the device settled — because of CPU ramp and cold page cache, not
module caching.

`flintc` emits 687 KB of textual IR from the 136 KB parser (5.1×), and
144 KB from the 72 KB lexer (2.0×). `clang++ -O0 -emit-llvm` writes 4.77 MB
of bitcode from 787 KB (6.1×) — a different format and a different
language, so the expansion ratios are not a head-to-head result.

The one same-session cross-language fact is wall time: Flint compiles its
own 136 KB parser in 896 ms, while `clang++ -O0` needs 32,373 ms for the
787 KB C++ compiler. That is **not** a controlled comparison (the inputs,
the languages, and the header load differ); it is the reason the bootstrap
is practical on a phone, not a claim that Flint is 36× faster than C++.
The `--fast` tier is currently a no-op, so no `--fast` row is listed as a
current measurement; the historical numbers are below.

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
less-optimized code: `parser.fl` IR was 828 KB vs 628 KB default
(+32%), and `sum_array` ran 42.5 ms vs 19.3 ms (2.3x slower).

## Testing

What exists and how to run it (file lists and line counts verified
2026-10-26 with `wc -l` and `tools/docs_check.sh`):

| Suite | Files | Lines | Run |
|-------|-------|-------|-----|
| `tests/` smoke tests | 5 `.fl` (`t_hello` 8, `t_arith` 26, `t_flow` 48, `t_funcs` 15, `t_types` 23) | 120 | `bash tests/run.sh ./flintc` |
| `tests/` runners | `run.sh` 116, `run_tutorial.sh` 18, `test_registry.sh` 43 | 177 | `bash tests/test_registry.sh` |
| diagnostic catalogs | `docs/errors-cpp.md` (149 fixed-pattern diagnostics over 157 codes, including W0001/W0002) + `docs/errors.md` (149 emitter codes) | — | `bash tests/test_errors_cpp.sh` · `bash tests/test_errors.sh` |
| `benchmarks/` workloads | 6 shootout `.fl` (`fib` 15, `fib2` 11, `pi` 23, `primes` 33, `strrev` 34, `sum_array` 31) + 13 `test_*` probes (86) | 233 | `bash tools/bench_compare.sh` |
| `benchmarks/` mirrors | 7 workloads in C, C++, Python (+numpy matmul mirror) + 2 Rust oracle mirrors + the iterative `fib_iter` set | 461 | `clang -O2` / `g++` / `python3` / `rustc` |
| `benchmarks/` iterative fib | `fib_iter` in Flint/C/C++/Rust/Python, same iterative algorithm | 122 | `bash tools/bench_compare.sh fib_iter` |
| `runtime/rust/` stdlib modules | `no_std` Rust: CSV parser, SHA-256/HMAC/AES-256-CTR/GCM (RustCrypto), SQLite wrapper over libsqlite3 | — | `bash tests/test_rust_stdlib.sh ./flintc` |
| HTTPS | `flint_http_get`/`post` over the platform libcurl; verification always on, bodies capped at 8 MiB | — | `bash tests/test_https.sh ./flintc` |
| `tests/rust/` FFI + oracle | 4 Rust sources, 2 Flint FFI programs; both ABI directions + 2 result oracles | — | `bash tests/test_rust_ffi.sh ./flintc` · `bash tests/test_rust_oracle.sh ./flintc` |
| `lsp/` language server | Rust, `serde_json` only; 21 unit tests + 31 protocol checks + keyword/builtin drift gates | — | `bash tests/test_lsp.sh ./flintc` |
| `editors/vscode/` extension | manifest, TextMate grammar (26 patterns), language config, stdio client | — | `python3 tools/check_vscode_extension.py` |
| `tutorial/` lessons | 8 (`01_hello`–`08_wrap`), each with `EXPECT` checks | 198 | `bash tests/run_tutorial.sh ./flintc` |
| `stage1/` Flint lexer | `lexer.fl` + edge corpora, byte-identical to `--dump-tokens` | — | `bash tests/test_lexdiff.sh ./flintc` |
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
./flintc examples/python_demo.fl -o pydemo   # builds, runs, keeps ./pydemo
```

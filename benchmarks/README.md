# Benchmarks: what is measured and how to reproduce it

The numbers in the top-level [`README.md`](../README.md) are session-dated
measurements, not a leaderboard and not a claim that Flint beats C. This
file records the method, the device, and the commands so a reader can
reproduce or refute a row.

## Device and toolchain

The measurements in the README were taken on:

- Termux on Android, AArch64 (`Linux localhost 4.19.157-perf+`, 8 cores)
- `clang`/`clang++` 21.1.8 (`aarch64-unknown-linux-android24`)
- Python 3.14.6
- Flint `0.22.0` (`./flintc --version`)

Absolute timings on a phone move with thermal state. The README records the
observed core clocks when a session was noisy; compare same-session medians,
not a number from one month with a number from another.

## Runtime: the comparison harness

```bash
bash tools/bench_compare.sh                 # sum_array primes fib pi
bash tools/bench_compare.sh sum_array pi    # a subset
```

For each workload the harness:

1. builds the Flint program with `flintc ... -o` (AOT, default opt level);
2. builds the C and C++ mirrors with `clang -O2` / `clang++ -O2`;
3. builds the Rust mirror with `rustc -O` when the `.rs` file exists;
4. runs the Python mirror with `python3`;
5. reads the program's own `time: <ns>` line (`flint_time_ns` /
   `clock_gettime(CLOCK_MONOTONIC)`), so process startup is outside the
   timed region.

The harness **does not** time JIT execution. The README's large runtime table
is a 2026-09-12 JIT snapshot; an AOT run through the harness will differ
slightly. Both use the same in-program timer.

`benchmarks/matmul.fl` is separate because it exercises the tensor/BLAS path:

```bash
./flintc benchmarks/matmul.fl -o matmul && ./matmul
clang -O2 benchmarks/matmul.c -o matmul_c && ./matmul_c
python3 benchmarks/matmul_np.py
```

## Iterative fib comparison

The original `fib.*` mirrors are naive recursive `fib(45)`. The Python
recursive version is too slow for a useful comparison, so `fib_iter.*`
implements the same **iterative** algorithm in Flint, C, C++, Rust and
Python and is benchmarked as its own workload:

```bash
bash tools/bench_compare.sh fib_iter
```

Each program times one `fib_iter(45)` call and prints the result
(`1134903170`) plus `time: <ns> ns`. The harness prints sub-millisecond
results in `us`; a single call is 0.6–0.9 us in the compiled languages and
roughly 15–18 us in Python here. These are single-run microbenchmarks at the
timer's resolution, so treat small differences as noise; the Python gap is
large enough to be stable. The recursive `fib.*` files stay in place and are
still the workload used by the historical README table.

## Bounded-range facts and the recursive fib

The recursive `fib.*` mirrors are the workload that exposed a check-induced
optimization loss. LLVM's accumulator-recursion transform turns
`fib(n-1) + fib(n-2)` into a loop, but only when the addition has no
overflow branch. The closed-world bounded-range analysis proves the sum
fits for an executable built from the whole program, so the branch is not
emitted and the transform fires; `fib(45)` runs at C speed (see the
top-level README note). The proof is disabled for `.ll`, `.o`, interface,
`--link`, and `--target` builds, which can be called from outside the
analyzed program:

```bash
./flintc --check-bce benchmarks/fib.fl   # 3 emitted, 1 survives O2
./flintc benchmarks/fib.fl -o fib.ll     # checked IR
```

`tests/test_range_facts.sh` is the gate: it checks the elision, the
overflow traps (constant, dynamic, branch-assigned, and map-literal call
sites), and the emit-mode retention.

## The middle-end TargetMachine and the vectorizer

The same session found a second, larger gap. `runLLVMOptimizations` built
`PassBuilder` with no `TargetMachine`, so `TargetTransformInfo` reported no
vector registers and LLVM's loop vectorizer declined every loop in every
build. `opt -O2` on the identical module vectorized it. Passing a TM for
the module's triple fixed it; `--unsafe sum_array 10M` went from 10.1 ms to
7.49–7.88 ms against `clang -O2` 7.60–7.91 ms. The default build stays at
11.0–11.1 ms because its accumulator check is still emitted and a checked
reduction cannot be vectorized. The compile-time cost on the stage3 emitter
is ~0.5–0.8 s. `tests/test_vectorize.sh` pins both directions.

## The pipeline's consumers now use `-O2`

The C++ compiler optimizes in-process; the self-hosted pipeline writes
text IR, and its consumers used to compile that IR at `-O0` — including
`driver/flintc.fl`, so pipeline-built programs shipped unoptimized. On
2026-10-26 the differential gate, the self-host bootstrap, the stage3
ladder and the driver switched to `clang -O2`. Same self IR, measured
three runs each: the pipeline-compiled emitter goes from 0.452 s to
0.191 s on the merged 331-form input, matching the C++-compiled emitter
(0.193 s), and both emit byte-identical IR. A pipeline-built
5M-iteration loop goes from 0.096–0.127 s to 0.059–0.068 s. The gates
stay green (differential 14/14, ladder 21/21, driver 6/6, fixed point
byte-identical); the differential's wall time rises from 30 s to 42 s
because it now optimizes 28 binaries.

## Tooling: binary size, build time, cold start

```bash
bash tools/bench_tooling.sh
```

This builds a fresh hello-world per compiler and reports raw and stripped
size, cold build wall time, and process start-to-exit time. The Flint source
carries a unique comment per run, because the cache is keyed by source
content rather than path (see the cache note below). The current hello-world
result is also recorded in `STATE.md` §6.1.

A one-off size check:

```bash
./flintc examples/hello.fl -o /tmp/hello
stat -c %s /tmp/hello            # 8048 on 2026-10-25
cp /tmp/hello /tmp/hello.stripped && strip /tmp/hello.stripped
stat -c %s /tmp/hello.stripped   # 4160 on 2026-10-25
```

The binary cache is keyed by **source content** (plus compiler identity,
codegen flags, and imported-module hashes), not by output path. Writing to a
new output path — or copying identical source bytes to a new path — still
hits the cache; `--print-size` then prints nothing, because nothing was
linked. To force a real build, clear the cache (`rm -rf ~/.cache/flintc`) or
change the source bytes (for example, append a unique comment). A cached
binary is not a new measurement.

## Compile speed

The README's compile-speed table was re-measured on 2026-10-25 against
today's tree. The `flintc` rows are **cache-miss builds**: each run compiles
a same-directory copy whose bytes differ (a unique comment), median of three
runs on an idle device. The `clang` rows are single runs on the same device.

The stage sources import siblings by relative path (`stage2/parser.fl`
imports `../stage1/lexer.fl`), so a copy in `/tmp` fails with `E9001`. Use a
same-directory copy and change its bytes every run:

```bash
T=$(mktemp -d)
for i in 1 2 3; do
    cp stage2/parser.fl stage2/zz_bench_parser.fl
    printf '\n# bench %s-%s\n' "$$" "$i" >> stage2/zz_bench_parser.fl
    time ./flintc stage2/zz_bench_parser.fl -o "$T/parser.$i.ll"
done
rm -f stage2/zz_bench_parser.fl
time clang++ -O2 -c src/main.cpp -o "$T/main.o"
```

The unique comment matters: the cache is keyed by source content, so
recompiling identical bytes is a cache hit (no lex/parse/codegen) and would
report a much smaller number. Clearing `~/.cache/flintc` is not required and
does not make the measurement more cold — imports are inlined into the one
module, so the compile work is the same once the source bytes differ.

Observed on 2026-10-25 (idle device): `parser.fl` median **896 ms**,
`lexer.fl` **328 ms**. The first compile of a fresh session measured
3,201 ms / 861 ms on the same tree — CPU ramp and cold page cache, not a
different build mode. `flintc` rows write `.ll` (text), `clang -c` rows
write an object — the comparison is not apples-to-apples, as the README says
next to the table.

## Rules for adding a row

- Add the mirror in the same directory (`fib.c`, `fib.cpp`, `fib.py`,
  `fib.rs`) and keep the workload identical.
- Time only the workload, not allocation or process startup, unless the row
  is explicitly a startup row.
- Print a checksum or the result, so a fast wrong answer is visible.
- Record the date, device, compiler version, and any non-default flag.
- Do not add a "faster than X" claim from a single run. State the ratio and
  the condition; the gate (`tools/bench_gate.sh`) is the regression check,
  not a marketing table.

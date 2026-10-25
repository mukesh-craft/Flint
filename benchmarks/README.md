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

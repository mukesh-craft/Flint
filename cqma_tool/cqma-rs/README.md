# cqma — C/C++ Code-Quality & Memory-Safety Analyzer

A single static Rust binary that audits a C or C++ file or project across four
independent stages and writes a Markdown report plus machine-readable JSON.

No Python, no build system, no configuration required. Point it at a directory
and it finds the sources itself.

```
cqma <file-or-dir> [options]
```

## Stages

| # | Stage | What it catches |
|---|-------|-----------------|
| 1 | Compiler warnings | `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` per translation unit |
| 2 | cppcheck | `--enable=all`: null derefs, unused functions, style, portability, logic bugs |
| 3 | Clang static analyzer | Path-sensitive: null derefs, leaks, dead stores, uninitialized reads |
| 4 | Runtime (ASan + UBSan + leak tracker) | Real buffer overflows, UAF, UB, and unreachable heap memory at exit |

Stage 4 only runs when there is something to run: a single-file target with a
`main`, a `--build-cmd`, or a `--binary`. Everything else still runs on any
project.

## The leak problem on Android/Termux

LeakSanitizer is **not built into the sanitizer runtime on Termux/Android**:
`ASAN_OPTIONS=detect_leaks=1` prints `detect_leaks is not supported on this
platform` and aborts. Valgrind is not packaged either.

cqma therefore ships its own tracker (`src/leak_hooks.c`), linked into every
ASan build it creates:

* `__sanitizer_malloc_hook` / `__sanitizer_free_hook` record every live block
  in a fixed-size, `mmap`-backed hash table — no allocator recursion.
* At exit the process is scanned **conservatively and transitively**: writable
  `PT_LOAD` segments of every module are roots, and any block reachable from a
  reachable block is marked live. This is the same fixpoint LeakSanitizer uses,
  and it is what stops a container's payload from being reported as lost.
* Blocks that remain unreachable are grouped by allocation site and reported
  with a symbolized stack.

Stack scanning is **off by default**. At exit, dead frames still hold stale
pointers to genuinely leaked blocks, which would mark every leak reachable.
Set `CQMA_LEAK_SCAN_STACK=1` to include the stack as a root (useful when a
leaked block is only reachable from a still-live frame).

The tracker can only see binaries that link its hooks. A user-supplied
`--binary` built without them still gets ASan/UBSan coverage, but its leaks
are invisible — the report says so rather than claiming the binary is clean.
`--build-cmd` links the hooks into the build, so that path does get leak
detection. `--build-cmd` runs *before* the `--binary` path is used, so
`cqma . --build-cmd "make" --binary ./app` builds and then runs the fresh
binary.

Tracker environment variables (set automatically by cqma):

| Variable | Meaning |
|---|---|
| `CQMA_LEAK_REPORT` | path for the report (default stderr) |
| `CQMA_LEAK_ROOT` | only report stacks containing a frame under this path |
| `CQMA_LEAK_SLOTS` | hash-table slots (default 262144) |
| `CQMA_LEAK_SCAN_STACK` | `1` = treat the stack as a root |
| `CQMA_LEAK_DEBUG` | print every symbolized frame while grouping |
| `CQMA_LEAK_DEBUG_ALL` | print every live block with its allocation site |
| `CQMA_LEAK_DEBUG_SCAN` | print each pointer that marks a block reachable |
| `CQMA_LEAK_DISABLE` | `1` = disable tracking |

### Platform notes (Termux/Android)

These are not optional tweaks; each one was required to make the tracker work:

* **`-fno-emulated-tls`** — Termux clang defaults to emulated TLS, whose
  accessor (`__emutls_get_address`) calls `malloc`, which re-enters the hook.
  Observed as a stack overflow inside `__asan::TryAsanInitFromRtl`.
* **`__thread` variable aligned to 64** — Bionic refuses to load an aarch64
  executable whose TLS segment is underaligned:
  `executable's TLS segment is underaligned: alignment is 4, needs to be at
  least 64`.
* **`detect_leaks=0`** — LSan is absent; the tracker replaces it.
* **`UBSAN_OPTIONS=halt_on_error=1`** — recovery mode spins forever on this
  platform.

## Usage

```bash
# Whole project
cqma . --compile-commands auto --output audit/report.md --json audit/report.json

# One file, skip the runtime stage
cqma src/foo.c --no-runtime

# Run an already-built sanitized binary
cqma . --binary ./build/app --run-args "--input test.txt"

# Let cqma build the project with sanitizers
cqma . --build-cmd "make -j4" --binary ./build/app

# Analyze a Flint program's runtime (emit IR, link the tracker by hand)
# see docs/flint-runtime.md
```

Useful flags: `--no-compile`, `--no-cppcheck`, `--no-analyzer`, `--no-runtime`,
`--cppcheck-level normal|exhaustive`, `--timeout-cppcheck`,
`--timeout-analyzer`, `--jobs`, `--max-files`, `--exclude`, `--exit-zero`.

`--cppcheck-level exhaustive` is the default for single files. On a large tree
it is not usable: measured on `src/main.cpp` it ran over 30 minutes and produced
only `missingInclude` noise, while `normal` covered the same tree in ~175 s.
Use `normal` for whole-project audits.

## Verdicts and exit codes

The report never claims `CLEAN` when coverage was incomplete. If a stage was
skipped, a stage command failed, `--max-files`/`--exclude` dropped an input,
cppcheck could not extract a working preprocessor configuration
(`noValidConfiguration`), output was truncated, or leak coverage was
unavailable, the verdict says `ANALYSIS INCOMPLETE`, lists every gap, and the
score is `null`/unavailable. A numeric score is only printed for runs where
every requested stage completed. `100` is reserved for a complete run with zero
confirmed findings: any confirmed finding caps the score at `99`, so a large
tree cannot round a real warning up to a perfect score.

| Exit | Meaning |
|---|---|
| `0` | every requested stage completed and no finding was reported |
| `1` | every requested stage completed and at least one finding was reported |
| `2` | configuration/toolchain error (bad arguments, report could not be written) |
| `3` | analysis incomplete — at least one requested stage did not finish |

Operational warnings (timeouts, a failed analyzer command, a truncated pipe, a
Valgrind error summary) are kept in the report and JSON with
`"operational": true`, but they do not lower the defect score. They still make
the run incomplete, so they cannot produce a false `CLEAN`.

To explicitly exclude a stage from the definition of "complete", disable it
with `--no-compile`, `--no-cppcheck`, `--no-analyzer`, or `--no-runtime`.
Disabling every stage is itself incomplete (exit `3`), never clean.

## Robustness limits

Child output is retained up to 8 MiB per stream; the pipe is still drained
after the cap so a chatty compiler cannot deadlock, and the stage is marked
truncated. Compiler, cppcheck, analyzer, and program runs all have explicit
timeouts.

## Design notes

* **cppcheck runs single-threaded.** `-j4` silently disables `unusedFunction`
  (6 findings at `-j1` versus 4 at `-j4` on the same tree).
* **cppcheck gets project include paths only.** Feeding it the Termux SDK
  headers breaks preprocessing (`limits.h` uses `__has_include_next`, which
  cppcheck 2.22 cannot evaluate) and drops entire files.
* **The Clang analyzer is Clang, explicitly.** `--analyze` is Clang's driver
  flag; GCC's equivalent is `-fanalyzer`, a different engine with different
  output. If `--cc`/`--cxx` is not Clang, cqma looks for `clang`/`clang++` and
  uses that. If no Clang is installed, the stage is marked `unavailable` and
  the run is incomplete — it never passes `--analyze` to GCC and calls the
  resulting failure clean.
* **System `-I` paths are demoted to `-idirafter`, not promoted to `-isystem`.**
  `-isystem $PREFIX/include` shadows libc++'s `math.h` wrapper, so
  `std::isfinite` expands to `std::__builtin_isfinite` and produces phantom
  errors (measured: 8 with `-isystem`, 0 with `-idirafter`). `-idirafter` keeps
  third-party headers out of the warning path without changing which header
  wins.

## Building

```bash
cargo build --release --offline   # only dependency: serde_json
```

## Regression fixtures

`../cqma-fixture/` holds a C file with exactly 9 known findings, a 64-byte leak
fixture, and a C++ `new[]` leak fixture. They are the reference for parser and
tracker changes.

`../cqma-tests/` is the detector validation suite:

| Path | Contents |
|---|---|
| `small/bugs.cpp` | 100 lines, 10 injected defects (`[D1]`…`[D10]`) |
| `large/bugs_large.cpp` | 598 lines / 65 functions, the same 10 defects (`[L1]`…`[L10]`) |
| `verify.py` | asserts every tagged defect produced a matching finding within ±5 lines |
| `flint/` | two compiled Flint programs proving the leak tracker on the language |
| `test_regressions.sh` | false-clean regressions: all-disabled, skipped input, failed analyzer, Valgrind timeout/no-log, valid Valgrind run |

```bash
cd ~/Flint/cqma_tool/cqma-tests
~/Flint/cqma_tool/cqma-rs/target/release/cqma small/bugs.cpp \
  --output small/report.md --json small/report.json
python3 verify.py small/bugs.cpp small/report.json   # -> 10/10
bash test_regressions.sh ~/Flint/cqma_tool/cqma-rs/target/debug/cqma  # -> 10/10
cd ~/Flint/cqma_tool/cqma-rs && cargo test                            # unit tests, incl. leak dedup
```

Known analyzer gap found by these fixtures: a null returned through a helper
(`p = maybe(ok); return *p;`) is not reported by the clang analyzer, though
cppcheck's `nullPointer` catches it and the inline form is caught by both.

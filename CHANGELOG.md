# Changelog

All notable changes to Flint are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.0.0/); versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0/).

## [Unreleased]

### Added
- **C-ABI interop proven in both directions, with Rust on both sides.**
  Flint could call C (`examples/ffi_demo.fl`), but nothing called *Flint*:
  the README's "FFI both directions" claim had no reverse test. Two facts
  make it real and both are now gated (`tests/test_rust_ffi.sh`, 8 checks,
  rustc + clang only, no cargo and no network):
  - **Flint -> Rust**: `tests/rust/flintrust.rs` builds as a static library
    exporting `rust_add`/`rust_sum_to`/`rust_strlen`; `tests/rust/from_flint.fl`
    declares them `extern "C"` and links it via `--link`, printing
    `42 / 500500 / 5`. The Rust side checks its pointer arguments and
    reports failure as a sentinel instead of trapping.
  - **Rust -> Flint**: `tests/rust/lib_add.fl` is a Flint module with no
    `main`, which `flintc -o lib_add.o` emits with user functions as
    unmangled globals (`T add`). `tests/rust/to_rust.rs` links that object
    plus the Flint runtime objects and calls `add(40,2) == 42` and
    `flint_str_length("hello") == 5` directly — the runtime ABI is
    reachable from Rust, not only from Flint.
- **`tests/test_rust_oracle.sh`: a memory-safe result oracle.** The benchmark
  suite compares *timings* between mirrors; nothing compared *results* across
  languages. `tests/rust/oracle_sum.rs` and `tests/rust/oracle_primes.rs`
  reimplement the `sum_array` and `primes` workloads with checked arithmetic
  and a `Vec` sieve, and the deterministic output line must be byte-identical
  to Flint's (`sum: 99999990000000`, `primes up to 10M: 664579`); timing
  lines are excluded by design. It earned its keep immediately: the first
  Rust primes mirror counted marked composites and reported 9335420.
- **LICENSE (Apache-2.0) + license pointer in README.** The repository had
  no license file, which legally blocked third-party reuse of Flint source
  and artifacts. The full Apache-2.0 text is now in `LICENSE` (copyright
  line: 2026 mukesh-craft), and README carries a `License:` line linking it.
- **`flintc --help` / `--version`** (`-h` / `-V`). Neither existed:
  `--help` fell through to "cannot open '--help'" and the usage text was
  reachable only on the bare-no-args error path. `--help` prints the full
  flag inventory grouped by purpose (output, codegen, safety, diagnostics
  and modes), each entry read from the parser so the text cannot drift
  from the flags it documents; `--version` prints the build-stamped
  version (`flintc 0.22.0`). Both exit 0 and are smoke-gated
  (`tests/run.sh`, 25 -> 27 checks).
- **`tests/test_errors.sh --fix`: the error catalog regenerates itself.**
  The 12 diagnostics added with the as-cast / tensor-literal / map-index
  work (commit 809a172) shipped without catalog entries, so the catalog's
  "generated list" provenance claim was false (116 documented vs 128
  emitted, and its own gate said so). `--fix` regenerates the list from
  `stage3/flint_emit.fl` in the curated shape (lambda family first, the rest
  sorted) and recomputes the total: `errors-catalog: 128/128 in sync`.
- **`--debug`: real DWARF line tables in artifacts.** The module previously
  carried no debug info, so gdb/lldb/llvm-dwarfdump could not map anything
  back to Flint source. The flag builds a DIBuilder compile unit + file +
  one `DISubprogram` per user function + per-statement `DILocation` (every
  statement can map, including block-tail expressions), and module passes
  run at O0 under `--debug` because O2 constant-folds statements and erases
  their line info. Verified by `tests/test_dwarf.sh` against
  `llvm-dwarfdump`: CU, subprograms, complete line table, source lines
  4/5/8-12 of `tests/t_debug.fl` all map. Scope truth: `--debug` requires
  `-o` (bare JIT-run refuses with an actionable message — an ORC run with
  debug metadata hangs at `mainFn`, reproduced with everything else held
  constant), because DWARF belongs to artifacts (.ll, .o, linked binaries),
  which work end-to-end (`--debug -o prog && ./prog` runs and the artifact
  keeps full debug info).
- **`--target <triple>`: cross-target emit control.** The module triple and
  datalayout were hardcoded to the Android host, so nothing could emit for
  another machine. `flintc main.fl --target x86_64-pc-linux-gnu
  --emit-llvm -o main.ll` now emits IR with the target's canonical triple
  and datalayout (taken from a real TargetMachine, not hand-written) and
  `.o` paths emit real objects — verified byte-identical to clang's own
  datalayouts for x86_64 and wasm32, with a real WASM object checked in
  `tests/test_target.sh`. `--target` without `-o` refuses loudly because
  ORC JIT execution is host-triple only; WASI execution (sysroot, `_start`,
  runner) remains documented T3.
- **Weight checkpoints (FLWT v1).** `flt_weights_save(path, tensor, append)`,
  `flt_weights_load(path, index)` and `flt_weights_count(path)` serialize
  tensors to a tiny versioned format (magic "FLWT", per-record dtype/rank/
  dims/data, little-endian, rank 1-5 for f64 and 1-4 for f32). f64 values
  round-trip bit-identically; the f32 dtype is carried so a loaded tensor
  computes with the right width. `tests/t_chkpt.fl` covers the round-trip,
  count re-walk, and out-of-range reads failing the null way.
- **`as` casts and tensor literals.** `7.9 as i64` truncates toward zero;
  `[[1.0, 2.0], [3.0, 4.0]]` builds a real f64 tensor (rank 1 and 2). All
  three backends (C++, emit-mode, self-hosted) agree.
- **Timezone zone-info accessors.** `flint_tz_abbrev_at(zone, epoch)` and
  `flint_tz_is_dst_at(zone, epoch)` expose everything `flint_tz_info`
  computes as scalar calls (Flint cannot express the C out-param struct).
  `tests/t_time.fl` covers both.
- **Timezone library** (`runtime/flint_time.c`, wired through `flint_time.h`):
  calendar maths, DST rules for ~100 zones, real `%Z` abbreviations, and
  zone-aware parsing. Verified by an 8,817-check C comparison against the
  libc oracle (`tests/time_check.c`).
- **f32 tensors.** Every tensor op is dtype-aware; f32 is half the memory and
  ~1.9x the matmul. f32 computes in double and narrows on store, so
  `exp`/`tanh`/`sigmoid` keep double precision in the last place it matters.
  Mixed-width binary ops promote to f64.
- **Machine-learning layers** (`flint_nn.c`): `flt_conv2d`, max/avg/global
  pooling, batchnorm, layer_norm, rms_norm, softmax, dropout, l2_reg, flatten,
  and Xavier/He initialisation. Convolution could not be expressed at all
  before these existed.
- **Utility layer** (`flint_util.c`): sorting (quicksort with a heapsort
  fallback, plus a stable merge sort), binary search, a hash set, a binary
  min-heap, a deque, mutex/condvar/rwlock, atomics, a CSPRNG
  (xoshiro256**, `/dev/urandom`-seeded), and a declarative CLI parser.
- **Maths and statistics** (`flint_math.c`): integer maths, angle
  conversion, power/log variants, fp classification, `erf`/`erfc`/`gamma`/
  `lgamma`, the constants, and the full descriptive-statistics block with a
  Welford streaming accumulator.
- **printf-style formatting** (`flint_fmt.c`). Flint previously had no format
  function at all, so every program concatenated with `flint_i64_to_string`.
- **A shared value model** (`flint_value.c`) used by every config format, with
  a TOML v1.0.0 parser and a YAML parser. Both reject what they cannot
  handle rather than mis-parsing it.
- **A gradient checker** (`tests/gradcheck.c`) comparing analytic gradients
  against central finite differences, for f64 and f32. It found a 2x error in
  `mul`, a 4x double-count from processing a repeated input twice, and a seed
  that was silently discarded.
- **`tests/test_ai.sh`** — the standard-library and AI gate, now 17 checks.

### Fixed
- **`flint-fmt` applied to the three self-host canon files**
  (`stage1/flint_lex.fl`, `stage2/flint_parse.fl`, `stage3/flint_emit.fl`).
  `tests/test_fmt.sh` had been failing since commit 809a172 landed new
  diagnostics without re-formatting. The reformat is whitespace-only (491
  lines changed in, 491 changed back) and pipeline-verified: `test_fmt.sh`
  0 failures, `test_lexdiff.sh` 112/0, `test_fixpoint.sh` 113 pass,
  `test_selfhost.sh` Phase C byte-identical with stable promotion OK.
- **README tooling section corrected against reality.** It claimed
  `--target`/`--emit-llvm` do not exist (both stale: `--target` has worked
  since its restoration, and the *output extension* — not a flag — selects
  `.ll` text / `.o` object / linked executable), and that `--test` "does
  not run tests" (it does: it runs every `test_` function and prints a
  pass/fail summary). The `--target` recipe carried the phantom
  `--emit-llvm`, now removed. The gap list states only real gaps: no
  `<subcommand>` verbs, no `--cgu`/`--no-strip`/`--emit-obj`/`--emit-header`,
  and an untested `flint-lsp` whose hover is line-based.
- **The JIT could not run programs using the utility/time layers** — it died
  with a `JIT session error: Symbols not found: [...]` fix since the runtime
  `.o` files were compiled with LLVM outline-atomics, whose `__aarch64_*`
  helper symbols exist only as local symbols inside the compiler binary the
  JIT cannot resolve. The runtime is now built with `-mno-outline-atomics`
  so every atomic op inlines; `t_util.fl` and `t_time.fl` pass as standing JIT
  smoke checks (run.sh now 25 checks).
- **Map subscript reads.** `m["k"]` desugared to an ICmp type-mismatch that
  aborted the compiler; a loop variable used as a cast subject parsed as a
  function call; `m[0]` (wrong key type) silently read a garbage byte. Map
  reads work, loop-variable casts parse, and a wrong key type is now a loud
  compile-time error with the static base type known.
- **`as` cast poison.** NaN/+-Inf and out-of-range f64→i64 casts were raw
  LLVM `fptosi` (undefined outcome as garbage values); both now panic with a
  clear message on every backend instead.
- **Scientific notation.** `1e19`, `2.5e-3` and `1E+4` did not lex at all;
  both lexers now accept exponent doubles.
- **Len() on every shape.** D2: bare `len()` on arrays/strings/maps hit
  three different divergences (D1 for-array, D3 method mangling, D4 str
  GEP); all resolved 2026-10-15 with 0 known-divergence differential.
- **Autograd and optimizers actually train.** Every backward rule was a
  pass-through that returned the seed instead of the derivative;
  `ag_sum`/`ag_mean` read f64 storage directly and segfaulted on an f32 tensor;
  `adam_step` and `sgd_step` updated nothing.
- **BLAS matmul.** `#ifdef FLINT_USE_BLAS` was defined by nothing, so the
  branch was dead and every matmul took the scalar triple loop. Replaced with a
  weak-symbol probe: link `-lopenblas` and the accelerated path is used, omit it
  and the portable loop runs.
- **`_unchecked` AEGIS accessors are no longer reachable from safe code.** They
  were registered as ordinary builtins, so any program could segfault the
  runtime with `free(a)` then `read_i64_unchecked(a, 999999)`.
- **The `AegisChecker` static pass**, lost in the 2026-09-14 wipe, and both
  static passes now run on the default LLVM path rather than only under
  `--backend qbe`.
- **Integer division and modulo by zero** were undefined behaviour; they now
  trap.
- **`isCopyType()`** counted `Str`, `Map`, `Struct` and `Enum` as Copy, so a
  moved-out binding stayed usable. Nine genuine use-after-moves in the
  self-hosted sources were exposed and fixed.
- **Print output was reordered.** `flint_println` used raw `write(2)` while
  every other print used `printf`, so labels appeared after their values.
- **The module cache key omitted the runtime**, so editing `runtime/*.c` left
  every cached module bound to the previous runtime.
- **Documentation:** `REQUIREMENTS.md` claimed `--target` and `CHANGELOG.md`
  claimed `--emit-header` and a WASM target. None was ever implemented; both
  files now say so, and README documents the eight new modules.

### Added
- Self-hosted compiler pipeline: Flint lexer/parser/emitter compile Flint
  (`stage1-3/*.fl`, `tests/test_selfhost.sh` Phase A/B/C, fixed point).
- S-expr emitter groups G0–G6 +async G7 set: multi-fn calls/defaults,
  arrays/bounds, strings/escapes, for/range, structs, enums, match,
  maps/methods, globals, compound-assign, ref/deref, slices/spread,
  payload enums, unwrap/try.
- Pipeline driver (`driver/flintc.fl`), S-expr merger (`tools/merge_sexp.fl`).
- Differential suite (`tests/test_differential.sh`) + `COMPATIBILITY.md`.
- Compile-time benchmarks (`bench/compile_time/`).
- Lambda expressions (`|params| body`): outlined to `lam.N` top-level fns,
  by-value captures, same-function calls, recursion
  (`stage3/ladder/g7e_lambda`, `g7e_recursion`).
- Production gates: `flint-fmt --check` + idempotency + parse-stability
  (`tests/test_fmt.sh`), error catalog sync (`tests/test_errors.sh`,
  `docs/errors.md` 116 messages), declaration-order tracker
  (`tests/test_decl_order.py`), fuzz corpus replay
  (`tests/test_fuzz_corpus.sh`), nightly CI (perf-ratio gate, 100-seed
  fuzz, SBOM via `tools/sbom.py`), release process (`docs/RELEASE.md`).
- P1.1 compile-time: bulk reader scanners (`flint_str_skip_ws`,
  `flint_str_word_end`) + long-string `strlen` memo — lex-size
  self-hosted total 1,687 → 349 ms (parity gate ≤ 400 ms met).
- R1 inline bounds checks: `a[i]`/`s[i]`/`a[i]=v` emit   `icmp ult` + br
  with the cold path reusing `flint_bounds_check` (panic messages
  byte-identical, negatives still trapped); O2 already deletes the check
  on provable shapes (`i % 8`). Goldens g3_arrays/g3_stridx/g7c_slice
  re-blessed for the new IR shape.
- R5 string memory: malloc-failure NULLs in `runtime.c` string builders
  (concat/substring/i64+f64_to_string/repeat/upper/lower/replace/join)
  now `flint_panic("out of memory")` instead of silent corruption
  (validation NULLs untouched); `benchmarks/strrev.fl` rebuilt on the
  linear `flint_sb_*` builder path (was O(n²) leaking concat loop —
  produced garbage lengths/NULL panics at 50K+).
- S2 sanitizers: `tests/test_sanitizers.sh` (ASan+UBSan, leaks off, in
  v1 gate + nightly CI) — first run caught a real bug:
  `flint_array_alloc` used unzeroed malloc (sieve worked by OS-page
  luck, failed deterministically under ASan fill) → now calloc.
- R3 loop counters: JIT `for`-range `i = i + 1` skips the overflow branch
  when body analysis (`bodyAssignsVar`, shadowing-aware) proves the
  counter clean (loop vars are immutable, so the proof is airtight);
  `--unsafe` parity unchanged. R2 hoist evaluated and deferred (measured
  ceiling ~5–10%; collections blocked by pre-existing D1 for-in crash).
- V1 opt-identity: `tests/test_opt_identity.sh` (O2/fast/O0/O3/unsafe
  must print identical outputs; timing lines normalized) — 11/11 agree,
  in v1 gate + CI. Also fixed: run timeouts (an unguarded run once hung
  the harness on a healthy binary under throttle).
- C1 cache correctness + JIT warm path: `~/.cache/flintc` key now covers
  link flags + std/lib search paths + per-import content hashes via a
  sidecar manifest; codegen bumps require a `flintc-vNNN` salt
  (`docs/RELEASE.md` checklist). RunMode reuses post-opt bitcode on
  repeat JIT runs; import edits (42→77) no longer silently reuse stale
  modules.

### Fixed
- C++ divergences D1–D4 logged (for-array segfault, `len()` rejection,
  method mangling, string-index crash); self-hosted correct throughout.
- Self-host blockers: `e_infer_init`/`e_skip_balanced` miscounted parens
  inside S-expr string content (broke on `"("` decl inits); merger
  splitters ws-skipped around string lengths (fused forms, dropped
  closers on leading-space content).

## [0.22.0] — 2026-09-06

### Added
- Channels (`flint_chan_*`), tutorial, registry, tooling, Windows
  `hello.exe`, lexer/parser/diagnostics hardening.

### Not in this release
- `--emit-header` and a WASM target were listed here previously. Neither was
  ever implemented; both are listed in README.md under "Not implemented".

See `memory.md` Phase History for the full 0.9–0.22 record.

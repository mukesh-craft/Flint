# Changelog

All notable changes to Flint are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.0.0/); versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0/).

## [Unreleased]

### Added
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

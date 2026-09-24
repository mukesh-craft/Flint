# Changelog

All notable changes to Flint are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.0.0/); versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0/).

## [Unreleased]

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
- Channels, `--emit-header`, WASM target, tutorial, registry, tooling,
  Windows `hello.exe`, lexer/parser/diagnostics hardening.

See `memory.md` Phase History for the full 0.9–0.22 record.

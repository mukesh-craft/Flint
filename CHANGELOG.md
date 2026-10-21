# Changelog

All notable changes to Flint are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.0.0/); versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0/).

## [Unreleased]

### Changed
- **The self-host pipeline files are renamed to their stage roles**:
  `stage1/flint_lex.fl` → `stage1/lexer.fl`, `stage2/flint_parse.fl` →
  `stage2/parser.fl`, `stage3/flint_emit.fl` → `stage3/emitter.fl`. All
  references are updated (imports, gates, CI, SBOM oracles, docs, driver
  usage strings). Entries below predate the rename and use the old file
  names.
- **The Python language server is removed** (`flint-lsp`, 522 lines), replaced
  by the Rust server in `lsp/` described below. It had no tests, wrote its
  scratch file to a hardcoded `/tmp` (absent on Android, where Flint targets),
  shipped a keyword list missing `continue`/`and`/`or`/`not`, and offered no
  `references` request. README, REQUIREMENTS and ROADMAP now describe the Rust
  server, and the README gap list no longer calls the server untested.

### Added
- **Argon2id password hashing** (`runtime/rust/src/argon2.rs`), the OWASP
  recommendation — the crypto module had HMAC/SHA/AES but nothing suitable for
  stored credentials, and hashing a password with a *fast* primitive is the
  mistake this closes. `flint_argon2_hash` defaults to 19 MiB / 2 iterations
  (OWASP 2024 minimum); `flint_argon2_hash_params` takes explicit costs for
  tests and future tuning. Returns a **PHC string** carrying the algorithm,
  version and parameters, so the cost can be raised later without invalidating
  stored hashes — verified by a test that hashes with 64 KiB, then 128 KiB, and
  checks both still verify. `flint_argon2_verify` returns 1 (match), 0 (wrong
  password) or **-1 (stored hash unusable)**: a login path must be able to tell
  those apart. Empty passwords are refused. 9 crate tests.
- **An arena allocator for the stdlib modules** (`runtime/rust/src/arena.rs`).
  Codecs need `alloc`, and these modules deliberately have no allocator — that
  is what keeps them JIT-loadable on Android and free of `std` symbols. Bounded
  bump allocator: `dealloc` is a no-op, memory is reclaimed by `reset()` at the
  start of each entry point, exhaustion returns null so Rust aborts loudly
  rather than handing back a short buffer, and a spin lock keeps two threads
  from claiming the same region. Two bugs its own tests caught: alignment was
  computed on the bump *offset* rather than the absolute address (the arena's
  base is only 8-byte aligned), and the test for that was missing.
- **Portable system information** (`runtime/rust/src/system.rs`) for Linux,
  macOS and Windows: platform tag, CPU count, memory total/available, uptime, OS
  version, temp dir. Machine facts only — camera, sensors, contacts and
  notifications are platform APIs with permission models, not libraries, so
  claiming them would be a promise this runtime cannot keep cross-platform.
  Measured on the way: `/proc/uptime` and `/proc/meminfo` are permission-denied
  on modern Android, and `sysconf` indices are libc-specific (bionic does not
  implement `_SC_NPROCESSORS_ONLN`, verified), so the contract the tests enforce
  is "a plausible value, or -1 with a reason" — never a plausible wrong number.
- **zstd decompression** (`runtime/rust/src/compress.rs`) via `ruzstd`, no_std
  with `default-features = false`. Output is refused, never truncated, above
  16 MiB.
- **Encoders and utilities** (`runtime/rust/src/encoding.rs`): CRC32C, base32,
  base58, percent-encode/decode, UUID v4 and v7. Every one pinned to published
  vectors (RFC 3720 CRC self-tests, the RFC 4648 base32 table, RFC 3986 rules),
  because an encoder that is subtly wrong is worse than no encoder.
- **String escapes and interpolation, documented and gated.** Flint strings
  interpolate `{identifier}` and `{{`/`}}` are the escapes for a literal brace.
  That interaction is the most common surprise in the language (it surprised
  this project twice), so README now states both rules and
  `tests/t_strings.fl` pins them (smoke gate 27 -> 29 checks).
- **SQLite FTS5 (full-text search), proven end to end.** Termux's libsqlite3
  is built with FTS5, rtree, json1 and the deterministic math functions, so
  this needed **no new code at all** — the existing Rust sqlite wrapper already
  speaks them. `tests/t_fts5.fl` proves what a language can actually reach:
  `CREATE VIRTUAL TABLE ... USING fts5`, `MATCH` with ranking, a two-term query,
  a zero-result query (0 rows, not an error), `snippet()` with highlight
  markers, and `sqrt`. The distinction matters: "the library supports it" and
  "a Flint program can use it" are different claims, and only the second one is
  a feature.
- **HTTPS works.** `flint_http_get` / `flint_http_post` previously *refused*
  `https://` outright (`parse_http_url` returned false), which made every modern
  API unreachable from Flint — the single largest stdlib gap in the audit.
  Plain HTTP still uses the raw socket path; TLS is handed to the platform's
  libcurl, which already links the system TLS stack.
  Deliberate choices, stated because they are the ones people argue about:
  certificate verification is **on and cannot be disabled** (a runtime that
  offers an "insecure" switch teaches every user to turn it off), response
  bodies are capped at 8 MiB and an oversized response is *refused* rather than
  silently truncated, timeouts are mandatory, redirects are capped at 5, and
  libcurl is reached through **weak symbols** — the same pattern `flint_blas.c`
  uses for OpenBLAS — so a device without libcurl still builds and reports a
  clear error instead of failing the build. New: `flint_http_status()` returns
  the status of the last request, so a caller can tell 404 from 200.
  Gated by `tests/test_https.sh` (4 checks), whose offline-safe checks use
  RFC 2606's `.invalid` TLD so the gate never depends on a network.
- **`runtime/rust/src/sqlite.rs`: SQLite, as a Rust wrapper over the system
  libsqlite3.** SQLite is a C engine and there is no production-grade pure-Rust
  one, so the choice was "bind it" or "ship nothing"; this binds it and puts
  every safety property available on the boundary — handles are validated,
  sqlite's pointers are copied into a module-owned arena so no C pointer ever
  escapes to Flint, text is NUL-checked, and NULL cells come back as empty
  strings rather than garbage.
  Surface: `flint_sqlite_open` / `close` / `exec` / `query` / `rows` / `cols` /
  `get` / `error` / `version`, with up to 8 concurrent connections.
- **The compiler can now publish a shared library's symbols to the JIT**
  (`publishJITLibrary` in src/main.cpp). ORC resolves objects and libc, but not
  an arbitrary dlopen'd library, so a module wrapping libsqlite3 or libcurl found
  its entry points undefined and ORC reported the failure misleadingly by naming
  the *entry symbol* ("Symbols not found: [ main ]"). Publishing is explicit and
  narrow: only the listed symbol names become visible, so a typo in a Flint
  extern still fails loudly instead of resolving to something unrelated.
  The AOT link line gains the same libraries by absolute path.
- **`runtime/rust/src/crypto.rs`: SHA-256, HMAC-SHA256, AES-256-CTR and
  AES-256-GCM, in pure Rust.** Built on the RustCrypto primitives (`sha2`,
  `hmac`, `aes`, `aes-gcm`, `ctr`) with `default-features = false`, so the
  module stays `no_std` and keeps the properties the CSV module established: no
  `std` symbols in the shipped object, loadable by the JIT, and able to coexist
  with a user's own Rust library in one binary. Nothing hand-rolled: HMAC, the
  AES modes and the GHASH tag come from audited implementations, because
  re-implementing them is exactly the cleverness that produces silent,
  catastrophic bugs.
  Flint surface: `flint_sha256_hex`, `flint_hmac_sha256_hex`,
  `flint_aes256_ctr_crypt_hex`, `flint_aes256_gcm_encrypt_hex`,
  `flint_aes256_gcm_decrypt_hex`, `flint_crypto_last`, `flint_crypto_error`,
  `flint_crypto_out_bytes`.
  Correctness is pinned to independent references, not to itself: the SHA-256
  vectors are FIPS 180-4, the HMAC vector is RFC 4231 case 2, and the AES-CTR
  expectation was generated with **OpenSSL 3.6.3** on this device rather than
  from this code. GCM authentication failure (wrong key, nonce, AAD or tag)
  returns -1 with a message instead of garbage output.
  Two API constraints are documented because they bite: results live in one
  shared buffer that ANY next crypto call overwrites (copy before the second
  use — the gate's own fixture hit this), and key/nonce sizes are validated, so
  a 2-byte key fails loudly instead of silently using the wrong bytes.
  11 crate tests, 26 crate tests in total, and the stdlib gate now exercises
  crypto on BOTH the JIT and AOT paths.
- **`runtime/rust/`: the first Rust standard-library module (CSV), working
  through both execution paths.** Flint's stdlib now has a Rust module and the
  pipeline it needs: `cargo build` -> staticlib -> `ld -r` into `flint_std.o` ->
  loaded by the JIT and named on the AOT link line. `flint_csv_parse`,
  `flint_csv_rows`, `flint_csv_cols`, `flint_csv_get`, `flint_csv_free`,
  `flint_csv_error` and `flint_csv_arena_bytes` are registered builtins; the
  dialect is RFC 4180 with `#` comment lines, blank-line skipping, CRLF, and
  doubled-quote escapes.
  Two constraints were discovered by experiment and are now enforced by the
  gate rather than discovered by users:
  - **The module is `#![no_std]`.** Rust's `std` resolves thread-local handles
    through libc, and a `std` object loaded into JIT memory aborts the process
    on Android with `increment_dso_handle_reference_counter: Couldn't find
    soinfo by dso_handle`. `no_std` also means the flattened object carries no
    `std` symbols, so a user's own Rust static library can link in the same
    binary -- two `std` copies collide (`rust_eh_personality`,
    `std::panicking::EMPTY_PANIC`). The cost is a fixed 1 MiB parse arena
    instead of a heap; overflow is reported as an error, never truncated.
  - **The archive is flattened with seeded `-u`, not `--whole-archive`.**
    Whole-archive pulls every `compiler_builtins` member (hundreds of float and
    atomic intrinsics the JIT cannot resolve); seeded flattening produces an
    11.6 KB object with exactly three undefined symbols (`abort`, `memcpy`,
    `memset`), all libc.
  Verified by `tests/test_rust_stdlib.sh` (7 checks): the crate's 15 unit
  tests, JIT and AOT runs of `tests/t_csv.fl` including quoted fields and the
  malformed-input error path, byte-identical JIT/AOT output, a `no_std` symbol
  assertion, and that a typo'd module symbol still fails loudly.
- **`lsp/`: the Flint language server, rewritten in Rust, and gated.** One
  static binary whose only dependency is `serde_json`. Diagnostics come from the
  real `flintc` (never a re-implemented parser), edits are debounced 300 ms on
  a worker thread, a compiler that cannot be executed reports one explicit
  diagnostic instead of a false all-clear, scratch files honour `$TMPDIR` /
  `$PREFIX/tmp` and are always removed, and stdout carries the protocol and
  nothing else. Capabilities: diagnostics, completion (30 keywords + literals
  + in-buffer declarations + all 515 registered builtins), hover with doc
  comment and signature, go-to-definition, find-references, document outline;
  unknown methods answer `-32601` instead of hanging the client.
  Verified by `tests/test_lsp.sh`: 21 `cargo test` unit tests (framing incl.
  split/malformed frames and UTF-8 byte lengths, diagnostics parsing against
  real compiler output, analyzer behavior) + 31 protocol checks driving the
  real binary over pipes + two drift gates (server keywords must equal the
  compiler lexer's 30; server builtins must equal the compiler's 515
  registrations).
- **`editors/vscode/`: VS Code extension for Flint.** Manifest, TextMate
  grammar (26 patterns: `#` and `//` comments, strings with escapes, ints /
  floats / scientific notation, control/declaration/type/operator keywords,
  builtin and tensor-function recognition, calls, operators, punctuation),
  language configuration (brackets, auto-closing pairs, `#` comment
  continuation, region folding, word pattern), and a dependency-free stdio
  client. Grammar keywords are generated from the compiler lexer;
  `tools/check_vscode_extension.py` fails on drift in either direction so the
  highlighter cannot quietly fall behind the language. Settings:
  `flint.lsp.serverPath`, `flint.lsp.arguments`, `flint.lsp.trace`.
- **`tools/gen_lsp_builtins.py`**: regenerates the server's builtin table from
  `src/main.cpp`; `--check` fails when it is stale.
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

# Flint Platform Tier Matrix (Phase J C2)

Tier rules are adapted from the Rust target-tier policy (see memory.md
"SCORE IMPROVEMENT PLAN / Portability" research notes):

- **Tier 1 — guaranteed to work.** CI builds AND the full gate list passes
  on the target; host tools run natively; full standard library (no stubbed
  modules); designated path for bug reports. Promotion into T1 requires a
  recorded full-gate green run on that target.
- **Tier 2 — guaranteed to build.** CI builds the compiler and runtime for
  the target; tests may or may not run. Known gaps are listed below, not
  hidden.
- **Tier 3 — may or may not build.** No CI guarantee; cross-compiled
  artifacts only, or blocked on a named prerequisite. Promotion requires:
  no stubbed-out std for the target, cross-compilable without the target
  as host, and target-specific docs.

## Matrix (2026-10-01, Termux AArch64 device)

| Tier | Platform | Status | Evidence / gap |
|------|----------|--------|----------------|
| 1 | Android ARM64 (Termux) | ✅ primary dev platform | All gates run here (`tests/run.sh`, tutorial, ladder, differential). LLVM 21.1.8, clang, `aarch64-unknown-linux-android24` host triple (`src/main.cpp:3736`). |
| 1 | Linux x86_64 | ✅ CI-tested | `.github/workflows` ubuntu-24.04 job (full suite). Cannot be re-confirmed from this device — last green is the CI record. |
| 1 | Linux ARM64 | ✅ CI-tested | Same CI record as x86_64 (ubuntu-arm runner). Same caveat as above. |
| 2 | macOS (Apple silicon + Intel) | 🟡 builds via CI | CI `macos-15` job builds; **promotion to T1 needs a full-gate run there** (smoke/tutorial/lexdiff/parse/emit/differential), not just a build. Not runnable from this device. Deps: Xcode CLT + `brew install llvm` (`REQUIREMENTS.md`). |
| 3 | Windows x86_64 (MinGW / LLVM-MinGW) | 🟡 cross-link proven | v0.0.20: all 8 runtime files + `src/flint_os.cpp` compile for x86_64-windows; a real `hello.exe` (COFF-x86-64) was cross-linked from ARM. `flint_regex_*` are err-flagged stubs (no POSIX engine) — the named T3 gap. Promotion to T2 needs a fully-green native CI run. Link needs `-lws2_32 -lpthread` (added automatically for Windows targets). |
| 3 | Windows (MSVC native) | 📋 blocked | Needs the native thread shim (`PORT` notes in code); pthreads path only covers MinGW. |
| 3 | WASM/WASI (`wasm32-unknown-unknown` / `wasm32-wasi`) | 🟡 emit works, execution blocked | `--target <triple>` now exists (2026-10-17): modules emit with the target's canonical triple + datalayout (`tests/test_target.sh`), including real WASM object files. Execution additionally needs: a WASI sysroot + libc + crt1.o (absent on device, policy: never vendor SDKs), `_start` linkage, and a runner (`node`/`wasmtime`). See "WASI hello probe". |

The human-readable install table per OS lives in `REQUIREMENTS.md`
("Platform Support", "Required Packages"); this file is the normative
tier definition, that file is the install how-to.

## Tier promotions (all require off-device CI — cannot run here)

1. **macOS T2→T1:** run the FULL gate list on macOS hardware
   (smoke/tutorial/lexdiff/parse-gate/fixpoint/ladder/differential),
   record the green run, then flip the row above.
2. **MinGW T3→T2:** confirm the native `windows-2025` CI job is fully
   green (build + smoke + registry at minimum), publish the result;
   keep MSVC at T3 until the thread shim lands, then promote on the
   same rule.
3. **WASM T3→T2:** `--target` shipped (2026-10-17, emit stage, canonical
   triple+datalayout verified; objects emit). Still needed: `_start` +
   WASI sysroot/libc linkage, then one WASI hello executing end-to-end
   under a runner, plus `--target`
   execution tests per triple. The exact missing pieces and the
   commands that would run elsewhere are recorded below.

## Hermetic temporary paths (C2)

All harness scratch directories honor `$TMPDIR` with a byte-identical
fallback to the historical Termux prefix when it is unset (Termux
often leaves it unset):

```sh
_TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
TMPD="$_TMPBASE/slipstream/<gate>"
```

- Applied to: `bench/compile_time/bench.sh`, `tests/test_selfhost.sh`
  (`OUT`, `STABLE_DIR`), and `tests/test_{differential,driver,emit,
  emit_stage3,fixpoint,merge,fmt,fuzz_corpus,sanitizers,opt_identity}.sh`.
- `SELF_DIR` keeps its existing override semantics (`${SELF_DIR:-…}`
  where it had them); hard assignments stay hard assignments — only
  the base changed. Unset `$TMPDIR` reproduces the old paths exactly.
- Already hermetic, untouched: `tests/test_cache.sh`,
  `tests/test_registry.sh`, `tests/test_lexdiff.sh`,
  `tests/test_parse.sh` (all use `mktemp -d`, which respects `$TMPDIR`
  by construction).
- C++ side: `flint_temp_dir()` (`runtime/runtime.c:1179-1185`) already
  implements `TMPDIR → TEMP → /tmp`. The compiler's other hardcoded
  paths are NOT tmp and were deliberately left alone: `sysRoot()`'s
  `/data/data/com.termux/files/usr` fallback (`src/main.cpp:177-181`)
  is a `$PREFIX` fallback (sysroot, not scratch), and `ModuleCache`
  (`src/main.cpp:582-584`) is a persistent `$HOME/.cache/flintc`
  cache (not temp). Moving either would change default behavior.
- Standing rule (unchanged): default scratch dirs are still shared
  (`slipstream/<gate>`), so long gates still run **solo, never in
  parallel**. A custom `$TMPDIR` per run isolates scratch dirs, but
  `$HOME/.cache/flintc` is still shared — after any killed run,
  `rm -rf ~/.cache/flintc` before retrying.

## WASI hello probe (2026-10-01 — execution `[Unverified]`; 2026-10-17 update below)

Attempted: emit a `print("hello from wasi")` program and link it with
`_start` for a WASI target. Result: stopped at the FIRST missing
piece — the compiler cannot target WASM at all:

1. No triple control: `flintc` accepts no `--target`/`--emit-obj`
   flag (see `src/main.cpp:8600-8750` argv parsing — no such branch;
   `README.md:258-259` confirms these flags do not exist), and the
   module triple is hardcoded (`src/main.cpp:3736`
   `aarch64-unknown-linux-android24`).
2. Present on this device (so NOT the blocker): `wasm-ld`
   (`$PREFIX/bin/wasm-ld` → `lld`), `clang 21.1.8` with registered
   `wasm32`/`wasm64` targets (`llc --version`), and `node v24.18.0`
   (a potential WASI runner once an artifact exists — irrelevant
   until step 1 links).
3. Missing on this device (blocks the NEXT step after triple
   control): no WASI sysroot. Forcing the issue proves it —
   `clang --target=wasm32-wasi wasi_probe.ll` fails with exactly:
   `cannot open crt1.o`, `unable to find library -lc`, and
   `cannot open .../lib/wasm32-unknown-wasi/libclang_rt.builtins.a`.
   No `wasmtime`/`wasmer` either (`command -v` rc=1 for both).

**2026-10-17 — piece 1 CLOSED (`--target` shipped, emit stage).**
`flintc file.fl --target <triple> -o out.ll|out.o` now emits
host-independent IR with the target's canonical triple + datalayout
(fetched from a real TargetMachine, not hand-written strings).
Verified byte-identical to clang's own canonical datalayouts for
x86_64-pc-linux-gnu and wasm32-unknown-wasi, and the wasm .o carries
real WASM object magic (`006130a6d01...` checked in
`tests/test_target.sh`, 9 checks). `--target` with no `-o` refuses
loudly: ORC JIT is host-triple only. Piece 3 remains the blocker for
EXECUTION (no WASI sysroot/crt1.o/-lc/runner here); everything in
this section above piece 1 is historical record.

Commands that would run elsewhere (once triple control + sysroot +
runner exist — do NOT run here, prerequisites absent):

```sh
# 1. emit host-independent IR, retarget, and link against the WASI sysroot
./flintc hello_wasi.fl --emit-llvm -o hello_wasi.ll   # needs --target first
clang --target=wasm32-wasi --sysroot="$WASI_SYSROOT" \
  hello_wasi.ll runtime/runtime.c -o hello_wasi.wasm  # needs _start + wasi libc
# 2. execute under a WASI runner
wasmtime hello_wasi.wasm                              # expect: hello from wasi
```

Note: `runtime/runtime.c` assumes POSIX (`unistd.h`, sockets,
`popen`, threads) and will need a WASI-port pass (fd-write
backing for `print`, no `fork`/`exec`) before step 1 can link —
that port is follow-up work, not part of this probe.

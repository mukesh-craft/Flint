# STATE — Flint compiler work log + pick-up guide

**Read this file first. It is self-contained: everything needed to verify the
current state and continue without re-deriving context.**

GOAL: finish the remaining tracked work items of the audit (memory.md §3 bug
table, §5 milestones), one gated commit at a time, with no silent answers.

STATUS (2026-10-19, commit `3f062e4`): milestone wave M3–M6 landed; M7 deferred
by maintainer; B-wave P0-NEW-1/2/3 landed; M2 residue part 1 landed
(generic inference for cast + nested-call arguments). All gates green on the
fresh build.

POLICY: STATE.md is the durable ledger — update it IN PLACE after each
meaningful step. Offload raw logs to `$TMPDIR/opencode/`. Keep this file
under ~8K tokens; rewrite, don't append forever.

CONSTRAINTS (from the user, verbatim in spirit): RAM 3.6 GB (one build at a
time); Rust wherever it pays; Linux/Windows/macOS support; NEVER break the
token contract — `test_lexdiff.sh` / `test_parse_gate.sh` / `test_fixpoint.sh`
green is the proof; the two emit paths (Codegen AST + single-pass Parser) must
BOTH receive every fix (the P0-NEW-1 collapse now enforces this with shared
cores).

---

## 1. HOW TO BUILD AND VERIFY (this device, Termux aarch64)

```bash
cd /data/data/com.termux/files/home/Flint

# 1. syntax check (fast, ~3-8 min; the full compile is the slow part)
timeout 480 clang++ -std=c++17 -fsyntax-only -I$(llvm-config --includedir) src/main.cpp

# 2. full build (12-15 min) — ALWAYS detached; a tool timeout kills the
#    process group and orphans the compile
(setsid nohup bash build.sh > build.log 2>&1 < /dev/null &)
#    poll:  grep -c "error:" build.log ; grep -n "Build complete" build.log
#            ls -la flintc      # mtime must be NEWER than your last edit

# 3. gates (each is a separate script; expect minutes each)
bash tests/run.sh                  # smoke (now 36 checks)
bash tests/test_lexdiff.sh         # 131
bash tests/test_parse_gate.sh      # 132
bash tests/test_fixpoint.sh        # 132 (self-host PARSER round-trip)
bash tests/test_errors.sh          # 128/128 catalog
bash tests/test_differential.sh    # 11/11 — NEEDS the self-host binaries (see below)
bash tests/test_generics.sh        # 7/7
bash tests/test_no_hang.sh         # 88/88
```

### Traps that WILL cost you time (all hit in this session)
- **Stale `flintc`**: editing `src/main.cpp` does NOT change `flintc` until a
  build finishes. Rebuild before trusting any test result.
- **`/tmp` is not writable** on Termux. Use
  `T=/data/data/com.termux/files/usr/tmp/opencode`.
- **Pipe eats the exit code**: `./flintc f.fl | head; echo $?` reports `head`'s
  code. Verify with `>/dev/null 2>&1` and no pipe.
- **`test_differential.sh` needs the self-host binaries**
  (`$TMPDIR/slipstream/self/{lex,parse,emit}.0`). If they are missing the gate
  reports `1 passed, 10 failed` with `self binaries (build via
  tests/test_selfhost.sh A ...)` — that is a MISSING ARTIFACT, not a code
  divergence. Rebuild with `bash tests/test_selfhost.sh A` (~4 min, detached).
  Termux may clean `$TMPDIR` between sessions, so expect to rebuild it.
- **Tool timeouts kill process groups**: never run a >8 min command in the
  foreground; use the detached pattern above and poll.
- **`pkill -f` can match your own shell**: use the bracket trick
  (`pkill -f "test_selfhos[t]"`).
- **Never `open('w')` a source file without writing to it** (once zeroed
  `src/main.cpp` that way). Use the `write`/`edit` tools.

### Verifying a compiler change, honestly
Map EVERY printed line of a fixture to its assertion, one by one. Eyeballing
"1s and 0s" once produced a false "all 16 verified" claim that shipped a real
bug (the enum accumulator, see HISTORY). Per-case probes (`$T/probe.fl`) beat
a single fixture diff.

---

## 2. OPEN ITEMS — what is actually left (status verified 2026-10-19)

| # | Item | Status | Where |
|---|---|---|---|
| 1 | **M2b — explicit type arguments `f::<T>(...)`** | **DONE** (commit 3d3d914). `::` is two COLON tokens, so the lexer is untouched; explicit args override inference and feed the same instantiation path. Fixture in tests_manual/ (the self-host parser does not implement the syntax). | `src/main.cpp` parser + `Codegen::emitCall` + `Parser::emitDirectCall` |
| 2 | **`let h = inc` — named function as a value** | **DONE** (commit fd8cf09). A named function joins the lambda protocol through a generated THUNK with the same fn(env, args...) shape, so the call site needs no discriminator. Frame-bound like a lambda (escape rules apply). A generic function as a value is refused by name. | `src/main.cpp` `Codegen::namedFnThunk` / `namedFnValue` |
| 3 | **M2c — delete the remaining silent i64 defaults** | **DONE** (commit 922d03a). The generic argument resolver is strict: an un-inferrable argument is reported by name ("cannot infer type parameter 'T' for 'idc'"). Float arithmetic and float fields infer f64; a call to a user function uses its declared return type. | `src/main.cpp` `Codegen::inferGenericArgType` |
| 4 | **B8 — type-level generic enums (`Option<T>`)** | OPEN by design. `Option.Some(i64)` is a fixed i64 payload; ROADMAP.md:466 overclaims. Needs a maintainer decision (it changes the type system + ABI), not a unilateral change. | memory.md:3628 |
| 9 | **The self-host feature gap (D5)** | RECORDED, not closed. The C++ compiler now has `trait`, generics, `f::<T>(...)`, enum equality and named-function values; the self-hosted pipeline (stage2/stage3) has NONE of them. The differential gate is green because it does not cover them. D5 in COMPATIBILITY.md lists the gap and the policy. Porting them is a project of its own (11.7k lines of Flint). | `COMPATIBILITY.md` D5 |
| 5 | **M2d — arrays as generic arguments** | **DONE** (the AST path already inferred `array(i64)`; the emit path now does too — commit 361d810). `idc([1, 2])` instantiates T = array(i64) and `z[1]` reads 2. (An earlier probe of mine was wrong: `.length` is not a field in this dialect.) | `src/main.cpp` `exprFlintType` + `Parser::emitDirectCall` inference |
| 6 | **P4 ship list** | MOSTLY DONE: error-code docs shipped (4214082 — `docs/errors-cpp.md`, 135 diagnostics, with a two-way sync gate in CI); the semver freeze and the SBOM generator already existed; signed tags remain (the repo has 0 tags and this device has no GPG key — the procedure is in `docs/RELEASE.md`). | memory.md:1604, 1624 · `docs/RELEASE.md` |
| 7 | **Environment-bound (cannot finish on this device)** | QBE backend needs an external `qbe` binary (path unverified, exit 127); Windows/macOS real runs + CI never executed from here; WASM objects are valid but there is no WASI execution; nightly/coverage-guided fuzz runs are not wired to any CI runner (the corpus + `tests/test_fuzz_corpus.sh` exist and pass). | memory.md:946, 1602 |
| 8 | **M7 `[T]` containers** | DEFERRED by maintainer decision (2026-10-19). Needs the Rust `{length, elem_size, ptr}` representation (§4) plus an explicit ABI decision. Do NOT start without that decision. | memory.md:3711, 3934 |

**New finding (2026-10-20): the C++ "single-pass emit path" is UNREACHABLE.**
`Parser::emitXxx` is entered only from `Codegen::emitFunction` for a function
with no AST body, and in a normal compile every function has one. Measured
with the `FLINT_TRACE_EMITPATH=1` probe (added in commit 361d810): 0 hits over
all 137 corpus `.fl` files. Consequences:
- Fixes applied to that path are harmless but not load-bearing. The ledger
  doctrine "every fix must land in both C++ paths" overstates the risk.
- The REAL second implementation is the SELF-HOST Flint pipeline
  (`stage2/flint_parse.fl` + `stage3/flint_emit.fl`) — an independent
  implementation the differential gate compares against. It has NO traits,
  NO generics, and no `::<` syntax, so recent C++ features are not yet
  available to the self-hosted toolchain. That gap is the one that matters.
- Measurement command: `FLINT_TRACE_EMITPATH=1 ./flintc <file>.fl` prints
  `[emit-path] function <name>` if the path is ever entered.

**Closed since the audit (do not re-open):**
- N1 (payload-enum `match` in a non-`main` function hung the compiler) — FIXED
  earlier; `tests/t_payload_match.fl` is the regression fixture and passes
  (7, 0, 6, 9, 12, 24, 0, 10). The audit note at memory.md:3793 ("not yet
  root-caused") is STALE — the root cause was the single-binding arm parser
  plus `consume()` not advancing on error, so the arm loop could not progress.
- D1–D4 (for-array segfault, `s[1]` crash, …) — FIXED (memory.md:3145).
- `parallel for`, `--test` dead, Python JIT symbols, strrev SIGABRT, missing
  LICENSE — all closed in earlier sessions.
- B1 (float tensor `a[0]` garbage) — closed by the narrow refusal
  (P0-NEW-3, `67edab9`); the thorough fix needs M7.
- B2 (f64 generic leaking raw LLVM IR), B4 (lambda as value), B5 (trait
  keyword), B6 (parallel for), B7 (specialize latent bugs) — closed.
- B9 (array API pairing) — mitigated by the shared call guard; the API shape
  itself is unchanged (documented, not a bug).

### Recommended order
9 (port the new features to the self-host pipeline, or narrow the gap) → 4
and 7 (decisions/other machines) → 8 (only after a maintainer ABI decision).
Items 1, 2, 3, 5 and most of 6 are DONE (commits 3d3d914, fd8cf09,
361d810, 922d03a, 4214082).

---

## 3. NEXT (concrete)

1. **The self-host pipeline gap (D5)** — the biggest remaining item. The C++
   compiler accepts programs the self-hosted pipeline cannot parse or emit:
   `trait`, generics, `f::<T>(...)`, enum equality, a named function as a
   value. Either port them to stage2/stage3, or decide the pipeline is a
   bootstrap-only artifact and say so plainly in README/COMPATIBILITY.
   Start by measuring: which of the five can the self-host pipeline
   actually express? (The emitter has some lambda shapes; the parser has no
   `typeParams` at all.)
2. **B8 (type-level generic enums)** — needs a maintainer decision; the
   ROADMAP claims `Option<T>` exists and it does not.
3. **Signed release tags** — the procedure is documented; the device has no
   GPG key, so the first signed tag waits for a machine that has one.

---

## 4. KEY FILES / ANCHORS

- `src/main.cpp` — the whole compiler (~12,900 lines).
  - Shared emit cores (P0-NEW-1): `emitCompareValues`, `createCallChecked`,
    `emitPrintCall`, `emitArrayGet/Set/Data/GetPtr/SetPtrCore`, `emitEnumEqual`
    (P0-NEW-2), `inferGenericArgType` (M2 residue).
  - `exprFlintType` (~:5836) — the inference walker; `Parser::inferCallType`
    (~:2229) — builtin return types only.
  - `emitCall` (~:8380) — lambda rewrite, fn-value call, generic dispatch.
  - `Parser::emitBinaryOpEmit` / `Parser::emitDirectCall` — the emit-mode
    path; both route to the shared cores now.
- `tests/run.sh` — smoke + `check_fail` cases; `tests/test_*.sh` — the gate
  suite; `tests/t_generic_infer.fl` — M2-residue regression fixture.
- `memory.md` — the audit: §3 bug table (B1–B9), §4 Rust candidates, §5
  milestones, §6 rejected approaches, §7 risks, §8 open decisions, §9 traps.
- `COMPATIBILITY.md` — C++ vs self-host divergences (D1–D4 all fixed).
- `stage2/flint_parse.fl`, `stage3/flint_emit.fl` — the self-hosted pipeline.
  NOTE: it implements **no generics at all** (no `typeParams`), so generic
  programs are C++-compiler-only and the differential ladder cannot cover them.

## 5. DEAD ENDS (do not retry)

- Element-wise / byte-wise enum payload comparison → wrong answers (garbage
  bytes past the variant payload). Solved instead by per-variant FIELD
  comparison (`emitEnumEqual`), not by byte comparison.
- Byte-comparing an enum payload array: bytes a variant never writes are stack
  garbage. Never do this.
- `hasStructName()` does not exist on `llvm::StructType` in this LLVM
  version — use `isLiteral()` to distinguish identified (named) structs.
- The enum accumulator must OR variant bodies into the result slot, never AND
  them against a zero-initialized slot (that bug shipped once).
- Re-running `test_differential.sh` without the self-host binaries produces
  fake failures — build phase A first.
- Replacing a whole function by brace-matching text surgery: the M5 attempt
  landed wrong. Read exact function bounds, replace atomically, and
  syntax-check before building.

---

# HISTORY (milestone + session log, kept for traceability)

M3 DONE (all gates green). Replayed after I zeroed src/main.cpp with a bare
python open('w') that was never written to — rule: NEVER call open('w')
without .write(); use the write tool or the edit tool for source changes.

NEXT:
2. M4: Symbol already carries lambda fields (isLambda, lambdaFn, captures)
   — find LambdaExprAST emit path; fn type in parseType; indirect call via
   CreateCall(fnType, loadedPtr, args) with closure = {fnptr, captures...}.
3. M5: atomic replace of parseParallelForStmtEmit both paths, migrate t_flow.fl
4. M6: cascade fix
5. M7: last, only if budget allows

NEXT:
M4 DONE (all gates green, committed).
- fn types: TypeKind::Func; funcParams vector<Type>, funcRet shared_ptr<Type>
  (a struct can't hold a Type member of itself — direct member was ill-formed,
  first build broke with "field has incomplete type" + inheritance cascades).
- Uniform closure protocol: outlined lambda = fn(env, userargs...) -> ret;
  env box = {fnptr, caps...} (fnptr slot written explicitly, caps by-value).
  ALL calls indirect: load field 0 as ptr, CreateCall(fnTy, loadedPtr,
  {box, args...}). fn-typed params/bindings work through the same path.
- emitExpr(Variable) on a lambda binding now yields the box pointer (fn value);
  inline lambda (call-site arg) materializes via emitLambdaDef with a synthetic
  "__inline_lam_N" binding, then yields its box.
- Escape rule (hard error): fn value in return position, or stored into a
  global. Real adapters for named functions as values (let h = inc) are future
  work; that case now fails clean: "function 'inc' cannot be used as a value
  here; bind a lambda or pass it in a direct call".
- emitVarDecl: lambda-value binding copies the source's fn type (annotation
  mismatch = error); prevents the i64 ladder demoting a ptr init to str.
- emitCall: fn-typed callee branch BEFORE the generic/direct lookup (arity +
  param coercion + arg type mismatch); lambda args type-check against the
  callee's declared fn params (functionDefs).
- Gates: tests_manual/fncall.fl (apply(|x| x+1, 5) == 6 in run), fnescape.fl
  (compile error), fnglobal.fl (exit=2), smokes: 29/29; suites differential,
  generics, no_hang, errors, parse_gate, lexdiff, fixpoint, python, ai,
  rust_stdlib all rc=0; selfhost fixed point stable.
- Known cosmetic: after the escape guard error, body emission continues and a
  second verifier line can print (M6 will stop cascades).

NEXT:
2. M5: atomic replace of parseParallelForStmtEmit both paths (AST + emit);
   migration error message per memory.md B6 ("was removed: it could not
   capture variables and had no result channel. Use a plain 'for' loop, or
   move the work into a Rust runtime function such as
   flint_parallel_reduce"); keep KW_PARALLEL + runtime symbol; migrate
   tests/t_flow.fl; test_errors.sh asserts the message.
3. M6: cascade fix
4. M7: last, only if budget allows

NEXT:
M5 DONE (committed).
- 'parallel for' syntax REPLACED at both paths (parseParallelForStmt and
  parseParallelForStmtEmit) with the migration error per memory.md B6; exact
  spans found by index+anchor, whole-body replacement (the last-session trap):
  not one line spliced.
- KW_PARALLEL token and flint_parallel_for runtime symbol kept (ABI).
- tests/t_flow.fl migrated to a plain `for i in range(5)` printing 100..104
  (same expectations, no run.sh change needed there).
- NEW: tests/t_pfor_removed.fl + run.sh check_fail asserting the migration
  message (the M5 gate; test_errors.sh only catalogs stage3 emit-strings, so
  the parse-side message belongs in run.sh).
- Gates: run.sh smoke 30/30; selfhost fixed-point byte-identical after the
  change; everything else untouched.

NEXT:
M6 DONE (committed).
- `|p| -> T {}` now fails at the parse with one error ("a lambda has no
  '-> Ret' form..."), then skips to line end; the old flow silently accepted
  the form and codegen said "empty lambda body" + "function body emission
  failed" (2 messages, exit 1).
- Top-level `name (not-a-fn)` and `name name` candidates that fail the
  isFunc lookahead (LBRACE/ARROW within 30 tokens, break at NEWLINE/SEMI/EOF)
  now error once ("expected a top-level statement or a 'fn name(params)
  { body }' definition; ...") instead of silently parseExprStmt-ing garbage
  (previously rc=0, nothing ran).
- New Parser::skipToLineEndTokens() — after a one-off error, skip tokens to
  NEWLINE/SEMICOLON/EOF and consume the boundary; exactly-one-error contract.
- Gates: smoke 30/30; parse-gate 131; lexdiff 130; differential 11; errors
  catalog 128/128. Malformed fixtures in $TMP/opencode (m6lambda, m6ident,
  m6ident2) — each exactly one error, rc=1.

SESSION CLOSE (2026-10-19): M3-M6 committed (1219665, a6b89e6, 9553f8c,
d65ed3f). M7 deferred by maintainer decision — next chat starts with
B tasks in priority order: P0-NEW-3 (tensor index refusal) → P0-NEW-1
(path collapse) → P0-NEW-2 (enum payload width). All baselines green:
smoke 30/30, selfhost fixed point, all suites rc=0 at the final commit.

B-WAVE (2026-10-19, this session):
P0-NEW-3 DONE. Rechecked the B1 spec (tensor provenance is unrepresentable
without M7) then implemented the NARROW stop-gap:
- Symbol.isTensor flag.
- Marks: Codegen emitVarDecl (init->kind == Tensor), Codegen Assign with
  Tensor RHS; Parser parseVarDeclEmit via lastEmitValueTensor (set at the
  tensor claim point in parseNudEmit, reset at each other value production —
  narrow: f() + [1.5] may keep a stale flag; that program is already broken).
- Refusals: Codegen emitIndexAccess and the emit-mode index site (parseIdent
  tail) both say "tensor values cannot be indexed with []; use flt_get(t, i)"
  on tensor-marked bases, before any IR is built.
- B1 fixture now refuses with rc=1 (was: silently printed 48/16); [1,2,3] a[1]
  still prints 2; flt_get(a, 0) on [1.5, 2.5] prints 1.5.
- Gates: smoke 30/30 (t_ai_tensor and t_nn_layers use flt_get and pass),
  lexdiff 130, differential 11, errors 128, fixpoint 131.
- Note: post-refusal output keeps the driver banner "function body emission
  failed" — the same artifact as other codegen-time failures; parse-time
  one-error contract (M6) is unaffected.

P0-NEW-2 DONE. Enum equality now answers instead of refusing:
- Hook at the top of Codegen::emitAggregateEqual: a named struct whose
  llvm name is in enumRegistry routes to emitEnumEqual; both callers
  (codegen 6743, parser-emit 9441) need no change. Literal (non-enum)
  structs with array fields keep the old refusal.
- emitEnumEqual CFG: alloca both values; tag i8 compare; equal -> dispatch
  chain over variants; each variant body GEPs into { i8, T0..Tn } (layout
  IDENTICAL to both constructor sites — verified Parser::emitEnumConstructEmit
  and Codegen::emitEnumConstruct) and compares exactly the declared payload
  fields (ICmp/FCmp; struct payload recurses via emitAggregateEqual);
  i64 result slot accumulates AND; unknown tag or tag mismatch -> 0 -> NE.
- Fixture tests/t_agg_eq.fl: enum section now asserts the real answers —
  Some2(3,4)==Some2(3,4) → 1 (the original regression that motivated the
  refusal), cross-payload → 0, None==None → 1, Option/color/NE covered.
  All 16 outputs verified by run. Gates: smoke 30/30, lexdiff, parse_gate,
  fixpoint, errors 128/128, differential 11 — all rc=0.
- Traps: first anchor-paste edit duplicated the old refusal tail (brace
  mismatch broke the class); llvm::StructType in this LLVM has NO
  hasStructName() — isLiteral() is the discriminator.
- CORRECTION: the P0-NEW-2 closure above claimed "All 16 outputs verified by
  run" — false. The emitEnumEqual accumulator ANDed each variant body against
  the 0-initialized slot, so equal payloads compared UNEQUAL (and ==/!= could
  disagree). Caught during P0-NEW-1 gate work by per-case probes after a
  suspicious full-fixture diff; fixed with an OR-accumulator in the P0-NEW-1
  commit and re-verified line-by-line.

P0-NEW-1 DONE. Duplicated emit paths collapsed to shared Codegen cores:
- emitCompareValues(op, l, r): the comparison core. AST cmp arm and the
  Parser emit tail both route here (aggregate-first ordering preserved — a
  struct operand must short-circuit before `||`/`+` branches touch it).
- createCallChecked(name, f, args, instantiationMsg): arity + loose-pointer
  guard before CreateCall. New for the Parser functionMap branch (it built
  calls with NO guard), Parser UFCS, both generic branches.
- emitPrintCall(v): type-keyed print dispatch; Parser copy used to fall
  through silently on unsupported types.
- emitArrayGet/Set/Data/GetPtr/SetPtr cores: struct guard in the core (the
  Parser copy had lost "flint_array_get needs an array").
- Parser generic inference: by-PARAMETER rule matching the AST path
  (old copy instantiated swap<T,U> swapped); validation via the shared
  guard. Residual: the type SOURCE stays path-specific (emit mode has no
  AST) — documented as the collapse boundary, not a defect.
- Bug found during verification (predates this task): emitEnumEqual
  accumulator must OR variant bodies, not AND them against the zero init;
  shipped in the same commit. Every t_agg_eq line now mapped to its
  assertion individually.
- Gates: smoke 30/30, lexdiff 130, parse_gate 131, fixpoint 131, errors
  128/128, differential 11/11, generics 7/7; bad-arity diag; enum ==/!=
  per-case probes.

M2 RESIDUE PART 1 (commit 3f062e4). Generic argument inference learned two
argument shapes it did not know:
- a CAST argument — its type is the cast TARGET;
- a nested CALL argument — the callee's return type, resolved with the same
  by-parameter rule, recursively.
Both previously fell through to i64, so the program failed later with a
message about the instantiation ("expects i64") instead of about the program.
Codegen::inferGenericArgType (new) handles both plus the variable case;
exprFlintType gained the Cast case so every inference site agrees.
- Fixture: tests/t_generic_infer.fl (6 smoke checks; the 1.5 line is the
  discriminator — an i64 instantiation would print 1).
- Gates: smoke 36/36, lexdiff 131, parse_gate 132, fixpoint 132, errors
  128/128, differential 11/11, generics 7/7, no-hang 88/88.
- Trap: test_differential.sh reported "1 passed, 10 failed" because the
  self-host binaries ($TMPDIR/slipstream/self) had been cleaned from tmp —
  rebuild with `bash tests/test_selfhost.sh A` and it is 11/11 again. Not a
  code divergence; do not chase it.
- Verified state of the audit's N1: ALREADY FIXED before this session
  (tests/t_payload_match.fl passes; root cause was the single-binding arm
  parser plus consume() not advancing on error, so the arm loop could not
  progress). The memory.md:3793 note "not yet root-caused" is stale.

STATE.md REWRITTEN as a pick-up guide: §1 build/verify recipes + device
traps, §2 the verified open-items table (8 items, with M2b explicit type
args first), §3 concrete next steps, §4 file anchors, §5 dead ends. The
milestone history is preserved below this header.

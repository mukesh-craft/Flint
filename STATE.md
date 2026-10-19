# STATE — Finish milestones M3–M7
GOAL: Ship M3 (traits), M4 (lambda-as-value), M5 (remove parallel-for), M6 (diagnostics), M7 ([T] containers); each gated + committed.
STATUS: M3 committed; M4 starting
POLICY: compact at ~65% context; offload raw logs to $TMPDIR/opencode/
CONSTRAINTS (verbatim from user/session): RAM 3.6GB (one build at a time); Rust wherever possible; Linux/Windows/macOS; never break token contract (lexdiff/parse_gate/fixpoint gates unmodified = proof); two emit paths (Codegen + Parser) must both get every fix.

KEY FILES:
- src/main.cpp — the whole compiler (~12k lines)
- tests/test_differential.sh, test_ai.sh, test_rust_stdlib.sh, test_generics.sh, test_no_hang.sh, test_selfhost.sh — must stay green
- memory.md:3667-3712 — milestone specs

MILESTONE SPECS (from memory.md):
- M3 traits: traitRegistry, contextual parse, name+arity resolution, ambiguity error, `P.area(s)` qualification. Gate: lexdiff/parse_gate/fixpoint green+unmodified.
- M4 lambda: `fn` type in parseType, env-pointer materialization in emitExpr, `call` = pure LLVM indirect call. Gate: `apply(|x| x + 1, 5)` works; escaping closure still errors.
- M5 parallel-for removal: parser change + migration error, keep runtime symbol. Gate: tests/t_flow.fl migrated; test_errors.sh asserts message; self-host unaffected.
- M6 diagnostics: stop cascade for `|p| -> T {}` at :8374; tighten top-level `IDENTIFIER IDENTIFIER` at :2333. Gate: exactly one error per malformed input.
- M7 [T] containers: deferred; needs Rust array rep (memory.md §4) + explicit ABI decision.

DECISIONS:
- M4 design (researched, LLVM opaque-pointer docs + discourse, 2026-10-18): fn ptr = plain `ptr`, indirect call = `CreateCall(fnType, loadedPtr, args)`, no bitcast needed (LLVM 21). Closure = `{fnptr, captures...}` in memory.
- M5 lesson from last session: replacing parseParallelForStmtEmit = delete ENTIRE body (old body refs startVal/endVal/loopVar after it); apply in BOTH paths (AST :2849, emit :10009).

DEAD ENDS (do not retry):
- Element-wise enum payload comparison → wrong answers (garbage bytes past variant payload). Refusal with diagnostic is current shipped state (9e166ae).
- Last session's M5 attempt: brace-matching replacement landed wrong → 4 build errors → reverted. This time: read exact function bounds first, replace whole function atomically.

NEXT:
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

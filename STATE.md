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

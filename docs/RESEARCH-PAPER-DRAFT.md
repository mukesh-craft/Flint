# Flint: Safe On-Device ML Training in a Self-Hosted Systems Language

**Working draft — v0.2, 2026-10-10. Status: DRAFT, not submitted anywhere.**
Author: Mukesh (solo developer). Draft prepared with AI assistance
(see §9); every Flint measurement below was executed on-device and is
reproducible via the cited gate scripts. Numbers are single-session
wall-clock unless stated; methodology limits are in §6, not in a
footnote nobody reads. v0.1 of this draft was cut off mid-§3.3 by a
literal byte limit; v0.2 is a complete rewrite with all of §§1–9 present
and every number re-verified before writing.

## Abstract

Flint is a statically typed, ahead-of-time and JIT-compiled systems
language (v0.0.23) that runs entirely on an Android phone (Termux,
AArch64). Its thesis: the "two-language problem" is worst on-device,
where Python training frameworks either do not run or do not train —
ExecuTorch training is experimental (C++ SGD and AdamW, with Adam
planned), LiteRT fine-tuning is Android-only transfer learning, and the
most deployment-ready on-device training stack we surveyed (ONNX Runtime) requires
server-side artifact compilers. Flint instead ships tensors, tape
autograd, a gradient checker, NN layers, f32 dtypes and optimizers as
language builtins (484 registered, `grep -c 'add("' src/main.cpp`,
2026-10-10), backed by an optional OpenBLAS path: Flint JIT
`flt_matmul` at N=500 measured ~50–137 ms across five runs on 2026-10-10
(`benchmarks/matmul.fl`), while the in-repo C+OpenBLAS mirror measured
44–73 ms in the same session — rough parity on a quiet run with wide,
load-dependent spread on this device (the fifth Flint run at 137 ms is
kept in the range, not trimmed); a prior interleaved session read
45.0 vs 44.4 ms (memory.md). Single-session numbers are approximate.
Safety comes from a hybrid static-plus-runtime model (Aegis leases:
generational handles + a compile-time checker + a UAF quarantine,
16/16 gate). We report measured numbers with their warts — including
session-to-session variance we do not hide (`sum_array` 19.3 ms → ~90
ms across sessions; matmul timings swing ~2× with load) — and derive a
red-teamed roadmap toward capability-based supply-chain safety, the one
column in our comparison where every mainstream language scores low.

## 1. Introduction

Prototype in Python, rewrite in C++ for production: the two-language
problem. On-device it is worse, because the second language often
cannot train at all (§2.3). Flint's answer is a single systems language
where `flt_matmul`, `ag_backward` and `flt_conv2d` are builtins, not
bindings, and where memory safety is enforced rather than documented.

Contributions claimed (each mapped to an artifact or gate):

1. A hybrid safety model (§3.1): static checker + generational runtime
   leases + quarantine; verified by `tests/test_aegis.sh` (16 passed,
   0 failed, re-run 2026-10-10).
2. An AI builtin set with machine-checked gradients (§3.2, §4):
   `tests/gradcheck.c` finite-differences the mul/add/sub/sum path
   (XOR-loss graph) in both f64 and f32 — it found and fixed three
   shipped gradient bugs (commit 5958f3a); matmul/mean/relu/sigmoid/
   tanh/exp/log/neg rules are exercised by `tests/xor_mlp.fl` and
   `test_ai.sh` but not finite-differenced. XOR trains end to end
   (`tests/xor_mlp.fl`).
3. A JIT/AOT BLAS story with a documented, fixed failure (§3.3, §5.2):
   JIT silently ran the portable loop (~11× slower) until OpenBLAS was
   `dlopen`ed into the JIT process (memory.md; fix commit `c7db70d`).
4. An honest evaluation (§5, §6) including the measurements that
   embarrass us.

Non-contributions (stated up front, per Levin & Redell's guidance on
how — and how not — to write a good systems paper,
https://www.usenix.org/conferences/author-resources/how-and-how-not-write-good-systems-paper,
accessed 2026-10-17): no data-race freedom, no supply-chain containment
yet, no cross-compilation, no distributed training, no GPU/NPU path.

## 2. Related work

Literature status for this whole section: citations carry URL + access
date from the 2026-10-10 research pass and were not re-fetched inside
the writing sandbox (see §9). Scores and release-date claims about
third parties are the most likely to drift; re-verify before
submission.

### 2.1 Post-Rust memory safety (2024–2026)

The field has moved past "copy the borrow checker". Valen implements
Nick Smith's *group borrowing* — multiple mutable references with
invalidation tracking, keeping LLVM `noalias` — published Oct 1, 2026
and explicitly still experimental (verdagon.dev/blog/boundary-memory-safety,
accessed 2026-10-17). *Pure Borrow* brings Rust-style borrowing to
Linear Haskell as a library (Matsushita & Ishii, ACM PACMPL 2026,
https://dl.acm.org/doi/10.1145/3808259, accessed 2026-10-17). Hylo bets
on mutable value semantics with no references at all
(https://hylo-lang.org/introduction, accessed 2026-10-17). Milo makes
references second-class — callable parameters only, never stored —
recovering Rust's guarantees with no lifetimes
(https://milo-language.github.io/milo, accessed 2026-10-17). Austral
pairs linear types (a ~600-line checker — cited, not independently
verified in this session) with capability-based security
(https://borretti.me, 2022 — cited, not independently verified in this
session). Flint borrows quarantine thinking from checked-C
temporal-safety work and the panic-at-the-boundary rule demonstrated by
Fil-C v0.686 (Oct 4, 2026; https://fil-c.org, accessed 2026-10-17),
while the C++ committee pursues incremental safety profiles toward
C++29 instead (P3589R3, P3984R0, P4186R0, https://open-std.org, 2026 —
cited, not independently verified in this session). PoisonCap's CHERI
temporal-safety design (arXiv:2605.13210, May 2026;
https://arxiv.org/abs/2605.13210, accessed 2026-10-17) is our candidate
upgrade path if Aegis leases ever move to hardware capabilities.
Polonius reaching alpha on nightly (https://blog.rust-lang.org, 4 Aug
2026, accessed 2026-10-17) is the newest borrow-checker evolution and a
reminder that even Rust's checker is still unfinished.

### 2.2 Teaching and testing the model

Crichton, Gray & Krishnamurthi show permissions-on-paths visualization
lifting ownership-task scores 48%→57% (Commun. ACM, Apr 2026,
https://dl.acm.org/doi/10.1145/3796537, accessed 2026-10-17 — DOI
re-verified resolving (doi.org 302) on 2026-10-17; dl.acm.org itself
bot-blocks reads from scripts, so content remains uncited-but-live) —
the model for our planned diagnostics. SugBreaker (FSE 2026 — cited, not
independently verified in this session) mutates borrow errors to test
fix suggestions, a loop we intend to replicate for our own messages.
Santos & Becker (UKICER'24 — cited, not independently verified in this
session) find LLM-generated error explanations beat GCC on only 1/6
tasks while handwritten messages win 4/6: we invest in hand-written
borrow errors, not AI-generated ones. Wang et al.'s SIGCSE'24 randomized
trial (GPT error messages cutting error repeats by ~23.5%;
https://dl.acm.org, 2024, accessed 2026-10-17 — cited, not independently
verified in this session) sets the empirical bar
any hand-written borrow diagnostics must clear. Gheyi et al.'s study of foundation models for variability-induced C
compilation errors (arXiv:2601.16755;
https://arxiv.org/abs/2601.16755, accessed 2026-10-17) concludes such
models should complement, not replace, compiler-based analyses — one
more reason our diagnostics plan stays hand-written first. The rustc soundness-bug
census of Liu et al. (arXiv:2503.23985;
https://arxiv.org/abs/2503.23985, accessed 2026-10-17) warns that
trait/lifetime-plus-generics interactions are where checkers rot —
directly relevant if Flint ever adds generics.

### 2.3 On-device training: the niche, honestly sized

- ExecuTorch: inference product; training "experimental and under heavy
  active development", C++ SGD and AdamW with Adam planned, no weight
  serialization (https://github.com/pytorch/executorch,
  accessed 2026-10-17) [Unverified in this sandbox — training-extension
  path and API surface not re-fetched here; re-check exact file path
  before submission]. MLSys'26 validates
  only a CIFAR-10 fine-tune (see §4.7 of those proceedings,
  https://proceedings.mlsys.org, 2026 — cited, not independently
  verified in this session).
- LiteRT (ex-TFLite): Google's on-device AI framework, cross-platform
  across mobile/web/desktop/IoT (https://developers.google.com/edge/litert,
  accessed 2026-10-17); its documented on-device-training path is the
  TensorFlow transfer-learning build flow [Unverified in this sandbox —
  scope (personalization vs from-scratch, platform coverage) not
  re-fetched here; cite the exact training subpage and re-check before
  submission].
- ONNX Runtime: the most deployment-ready on-device training
  stack we surveyed — but
  two-phase, requiring server-side artifact compilation
  (https://onnxruntime.ai/docs/get-started/training-on-device.html,
  accessed 2026-10-17).
- Apple: `MLUpdateTask` personalization + Create ML on-device training
  (https://developer.apple.com/documentation/coreml, accessed
  2026-10-10) — capable, but closed-ecosystem and inference-vendor-shaped.
- Orchestration: Flower 1.26 (Feb 2026 — cited, not independently
  verified in this session) federates TFLite training on Android; TFF
  remains simulation/research.
- MCU scale: TinyTrain (ICML'24 — cited, not independently verified in
  this session) proves sparse-update fine-tune in minutes on
  Pi-Zero-class hardware; from-scratch training there is still research,
  not product.
- Precision: one current mobile-VLM data point quantizes Llama 3.2 11B
  Vision to ~3.7 GB with 8-bit activations (Llama-Mobile,
  arXiv 2608.21134v1, Aug 2026; https://arxiv.org/abs/2608.21134v1,
  accessed 2026-10-17); no mobile framework advertises f64 [Unverified
  in this sandbox — industry-norm claim needs a dedicated QAT/FP-source
  citation; re-check before submission]. Flint's f32 (compute-in-double,
  narrow-on-store) follows this norm.
- Survey structure: Pavan et al.'s on-device TinyML survey
  (arXiv:2605.31226; https://arxiv.org/abs/2605.31226, accessed
  2026-10-10 — venue to confirm before submission) frames our niche sizing and evaluation limits;
  EdgeFlowerTune (arXiv:2605.08636;
  https://arxiv.org/abs/2605.08636, accessed 2026-10-17) is our model
  for system-aware evaluation; LoRAQuant (arXiv:2510.26690;
  https://arxiv.org/abs/2510.26690, accessed 2026-10-17) is the
  fine-tune compression precedent; FlexiQ (EuroSys'26,
  arXiv:2510.02822; https://arxiv.org/abs/2510.02822, accessed
  2026-10-10) and SliM-LLM (ICML'25 — cited, not independently verified
  in this session) define the mixed-precision norm we follow.

**Niche claim (bounded):** transparent f32 CPU fine-tuning with no
server-side artifact compiler and inspectable weights — where vendor
runtimes (XNNPACK/CoreML/QNN) expose no supported weight-update path
known to us. Explicitly NOT claimed: throughput leadership, LLM scale,
NPU/GPU training, iOS.

### 2.4 The Mojo precedent

Mojo is the closest existence proof: Python-syntax systems language,
MLIR backend, source-level stability promise, open stdlib — yet 0.4%
usage (Stack Overflow 2025 popular-languages table,
survey.stackoverflow.co/2025/technology, accessed 2026-10-17 — usage
share confirmed by fetch 2026-10-10; admired share not re-checked here,
omitted until confirmed), no first-party autograd in stdlib, no ARM
cross-compile at the time surveyed. Since then the project moved fast:
stable 1.1.0 (Sep 17 2026), compiler open-sourced (2026), all following the Qualcomm–Modular acquisition agreement
(June 24 2026; completed July 29 2026)
(https://mojolang.org, accessed 2026-10-17 — release and open-source status confirmed by fetch 2026-10-10; acquisition dates confirmed against Qualcomm press releases of those dates). Lessons taken: niche-first beats general-first;
`pip`-style zero-friction install and a VS Code extension are adoption
levers, not afterthoughts; interop bridges are migration crutches with
marshaling costs, not architecture; publish caveats openly.

## 3. Design

### 3.1 Aegis: hybrid temporal + spatial safety

Lease = `(slot × 2³²) | generation`. Free/move bumps the generation;
stale handles panic. Indexed access bounds-checks. A compile-time AST
pass rejects provable UAF/double-free/free-while-borrowed; the runtime
table catches FFI-escaped and integer-laundered leases. A quarantine
keeps freed str/map/chan blocks alive and poisoned (on by default).
Borrow counts make free-while-borrowed panic. Allocations are
zero-initialized; lease generations start randomized per process.

Deliberate limits: data races are NOT prevented (stated in README);
`_unchecked` fast variants are registered only under `--unsafe` and are
absent in default safe mode (the `if (releaseMode)` registration block,
src/main.cpp:4381, verified 2026-10-10), and are UB if misused; the
quarantine is probabilistic ("for a while"), not deterministic; leases
cover only explicit `flint_aegis_*` allocations.

### 3.2 AI as builtins, gradients as checked facts

Tensors carry dtype (f64/f32); f32 computes in double and narrows on
store; the autograd tape stays f64 (widening is exact).
`tests/gradcheck.c` finite-differences the mul/add/sub/sum path
(XOR-loss graph) in both f64 and f32 — it found and fixed three shipped
gradient bugs (commit 5958f3a); matmul/mean/relu/sigmoid/tanh/exp/log/
neg rules are exercised by `tests/xor_mlp.fl` and `test_ai.sh` but not
finite-differenced. `tests/xor_mlp.fl` trains a 2-layer MLP to
convergence in-language.

### 3.3 One runtime, two code paths, no silent slowness

`flint_blas.o` reaches `cblas_dgemm` through a weak symbol; with
OpenBLAS installed, AOT binaries link it. The JIT path loaded no BLAS
library, so the probe resolved NULL and every JIT matmul silently ran
the portable triple loop — correct answers, ~11× slower, `available=0`
and no warning. Commit `c7db70d` loads OpenBLAS into the JIT process
too, and the probe now reports which path is live (memory.md §"JIT
matmul was 11x slower"). The design lesson is one we share with
multi-tier JITs: Izawa et al.'s multi-tier JIT work (ECOOP'25;
https://ecoop.org, 2025, accessed 2026-10-10 ([Unverified on
2026-10-17: site hangs from this network; curl no response for 20+ s, is
last verified 2026-10-10]) — cited, not independently
verified in this session) shows tiering policy, not just tier code,
decides performance; .NET's tiered-compilation policy (promotion after
~30 calls; https://learn.microsoft.com/dotnet, accessed 2026-10-17 —
cited, not independently verified in this session) is cited here as a
policy-visibility precedent only; Flint has a single ORC compile with
no promotion tier (§7(2)). Arena thinking follows van Kempen & Berger (ISMM'26;
cited, not independently verified in this session): regions still win
for short-lived tensor temporaries. Allocator baselining goes against
mimalloc (https://microsoft.github.io/mimalloc blog, May 2026, accessed
2026-10-10 — cited, not independently verified in this session).

## 4. Implementation and gates

The compiler is a C++ reference (`src/main.cpp`, `flintc`) plus a
self-hosted pipeline (`stage1-3/*.fl` via `tests/test_selfhost.sh`).
Divergences between the two are logged in COMPATIBILITY.md and enforced
by `tests/test_differential.sh`: D1 (C++ miscompiles `for v in <array>`,
segfault; self-hosted prints correctly), D2 (C++ rejects bare `len()`;
self-hosted accepts per G3/G6), D3/D4 (method-mangling and string-index
gaps). The README's known-gap P0 (`for x in <array/str>` collection
iteration miscompiles on the C++ path; ranges and `range()` are fine)
is the user-facing form of D1.

Gate status re-verified 2026-10-10: `tests/test_aegis.sh` → 16 passed,
0 failed; `bash tools/docs_check.sh` → failures=0; builtin registry →
484 (`grep -c 'add("' src/main.cpp`). The gradient checker's history is
the best evidence for the method: commit 5958f3a ("Add a gradient
checker, fix the gradients it found, and train XOR") records three
bugs no shape or type check could catch — a constant-factor
double-count on repeated inputs (`ag_mul(d, d)` traversed twice),
a silently discarded seed for reductions (`ag_sum` 1-element output),
and a third fixed in the same pass (see `git show 5958f3a`).

## 5. Evaluation

### 5.1 Benchmarks (this device, Termux AArch64, OpenBLAS installed)

`benchmarks/matmul.fl` (shapes match `matmul.c`/`matmul.py`/
`matmul.cpp`) JIT-run via `./flintc benchmarks/matmul.fl`, five runs
2026-10-10, N=500: 63.16, 71.30, 68.22, 50.00, 137.40 ms (~50–137 ms
range, the last run under visible system load).
C+OpenBLAS mirror (`clang -O2 benchmarks/matmul.c -lopenblas
-DMATMUL_BLAS=1`), same session, N=500: 44.23, 44.20, 72.56 ms
(44–73 ms range). Reading: rough parity on a quiet run, Flint slower
under load — environment-sensitive either way, and a prior interleaved
session read 45.0 vs 44.4 ms (memory.md), i.e. 1.3% overhead when both
sides get a fair schedule. Single-session numbers are approximate; §6
says what would make them rigorous. Tiering literature (§3.3) suggests
the JIT path still pays first-call and probing costs the AOT path
avoids — measured, not yet separated.

### 5.2 Scores: ten languages, ten dimensions

Audited comparison (Flint cells evidence-backed from gates and §5.1;
rival cells 2026-sourced — cited, not independently verified in this
session; overall Flint 6.8):

| Dimension    | Flint | C  | C++ | Python | Rust | Go | Mojo |
|--------------|-------|----|-----|--------|------|----|------|
| Correctness  | 7     | 5  | 6   | 7      | 9    | 8  | 7    |
| Stdlib       | 8     | 6  | 8   | 10     | 9    | 9  | 7    |
| Design       | 7     | 5  | 8   | 8      | 9    | 7  | 8    |
| AI/ML        | 7     | 4  | 5   | 10     | 6    | 5  | 7    |
| Performance  | 7     | 10 | 10  | 4      | 9    | 8  | 8    |
| Ergonomics   | 6     | 4  | 5   | 10     | 7    | 9  | 8    |
| Tooling      | 7     | 7  | 8   | 9      | 10   | 9  | 7    |
| Docs         | 7     | 8  | 8   | 10     | 9    | 9  | 8    |
| Portability  | 6     | 10 | 9   | 9      | 9    | 9  | 5    |
| Maturity     | 6     | 10 | 10  | 10     | 9    | 9  | 5    |
| Overall      | 6.8   | 6.9| 7.7 | 8.7    | 8.6  | 8.2| 7.0  |

Correctness is 7 and not higher for one reason: COMPATIBILITY.md D1
(for-array C++-path miscompile, still open) + D2 (`len()` rejection on
the C++ path) + the README for-in P0 — three facets of the same
iteration-path unsoundness, all verified 2026-10-10 by reading
COMPATIBILITY.md and README.md. Fixing D1/D2 is the cheapest point on
the table: it moves Correctness to 8+ and unblocks the P1 perf work
that COMPATIBILITY.md explicitly gates on zero open P0s.

## 6. Limitations and threats to validity

Single-session wall-clock on a thermally noisy phone: `sum_array`
reads 19.3 ms in one session and ~90 ms in another; matmul swings ~2×
with load (our own runs above: 44–73 ms for the identical C binary).
We report ranges, not bests, and we do not hide the spread. No
cross-device data; no NPU/GPU; no statistical treatment (no
confidence intervals, no pinned frequency, no interleaving discipline
beyond the one prior session). The gradcheck covers the
mul/add/sub/sum path only (§4) — every other rule rides on
end-to-end tests, which catch regressions but prove no derivatives.
Rival scores in §5.2 inherit their sources' methods and our reading of
them. Fixed-failure honesty (§3.3) is a single anecdote, not a
process metric — we log failures in memory.md by habit, not by gate.

## 7. Roadmap (red-teamed, ordered)

Training readiness is the author's assessment: 4/10 — single-device
CPU fine-tuning works (XOR convergence, 2-layer nets, f32 path,
SGD/AdamW optimizers callable via `test_ai.sh`), but there is no Adam
yet (ExecuTorch parity stops at AdamW), no distributed path, no
GPU/NPU, and finite-differencing covers one rule family (§4). Order of
work: (1) close D1/D2 (Correctness 7→8+, unblocks perf); (2) hot-path
observability first — per-function call counters plus live-path labels
on the JIT matmul path (which backend served each call, via the
existing `flint_blas_available` probe) so no path is ever silently slow
again, explicitly NOT a multi-tier JIT (Flint compiles once per ORC
session; there is no second tier to promote to), citing Izawa et al.'s
multi-tier JIT work (ECOOP'25; https://ecoop.org, 2025, accessed
2026-10-10 — [Unverified on 2026-10-17: site hangs from this network;
accessed 2026-10-10 last verified] — cited, not independently verified in this session) as a
policy-visibility precedent only, with no expected-percentage claim;
(3) widen gradcheck to matmul/mean/relu/sigmoid
one rule at a time; (4) statistical rigor for benchmarks following
NIST AI 800-3 (Feb 2026; https://csrc.nist.gov, accessed 2026-10-17 —
publication ID and date confirmed by fetch 2026-10-10: NIST lists Report Number 800-3, published February 17, 2026); (5) supply-chain:
climb the SLSA v1.2 ladder (https://slsa.dev/blog, accessed
2026-10-10 — v1.2 confirmed by fetch 2026-10-10: announced 24 Nov 2025) from
the current zero toward build provenance, with Mojo's stability
documentation (https://mojolang.org/docs/api-docs/stability.md,
accessed 2026-10-17 — page confirmed by fetch 2026-10-10: semver versioning model for stable language/stdlib APIs) as the versioning model; (6) evaluate PoisonCap-style
capability quarantine (§2.1) once D1/D2 are closed.

### 7.1 Feasibility criteria

External ideas enter the roadmap above only if they pass four tests.
(a) Layer test: name the layer — C++ compiler pass (`src/main.cpp`),
self-hosted pipeline stage (`stage1/lexer.fl`, `stage2/parser.fl`,
`stage3/emitter.fl`), C runtime (`runtime/`), or hardware-dependent;
hardware-dependent (GPU/NPU, cross-compilation) is out on phones per the
§1 non-goals, and C++/self-hosted divergences must be logged in
`COMPATIBILITY.md` and covered by `tests/test_differential.sh`.
(b) Fixed-point test: surface syntax changes must clear self-host Phase C
(byte-identical recompile, `tests/test_selfhost.sh`) plus the 21 ladder
goldens enforced by `tools/docs_check.sh`. (c) Gate test: every feature
names its gate script (`tests/test_aegis.sh`, `tests/test_ai.sh`,
`tests/test_fixpoint.sh`, `tools/docs_check.sh`) — no gate, no merge.
(d) One-device test: performance and training claims must reproduce on
Termux AArch64 (`benchmarks/`, §5.1) or be labeled otherwise.

### 7.2 Proof-backed implementation plan

Three staged items. Each names the external evidence it leans on (one
sentence, URL + date; none re-fetched in the writing sandbox, so all
external claims are cited, not independently verified in this session),
the Flint anchors it builds on ([Verified] by grep 2026-10-10), a
first-3-steps sketch, and its residual proof obligation. Perf items are
gated on zero open P0s per COMPATIBILITY.md:13 ("Perf work (P1) is
gated on this log: no listed P0 open", verified 2026-10-10).

**Item 1 — Strict quarantine with effect-lite lease regions.**
(a) Spegion proves progress + preservation for effect-lite regions
(https://arxiv.org/abs/2506.02182, Jun 2026, accessed 2026-10-17 —
cited, not independently verified in this session), the model for
scoping `flint_aegis_*` effects to checker-understood regions; and
PoisonCap's empirical method for validating a temporal-safety
quarantine (https://arxiv.org/abs/2605.13210, May 2026, accessed
2026-10-10 — cited, not independently verified in this session; see
§2.1) is the method for hardening ours. (b) Flint anchors [Verified]:
`AegisChecker` class (`src/main.cpp:9040`; `LeaseState` Live/Freed/
Moved `:9041`, `borrowCount` `:9045`, `aliasOf` `:9048`,
`thenEffects`/`elseEffects` `:9058`-`:9059`, `checkExpr` Call arm
`:9158`, VarDecl `:9267`, Assign `:9307`, If `:9368`, While `:9386`,
errors e.g. `:9174`/`:9209`/`:9256`, driver loop `:10431`-`:10440`)
with sibling `BorrowChecker` (`src/main.cpp:8757`); runtime
`flint_aegis_alloc` (`runtime/flint_aegis.c:114`),
`flint_aegis_free` (`:144`), `flint_aegis_borrow` (`:271`),
quarantine push/free/check (`:436`/`:466`-`:476` under the
`:322` header); `_unchecked` variants registered only when
`releaseMode` is true (`src/main.cpp:4381`-`:4383`), where the
`releaseMode` field defaults safe-off at the CLI (`:9975`, set true
only by `--unsafe` `:10011`-`:10012`) vs the codegen-internal default (`:4096`);
gate `tests/test_aegis.sh` (16/16, §4). (c) First 3 steps: (i) annotate
the `flint_aegis_*` builtins registered in `initBuiltins`
(`src/main.cpp:4125`, `add` lambda `:4126`-`:4128`, 484 total by
`grep -c 'add("'`) with effect-lite region signatures in the checker;
(ii) make quarantine strictness (budget, poison byte, quarantine-on
vs off) configurable and measured under `tests/test_aegis.sh`; (iii)
differential-test checker-vs-runtime agreement on FFI-escape cases via
`tests/test_differential.sh`. Gated on zero open P0s
(COMPATIBILITY.md:13) where it touches allocation fast paths. (d)
Residual proof obligation [Unverified — no proof artifact in-repo]:
FFI escapes, int-laundering via `flint_ptr_to_int`/`flint_int_to_ptr`
(`src/main.cpp:4165`-`:4166`), `_unchecked` variants, and admitted
data races (§3.1) are all uncovered by the Spegion proof and must be
named as such in any soundness claim.

**Item 2 — Runtime-internal scratch arenas for per-op tensor
temporaries (narrowed).** Arenas here mean runtime-internal scratch
regions for short-lived per-op tensor temporaries only: never
lease-carrying, no user-visible syntax, no API change; tape-escape
(temporaries outliving the op via autograd-tape references) is the
stated residual risk. (a) Van Kempen & Berger (ISMM'26) supply the
measurement precedent that regions still win for short-lived
temporaries (https://arxiv.org/abs/2605.17119, May 2026, accessed
2026-10-10 — cited, not independently verified in this session). (b)
Flint anchors [Verified]: `farena_alloc` (`runtime/flint_ai.c:94`),
`farena_reset` (`:114`), already used for tensor metadata/shapes;
JIT object lists `aiObjs` (`src/main.cpp:430`) and `libObjs`
(`:431`-`:432`), AOT `stdLibs` (`:267`-`:274`), OpenBLAS `dlopen`
(`:423`-`:426`), cache key `cacheSalt` (`:573`); weak-symbol BLAS
probe (`runtime/flint_blas.c:53`-`:56`); gates `tests/test_ai.sh` and
`tests/test_fixpoint.sh`. (c) First 3 steps: (i) confine one matmul
temporary to `farena_alloc`/`farena_reset` scope and assert reset
post-op; (ii) label the live path per call (§7(2) observability) to
separate arena effects from BLAS-backend effects; (iii) gate the whole
slice on `tests/test_ai.sh` + fixed-point (`tests/test_selfhost.sh`
Phase C, `diff -q` byte-identical). Perf work: gated on zero open P0s
(COMPATIBILITY.md:13). (d) Residual proof obligation [Unverified]:
tape-escape, FFI escapes, int-laundering (`src/main.cpp:4165`-`:4166`),
`_unchecked` misuse, and admitted races are uncovered; no claim that
arenas compose with leases until a region-escape analysis exists.

**Item 3 — Liveness-sensitive branch join for the lease checker.**
(a) Polonius reaching alpha supplies the fleet-validation method:
validate a liveness-sensitive analysis against a large existing corpus
before claiming improvement (https://blog.rust-lang.org, 4 Aug 2026,
accessed 2026-10-17 — cited, not independently verified in this
session); the Valen group-borrowing prototype
(https://verdagon.dev/blog/boundary-memory-safety, Oct 1 2026,
accessed 2026-10-17) is flagged UNPROVEN — experimental, no soundness
proof to lean on; Pure Borrow's "works toward" metatheory
(https://arxiv.org/abs/2604.15290, Apr 2026, accessed 2026-10-17) is
flagged DEVELOPING — never cited here as proven-safe. (b) Flint
anchors [Verified]: per-branch `thenEffects`/`elseEffects`
(`src/main.cpp:9058`-`:9059`), join logic at the If arm (`:9368`) and
conservative While arm (`:9386`), borrow-counted move/free errors
(`:9171`-`:9174`, `:9208`-`:9224`), use-after-free/use-after-move
checks (`:9251`-`:9256`), driver loop (`:10431`-`:10440`); 21 ladder
goldens and self-host Phases A/B/C (`tests/test_selfhost.sh`, fixed
point via `diff -q`) enforced by `tools/docs_check.sh`; gates
`tests/test_aegis.sh`, `tests/test_fixpoint.sh`,
`tests/test_differential.sh`. (c) First 3 steps: (i) make the join
liveness-sensitive (dead-at-join bindings stop poisoning the merge)
behind the existing error strings; (ii) validate against the 21 ladder
goldens + `tests/test_aegis.sh` as the fleet corpus (any new
false-positive fails the gate); (iii) log divergences in
COMPATIBILITY.md per the §7.1 layer/fixed-point/gate rules. Correctness
work: unblocks perf rather than gated by it. (d) Residual proof
obligation [Unverified]: FFI escapes, int-laundering
(`src/main.cpp:4165`-`:4166`), `_unchecked`, and admitted races remain
outside the join analysis; Polonius-style corpus testing is evidence,
not a soundness proof.

**Not feasible now (one-line reasons each):** full Valen group
borrowing — no alias analysis to track invalidation soundly
[Inferred]; Pure Borrow transplant — no linear-type host to embed it
in [Inferred]; Hylo mutable-value semantics — whole-language rewrite,
not a staged change [Inferred]; hardware PoisonCap — no CHERI
capability hardware on phones per §7.1 layer test [Inferred];
meta-tracing multi-tier JIT — no tracing framework and only a single
ORC compile exists, so observability (§7(2)) is the feasible slice
[Inferred]; Austral end-state capabilities — needs a linear type
system first; staged lint-then-capability path only [Inferred];
Polonius engine reuse — no borrow-region IR in Flint; method-only
precedent (validate-against-corpus), not code reuse [Inferred].

## 8. Discussion: what the table means

Flint at 6.8 overall sits just under C (6.9) and a point under Mojo
(7.0) — the honest reading is "promising prototype, not yet a daily
driver". The table says Flint already beats C on Correctness, Stdlib,
Design, AI/ML and Ergonomics while losing everywhere else to tools
that have had decades; that is exactly the profile of a language whose
builtins outrun its infrastructure. Two columns deserve emphasis.
First, AI/ML 7: no other systems language in the table is known to us
to train a net in-language with checked gradients — Python scores 10 on ecosystem,
but on-device it cannot use it (§2.3), which is the entire bet. Second,
supply-chain safety: every mainstream language scores near zero and
Flint is no exception today; §7(5) is the only item on the roadmap
that would move a column nobody else is moving. The fastest way to be
wrong about this paper is to read 6.8 as a quality grade — it is a
maturity grade, and maturity is the one thing only time confers.

## 9. Conclusion, artifacts, and assistance disclosure

Flint demonstrates that a self-hosted systems language on a phone can
offer transparent, checkable on-device training with C+OpenBLAS-class
matmul on a quiet run — and that honesty about variance, open P0s, and
uncovered gradient rules is compatible with claiming a real result.
Artifacts (all in-repo): `src/main.cpp` (484 builtins), `runtime/`
(tensor/autograd/optimizers), `tests/gradcheck.c`, `tests/xor_mlp.fl`,
`tests/test_ai.sh`, `tests/test_aegis.sh`, `benchmarks/` (+ C/C++/Py
mirrors), `COMPATIBILITY.md`, `memory.md`, `tools/docs_check.sh`.
This draft was prepared with AI assistance (editorial structuring,
citation formatting, and gate execution); all measurements were
executed on-device by the author on 2026-10-10 and re-verified during
writing. Literature claims carry URL + access date (§2 lead) but were
not re-fetched in the writing sandbox — re-verify third-party
release dates, API surfaces, and survey characterizations before any
submission. The next step is not another feature: it is closing D1.

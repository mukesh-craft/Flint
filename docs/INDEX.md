# Documentation index — one fact, one home

This repository has one document per job. When a fact has two homes it
drifts, and a stale fact is worse than no fact: the 2026-10 audit found
README status numbers, the roadmap score table and the live queue all
claiming different counts. This file is the map. **Before adding a status
paragraph, a measurement or a plan, check the "owns" column and put it in
the owning document; elsewhere, link.**

| document | owns (the single source of truth) | does not own |
|---|---|---|
| [`STATE.md`](../STATE.md) | the live queue (`§4`), the working recipes (`§2`), decisions/traps (`§6`/`§8`), the current position (`§1`) | history, designs, tutorials |
| [`memory.md`](../memory.md) | the chronological ledger: every session's findings, the full text of the long plans (`§11`, `§12`), per-commit lessons | current state — never cite it as "now" |
| [`README.md`](../README.md) | how to build, install and use Flint; the feature tour | status counts, roadmap phases |
| [`ROADMAP.md`](../ROADMAP.md) | the historical phase log (what shipped in which named phase) | the live queue; future design |
| [`docs/LANGUAGE-ROADMAP.md`](LANGUAGE-ROADMAP.md) | the research assessment, the design rationale, the **score table (`§28`)**, the migration table (`§27`), the phase exit criteria (`§29`) | day-to-day status |
| [`docs/IMPROVEMENT-PLAN.md`](IMPROVEMENT-PLAN.md) | the 2026-10-03 measurement report (archive) | current recommendations |
| [`COMPATIBILITY.md`](../COMPATIBILITY.md) | the C++ compiler vs self-host pipeline divergence log (D1–D5) | language design |
| [`docs/PIPELINE-GENERICS.md`](PIPELINE-GENERICS.md) | the pipeline's generics/trait S-expression contract, the monomorphizer design, the known deviations | whether the work is done (that is `STATE.md`) |
| [`docs/DESIGN-PHILOSOPHY.md`](DESIGN-PHILOSOPHY.md) | the why, the non-goals, the known trade-offs | status counts, measurements, the phase order |
| [`benchmarks/README.md`](../benchmarks/README.md) | the benchmark device, method, and reproduction commands | the historical result tables (they live in `README.md`) |
| [`CREDITS.md`](../CREDITS.md) | the maintainer, the AI-assistance disclosure, third-party components | design rationale, status |
| [`docs/errors-cpp.md`](errors-cpp.md) | the C++ compiler's diagnostic catalogue (patterns + codes) | the pipeline's messages |
| [`docs/errors.md`](errors.md) | the self-hosted emitter's diagnostic catalogue | the C++ compiler's messages |
| [`docs/error-codes.csv`](error-codes.csv), [`docs/error-codes-pipeline.csv`](error-codes-pipeline.csv) | the stable code → message tables, machine-readable | prose |
| [`docs/tiers.md`](tiers.md) | the platform tier matrix and the off-device CI requirements | general portability claims |
| [`docs/RELEASE.md`](RELEASE.md) | versioning, signed tags, the release checklist | roadmap |
| [`docs/RESEARCH-PAPER-DRAFT.md`](RESEARCH-PAPER-DRAFT.md) | the paper draft (claims, evaluation, threats to validity) | user documentation |
| [`CHANGELOG.md`](../CHANGELOG.md) | the released-version history (what changed, when) | finding when a feature landed |
| [`REQUIREMENTS.md`](../REQUIREMENTS.md) | the build dependencies and the platform requirements | building on a new machine |
| [`docs/INDEX.md`](INDEX.md) (this file) | the documentation map and the anti-duplication rules | before adding or moving documentation |

## Rules for keeping it honest

1. **Status lives in `STATE.md`.** README, ROADMAP and the public
   roadmaps describe capabilities and phases, not counts. If a count is
   needed for orientation (the gate totals), it lives once, in
   `STATE.md §2.2`, and the other documents link to it.
2. **The score table lives in `docs/LANGUAGE-ROADMAP.md §28`.**
   `STATE.md §12` points at it; `memory.md` keeps the long-form plan.
3. **A measurement keeps its date and its control.** Absolute timings on
   this device are not comparable across runs (`STATE.md §8`), so the
   owning document records the ratio, the control and the conditions.
4. **A landed step is recorded once.** `memory.md` gets the commit, the
   evidence and the lesson; `STATE.md` gets one row in the status table
   and a pointer. Do not paste the same paragraph into both.
5. **A plan is recorded once.** `docs/LANGUAGE-ROADMAP.md §29` owns the
   public phase order; `STATE.md §4` owns the private work queue; the
   long-form research plan is in `memory.md`.
6. **Check before committing:** `bash tests/test_docs.sh` verifies that
   every relative link resolves and that no two documents repeat a
   paragraph (except the explicitly allow-listed one-sentence summaries).

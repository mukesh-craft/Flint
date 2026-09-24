# Compile-time baselines (2026-09-10 orig; 2026-09-13 P1.1, Termux AArch64, median of 3, wall clock)

Source: `bash bench/compile_time/bench.sh` (see script for method).

| Input | Bytes | C++ ms | Self parse ms | Self emit ms | Self total ms |
| small (common.fl) | 541 | 300 | 43 | 57 | 100 |
| lex (flint_lex.fl) | 48,408 | 306 | 276 | 1,411 | 1,687 |

## P1.1 (2026-09-13): reader bulk scans — TARGET MET

| Input | Bytes | C++ ms | Self parse ms | Self emit ms | Self total ms |
| small (common.fl) | 541 | 242 | 24 | 18 | 42 |
| lex (flint_lex.fl) | 56,990 | 280 | 313 | 35 | 349 |

What changed (cut per instrumented data: scan 637 ms + emit 897 ms on
28 KB S-expr; ~100K `e_skip` + 11.5K `e_word` calls, each several
Flint frames + C calls per character):
- `runtime.c`: `flint_str_skip_ws` / `flint_str_word_end` bulk scanners
  (one C call replaces N per-character round trips) + long-string
  `strlen` memo (8-entry, >= 256 bytes; a 1-entry cache measured ~0%
  hit rate under interleaved map-key `strlen`s).
- `src/main.cpp`: registered both fns for Flint calls.
- `stage3/flint_emit.fl`: `e_skip`/`e_word` rewritten on the bulk
  scanners (byte-identical output proven); `e_rt_sig` entries `1:3,1`.
- Result: lex emit 1,411 → 35 ms (40x), total 1,687 → 349 ms,
  under the 400 ms parity gate. Parse unchanged (stage1/2 readers
  are separate code — P1 follow-up if the gate tightens).

Note: Termux-class devices throttle; expect ±2x run variance. Always compare
same-session medians (the harness runs 3x). Earlier manual runs: small
95/40, lex 353/1624 — same conclusions.

Gates (P1 acceptance):
- lex-size total <= 400 ms (parity with C++). MET 2026-09-13 (349 ms).
- No single run regresses >10% vs this table (CI perf gate).
- Ladder 21/21 + differential green on every perf change.

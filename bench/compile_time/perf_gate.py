#!/usr/bin/env python3
"""Perf-ratio gate: compare bench.sh CSV ratios against BASELINE.md.

Absolute ms varies +/-2x on shared runners and throttled devices, so the
gate compares self-vs-C++ RATIO per input (runner speed cancels out):
    ratio = self_total_ms / cpp_ms
PASS when ratio <= baseline_ratio * 1.10 for every input row.

Usage: python3 bench/compile_time/perf_gate.py bench_result.csv
bench_result.csv is the stdout of bench/compile_time/bench.sh (with header).
Baselines (from BASELINE.md P1.1 table, 2026-09-13, Termux AArch64):
  small: cpp=242 self_total=42 -> 0.1736
  lex:   cpp=280 self_total=349 -> 1.2464
"""
import csv
import sys

BASELINE = {
    "small": 42.0 / 242.0,
    "lex": 349.0 / 280.0,
}
TOLERANCE = 1.10


def main(path):
    with open(path) as f:
        rows = list(csv.DictReader(f))
    if not rows:
        print("perf-gate: empty CSV")
        return 1
    failed = 0
    for r in rows:
        name = r["input"]
        cpp = float(r["cpp_ms"])
        total = float(r["self_total_ms"])
        if cpp <= 0:
            print("perf-gate: %s has non-positive cpp_ms, skip" % name)
            continue
        ratio = total / cpp
        base = BASELINE.get(name)
        if base is None:
            print("perf-gate: %s has no baseline, skip" % name)
            continue
        limit = base * TOLERANCE
        status = "PASS" if ratio <= limit else "FAIL"
        print("perf-gate: %s ratio=%.4f baseline=%.4f limit=%.4f %s"
              % (name, ratio, base, limit, status))
        if ratio > limit:
            failed += 1
    if failed:
        print("perf-gate: %d input(s) regressed >10%% vs baseline ratio" % failed)
        return 1
    print("perf-gate: all ratios within +10% of baseline")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "bench_result.csv"))

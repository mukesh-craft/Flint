#!/usr/bin/env bash
# The measurement gate: run the benchmarks, write a per-commit summary, and
# fail when a change moved a number past its threshold.
#
# This is the Phase 0 instrumentation the roadmap asks for: "every compiler
# change should be measurable". Without it, two regressions shipped green --
# a 130x binary-size regression and a 5x silent matmul slowdown.
#
# Baselines are PER MACHINE. The numbers are timings, and absolute timings
# are machine-specific (a published benchmark suite measured a 41% run-to-run
# spread on one laptop and 15% on another, while its verdicts reproduced
# exactly). So the baseline file records the machine it was taken on, and a
# baseline from another machine is refused rather than silently compared.
#
# Usage:
#   bash tools/bench_gate.sh                     # compare against the baseline
#   bash tools/bench_gate.sh --update            # (re)write the baseline
#   bash tools/bench_gate.sh --json out.json     # also write a summary
#
# Environment: BASELINE (default tools/bench_baseline.json), FLINTC.
set -u
cd "$(dirname "$0")/.."
FLINTC="${FLINTC:-./flintc}"
BASELINE="${BASELINE:-tools/bench_baseline.json}"
OUT_JSON=""
UPDATE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --update) UPDATE=1 ;;
        --json=*) OUT_JSON="${1#--json=}" ;;
        --json) shift; OUT_JSON="${1:-}" ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

TMPD="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/bench_gate"
mkdir -p "$TMPD"

# Thresholds (percent). A change past these fails the gate.
THRESH_RUNTIME=${THRESH_RUNTIME:-3}
THRESH_BINARY=${THRESH_BINARY:-5}
THRESH_COMPILE=${THRESH_COMPILE:-10}

machine_id() { # a stable id for "this machine"
    uname -s -m 2>/dev/null | tr ' ' '_'
}

# --- collect the current numbers -------------------------------------------
# Runtime: the sum_array workload (integer loop with checks) and the pi
# workload (f64 loop) -- the two that between them expose the check cost and
# the vectorization gap.
# The MINIMUM of N runs, not the mean: on a loaded machine the minimum is
# the least noisy estimate of the machine's real speed, and it is the
# standard estimator for benchmarks (a mean is dragged up by every
# unrelated process that happened to run).
run_one() { # $1=workload $2=reps -> ns (minimum)
    local w="$1" reps="$2" i best=0 t
    local bin="$TMPD/$w.bin"
    for i in $(seq 1 "$reps"); do
        "$FLINTC" "benchmarks/$w.fl" -o "$bin" >/dev/null 2>&1 || { echo 0; return; }
        t=$("$bin" 2>/dev/null | awk '/^time:/ {print $2; exit}')
        [ -z "$t" ] && t=0
        if [ "$best" = 0 ] || [ "$t" -lt "$best" ]; then best=$t; fi
    done
    echo "$best"
}

sum_ns=$(run_one sum_array 3)
pi_ns=$(run_one pi 2)

# Binary size: stripped hello-world.
printf 'fn main() {\n    print("hello")\n}\n' > "$TMPD/hello.fl"
"$FLINTC" "$TMPD/hello.fl" -o "$TMPD/hello.bin" >/dev/null 2>&1
hello_raw=$(stat -c %s "$TMPD/hello.bin" 2>/dev/null || echo 0)
cp "$TMPD/hello.bin" "$TMPD/hello.stripped" 2>/dev/null
strip "$TMPD/hello.stripped" >/dev/null 2>&1
hello_stripped=$(stat -c %s "$TMPD/hello.stripped" 2>/dev/null || echo 0)

# Compile time: three cold builds, the median.
# Cold-build cost: the minimum of 3 runs (see run_one for why minimum).
ct_best=0
for i in 1 2 3; do
    t0=$(date +%s%N)
    "$FLINTC" "$TMPD/hello.fl" -o "$TMPD/ct.bin" >/dev/null 2>&1
    t1=$(date +%s%N)
    t=$(( (t1 - t0) / 1000000 ))
    if [ "$ct_best" = 0 ] || [ "$t" -lt "$ct_best" ]; then ct_best=$t; fi
done
compile_ms=$ct_best

cat > "$TMPD/current.json" <<EOF
{
  "machine": "$(machine_id)",
  "sum_array_ns": ${sum_ns:-0},
  "pi_ns": ${pi_ns:-0},
  "hello_raw_bytes": ${hello_raw:-0},
  "hello_stripped_bytes": ${hello_stripped:-0},
  "compile_ms": ${compile_ms:-0}
}
EOF

# --- compare ----------------------------------------------------------------
status=0
if [ "$UPDATE" = "1" ] || [ ! -f "$BASELINE" ]; then
    cp "$TMPD/current.json" "$BASELINE"
    echo "bench-gate: baseline written to $BASELINE (machine $(machine_id))"
    [ "$UPDATE" = "1" ] || echo "bench-gate: no baseline existed; one was created -- run again to compare"
    status=0
else
    base_machine=$(python3 -c "import json;print(json.load(open('$BASELINE')).get('machine','?'))" 2>/dev/null)
    cur_machine=$(machine_id)
    if [ "$base_machine" != "$cur_machine" ]; then
        echo "bench-gate: SKIPPED -- the baseline was taken on '$base_machine', this is '$cur_machine'."
        echo "            Absolute timings are machine-specific; compare verdicts, not milliseconds."
        echo "            Re-take a baseline here with: bash tools/bench_gate.sh --update"
    else
        python3 - "$BASELINE" "$TMPD/current.json" "$THRESH_RUNTIME" "$THRESH_BINARY" "$THRESH_COMPILE" <<'PYEOF'
import json, sys
base = json.load(open(sys.argv[1])); cur = json.load(open(sys.argv[2]))
tr, tb, tc = (float(x) for x in sys.argv[3:6])
def pct(b, c):
    if not b: return 0.0
    return (c - b) * 100.0 / b
rows = [
    ("runtime sum_array", "sum_array_ns", tr, -1),
    ("runtime pi", "pi_ns", tr, -1),
    ("binary stripped", "hello_stripped_bytes", tb, 1),
    ("compile time", "compile_ms", tc, 1),
]
bad = 0
for label, key, thresh, direction in rows:
    b = base.get(key, 0); c = cur.get(key, 0)
    d = pct(b, c)
    # runtime: slower is worse (positive delta is bad). size/time: bigger is worse.
    if d > thresh:
        print("FAIL %-18s %s -> %s (%+.1f%%, threshold +%.0f%%)" % (label, b, c, d, thresh))
        bad += 1
    else:
        print("ok   %-18s %s -> %s (%+.1f%%)" % (label, b, c, d))
sys.exit(1 if bad else 0)
PYEOF
        status=$?
    fi
fi

[ -n "$OUT_JSON" ] && cp "$TMPD/current.json" "$OUT_JSON"
exit $status

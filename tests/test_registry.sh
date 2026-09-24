#!/usr/bin/env bash
# Registry end-to-end test (no network: file:// package + local git).
# Usage: bash tests/test_registry.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
export HOME="${HOME:-$HOME}"
T="$(mktemp -d 2>/dev/null || echo /tmp/flint-regtest-$$)"
trap 'rm -rf "$T"' EXIT
fail=0
expect() { # $1=desc $2=expected $3=actual
    if [ "$2" = "$3" ]; then echo "ok: $1";
    else echo "FAIL: $1 (want [$2] got [$3])"; fail=$((fail+1)); fi
}

mkdir -p "$T/pkg/calc" "$T/app"
printf 'fn cadd(a: i64, b: i64) -> i64 {\n    a + b\n}\n' > "$T/pkg/calc/calc.fl"
git init -q "$T/pkg/calc" || exit 1
git -C "$T/pkg/calc" -c user.email=t@t -c user.name=t add -A
git -C "$T/pkg/calc" -c user.email=t@t -c user.name=t commit -qm init
printf 'import "calc"\nfn main() -> i64 {\n    print(cadd(19, 23))\n    0\n}\n' > "$T/app/main.fl"
printf '[dependencies]\ncalc = "file://%s/pkg/calc"\n' "$T" > "$T/app/flint.toml"

out="$("$FLINTC" "$T/app/main.fl" 2>&1)"
expect "auto-fetch build prints 42" "42" "$(printf '%s' "$out" | tail -n 1)"
[ -f "$T/app/flint.lock" ] || { echo "FAIL: lockfile not written"; fail=$((fail+1)); }
grep -q 'calc.rev' "$T/app/flint.lock" 2>/dev/null || { echo "FAIL: lockfile lacks rev"; fail=$((fail+1)); }

# Cached rebuild: no fetch message, same output.
out2="$("$FLINTC" "$T/app/main.fl" 2>&1)"
expect "cached rebuild prints 42" "42" "$(printf '%s' "$out2" | tail -n 1)"
case "$out2" in *fetching*) echo "FAIL: refetched cached package"; fail=$((fail+1));; esac

# Offline with cache: works.
out3="$("$FLINTC" --offline "$T/app/main.fl" 2>&1)"
expect "offline cached prints 42" "42" "$(printf '%s' "$out3" | tail -n 1)"

# Offline without cache: clean error, nonzero exit.
rm -rf "${HOME}/.cache/flint_pkgs/calc"
"$FLINTC" --offline "$T/app/main.fl" > /dev/null 2>&1
[ $? -ne 0 ] && echo "ok: offline-missing fails nonzero" || { echo "FAIL: offline-missing exited 0"; fail=$((fail+1)); }

echo "registry: failures=$fail"
[ "$fail" -eq 0 ]

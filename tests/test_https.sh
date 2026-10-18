#!/usr/bin/env bash
# https gate: the net module must speak TLS, not just plain HTTP.
#
# Until this existed, an https:// URL was refused outright
# (runtime/flint_net.c returned false from parse_http_url), which made every
# modern API unreachable from Flint. The transfer now goes through the
# platform's libcurl.
#
# The gate is offline-safe by design: RFC 2606's .invalid TLD must not resolve,
# so the first checks run anywhere. The live request then reports what happened
# and only asserts the properties we control.
#
# Usage: bash tests/test_https.sh [path/to/flintc]   (default: ./flintc)
set -u
FLINTC="${1:-./flintc}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "PASS $1"; }
bad() { fail=$((fail+1)); echo "FAIL $1${2:+ — $2}"; }

if ! out="$(timeout -s KILL 120 "$FLINTC" tests/t_https.fl 2>&1)"; then
    bad "tests/t_https.fl runs" "$(printf '%s' "$out" | tail -n 3 | tr '\n' ' ')"
    echo "https: $pass passed, $fail failed"
    exit 1
fi

# 1: the unreachable host failed (NULL == 0 in Flint), as it must.
printf '%s\n' "$out" | grep -qxF "1" && ok "unreachable host fails fast (.invalid)" \
    || bad "unreachable host fails fast (.invalid)" "$(printf '%s' "$out" | tr '\n' ' ')"

printf '%s\n' "$out" | grep -qxF "0" && ok "status stays 0 for a failed request" \
    || bad "status stays 0 for a failed request"

printf '%s\n' "$out" | grep -qxF "https-ok" && ok "fixture completed" || bad "fixture completed"

# Live request: report, do not demand a specific answer. Offline prints
# "offline" and that is a pass, because the gate must not depend on a network.
if printf '%s\n' "$out" | grep -qxF "offline"; then
    ok "live request skipped (no network) — TLS path still compiled and linked"
elif printf '%s\n' "$out" | grep -qxF "200"; then
    ok "live https request returned 200"
elif printf '%s\n' "$out" | grep -qxE "[23][0-9][0-9]"; then
    ok "live https request returned a 2xx/3xx status"
else
    bad "live https request status" "$(printf '%s' "$out" | tr '\n' ' ')"
fi

echo "https: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
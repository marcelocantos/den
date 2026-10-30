#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Tests for scripts/index-disabled.py, which verify-release uses to refuse a
# fixture package that upstream has disabled: exit 0 with a message naming
# the package (and its reason, date and replacement when the index has them)
# for a disabled entry, 1 for an enabled or unknown one, 2 for a missing or
# unreadable index or bad usage.
#
# Run:
#   sh tests/scripts/test_index_disabled.sh
# or via ctest (name: script_test_index_disabled).

set -u

_SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
SCRIPT="${_SELF_DIR}/../../scripts/index-disabled.py"

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not found"
    exit 0
fi

_pass=0
_fail=0
_tmp=$(mktemp -d "${TMPDIR:-/tmp}/index-disabled-test.XXXXXX")
trap 'rm -rf "$_tmp"' EXIT

ok() { _pass=$((_pass + 1)); echo "PASS: $1"; }
bad() { _fail=$((_fail + 1)); echo "FAIL: $1"; [ -f "$_tmp/out" ] && sed 's/^/    /' "$_tmp/out"; }

# check NAME WANT_RC WANT_OUT ARGS...: run the script; WANT_OUT, if not
# empty, must be its exact stdout.
check() {
    _name=$1; _want=$2; _wout=$3; shift 3
    _rc=0
    python3 "$SCRIPT" "$@" >"$_tmp/out" 2>"$_tmp/err" || _rc=$?
    if [ "$_rc" -ne "$_want" ]; then bad "$_name: exit $_rc, want $_want"; return; fi
    if [ -n "$_wout" ] && [ "$(cat "$_tmp/out")" != "$_wout" ]; then
        bad "$_name: stdout '$(cat "$_tmp/out")', want '$_wout'"; return
    fi
    ok "$_name"
}

cat >"$_tmp/index.json" <<'JSON'
{"packages": [
  {"name": "hex-fiend", "artifact_type": "app", "disabled": false},
  {"name": "macdown", "artifact_type": "app", "disabled": true,
   "disable_reason": "fails_gatekeeper_check", "disable_date": "2026-09-01"},
  {"name": "oldapp", "artifact_type": "app", "disabled": true,
   "disable_replacement": "newapp"},
  {"name": "bare", "disabled": true},
  {"name": "nokey"}
]}
JSON

check "disabled: reason and date" 0 \
    "'macdown' is disabled upstream: fails_gatekeeper_check (disabled on 2026-09-01)" \
    "$_tmp/index.json" macdown
check "disabled: replacement" 0 "'oldapp' is disabled upstream; use newapp instead" \
    "$_tmp/index.json" oldapp
check "disabled: no details" 0 "'bare' is disabled upstream" "$_tmp/index.json" bare
check "enabled" 1 "" "$_tmp/index.json" hex-fiend
check "no disabled key" 1 "" "$_tmp/index.json" nokey
check "not in the index" 1 "" "$_tmp/index.json" nosuch
check "missing index" 2 "" "$_tmp/none.json" macdown
printf 'not json' >"$_tmp/bad.json"
check "unreadable index" 2 "" "$_tmp/bad.json" macdown
printf '{"formulae": []}' >"$_tmp/nopk.json"
check "index without packages" 2 "" "$_tmp/nopk.json" macdown
check "usage" 2 "" "$_tmp/index.json"

echo
echo "${_pass} passed, ${_fail} failed"
[ "$_fail" -eq 0 ]

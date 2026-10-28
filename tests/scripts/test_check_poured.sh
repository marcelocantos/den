#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Tests for scripts/check-poured.sh (verify-release's `poured` step): the
# install receipt and `brew info --json=v2` must both report that the keg
# was poured from a bottle.
#
# Run:
#   sh tests/scripts/test_check_poured.sh
# or via ctest (name: script_test_check_poured).

set -u

_SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT="${_SELF_DIR}/../.."
SCRIPT="${ROOT}/scripts/check-poured.sh"

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not found"
    exit 0
fi

_pass=0
_fail=0
_tmp=$(mktemp -d "${TMPDIR:-/tmp}/check-poured-test.XXXXXX")
trap 'rm -rf "$_tmp"' EXIT

# expect NAME WANT_EXIT ARGS... (BREW comes from the environment)
expect() {
    _name="$1"
    _want="$2"
    shift 2
    sh "$SCRIPT" "$@" >"$_tmp/out" 2>&1
    _got=$?
    if [ "$_got" -eq "$_want" ]; then
        _pass=$((_pass + 1))
        echo "PASS: $_name"
    else
        _fail=$((_fail + 1))
        echo "FAIL: $_name (exit $_got, want $_want)"
        sed 's/^/    /' "$_tmp/out"
    fi
}

# keg NAME VERSION RECEIPT_JSON — Cellar-shaped keg plus an opt symlink.
keg() {
    mkdir -p "$_tmp/$1/Cellar/den/$2/bin" "$_tmp/$1/opt"
    [ -n "$3" ] && printf '%s\n' "$3" >"$_tmp/$1/Cellar/den/$2/INSTALL_RECEIPT.json"
    ln -s "../Cellar/den/$2" "$_tmp/$1/opt/den"
}

# fakebrew NAME JSON|fail — a brew whose `info --json=v2 X` prints JSON.
fakebrew() {
    if [ "$2" = fail ]; then
        printf '#!/bin/sh\necho "Error: no such formula" >&2\nexit 1\n' >"$_tmp/$1"
    else
        printf '#!/bin/sh\n[ "$1 $2" = "info --json=v2" ] || exit 9\ncat <<JSON\n%s\nJSON\n' "$2" >"$_tmp/$1"
    fi
    chmod 755 "$_tmp/$1"
}

poured='{"built_as_bottle":true,"poured_from_bottle":true,"changed_files":[]}'
keg good 1.2.0 "$poured"
keg tarball 1.2.0 '{"built_as_bottle":false,"poured_from_bottle":false}'
keg prepour 1.2.0 '{"built_as_bottle":true,"poured_from_bottle":false}'
keg missing 1.2.0 ""
keg garbage 1.2.0 'not json'
keg nullfield 1.2.0 '{"built_as_bottle":true}'

info() { printf '{"formulae":[{"name":"den","installed":[{"version":"%s","poured_from_bottle":%s}]}],"casks":[]}' "$1" "$2"; }
fakebrew brew-poured "$(info 1.2.0 true)"
fakebrew brew-source "$(info 1.2.0 false)"
fakebrew brew-other "$(info 1.1.0 true)"
fakebrew brew-none '{"formulae":[{"name":"den","installed":[]}],"casks":[]}'
fakebrew brew-fail fail

export BREW="$_tmp/brew-fail" # receipt-only cases must not call brew

expect "usage: no args" 2
expect "usage: too many args" 2 a b c

expect "poured receipt (keg path)" 0 "$_tmp/good/Cellar/den/1.2.0"
expect "poured receipt (opt symlink)" 0 "$_tmp/good/opt/den"
expect "tarball install rejected" 1 "$_tmp/tarball/opt/den"
expect "unpoured bottle receipt rejected" 1 "$_tmp/prepour/opt/den"
expect "missing receipt rejected" 1 "$_tmp/missing/opt/den"
expect "malformed receipt rejected" 1 "$_tmp/garbage/opt/den"
expect "receipt without the field rejected" 1 "$_tmp/nullfield/opt/den"
expect "nonexistent prefix rejected" 1 "$_tmp/nope"

BREW="$_tmp/brew-poured" expect "receipt + brew info agree" 0 "$_tmp/good/opt/den" marcelocantos/tap/den
BREW="$_tmp/brew-source" expect "brew info says not poured" 1 "$_tmp/good/opt/den" marcelocantos/tap/den
BREW="$_tmp/brew-other" expect "brew info lists another version" 1 "$_tmp/good/opt/den" marcelocantos/tap/den
BREW="$_tmp/brew-none" expect "brew info lists nothing installed" 1 "$_tmp/good/opt/den" marcelocantos/tap/den
BREW="$_tmp/brew-fail" expect "brew info fails" 1 "$_tmp/good/opt/den" marcelocantos/tap/den
BREW="$_tmp/brew-poured" expect "brew info cannot rescue a bad receipt" 1 "$_tmp/tarball/opt/den" marcelocantos/tap/den

# verify-release refuses VR_POURED=1 when den did not come from brew.
printf '#!/bin/sh\necho 1.2.0\n' >"$_tmp/den"
chmod 755 "$_tmp/den"
DEN_BIN="$_tmp/den" VR_POURED=1 RELEASE_TAG=v1.2.0 bash "$ROOT/scripts/verify-release.sh" >"$_tmp/out" 2>&1
if [ $? -eq 2 ] && grep -q "VR_POURED=1 needs a brew-installed den" "$_tmp/out"; then
    _pass=$((_pass + 1)); echo "PASS: verify-release rejects VR_POURED=1 with DEN_BIN"
else
    _fail=$((_fail + 1)); echo "FAIL: verify-release rejects VR_POURED=1 with DEN_BIN"; sed 's/^/    /' "$_tmp/out"
fi
grep -q 'step poured s_poured' "$ROOT/scripts/verify-release.sh" \
    && { _pass=$((_pass + 1)); echo "PASS: verify-release runs the poured step"; } \
    || { _fail=$((_fail + 1)); echo "FAIL: verify-release runs the poured step"; }

echo ""
echo "Results: ${_pass} passed, ${_fail} failed"
[ "$_fail" -eq 0 ]

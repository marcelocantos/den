#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Regression tests for scripts/prev-release-tag.sh: release notes must diff
# against the highest final vX.Y.Z strictly below the new version, never an
# RC or a higher (e.g. stray v1.0.0-rc.1) tag.
#
# Run:
#   sh tests/scripts/test_prev_release_tag.sh
# or via ctest (name: script_test_prev_release_tag).

set -u

_SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
SCRIPT="${_SELF_DIR}/../../scripts/prev-release-tag.sh"

_pass=0
_fail=0

# expect NAME VERSION WANT_OUTPUT TAGS...
expect() {
    _name="$1"
    _version="$2"
    _want="$3"
    shift 3
    _got=$(printf '%s\n' "$@" | sh "$SCRIPT" "$_version" 2>&1)
    _rc=$?
    if [ "$_rc" -eq 0 ] && [ "$_got" = "$_want" ]; then
        _pass=$((_pass + 1))
        echo "PASS: $_name"
    else
        _fail=$((_fail + 1))
        echo "FAIL: $_name (exit $_rc, got '$_got', want '$_want')"
    fi
}

# The release-candidate dry run for 0.14.0 picked v1.0.0-rc.1 as the
# prior tag; this is that tag set.
expect "picks v0.13.0 for 0.14.0" 0.14.0 v0.13.0 \
    v0.12.0 v0.13.0 v0.13.0-rc.3 v0.14.0-rc.1 v1.0.0-rc.1

expect "rc tags are ignored" 0.14.0 v0.12.0 \
    v0.12.0 v0.13.0-rc.1 v0.13.0-rc.2 v0.13.0-rc.3

expect "higher tags are ignored" 0.14.0 v0.13.0 \
    v0.13.0 v0.15.0 v1.0.0 v1.0.0-rc.1

expect "target itself is ignored" 0.14.0 v0.13.0 \
    v0.13.0 v0.14.0

expect "no previous tag (empty list)" 0.14.0 ""

expect "no previous tag (only rc/higher)" 0.14.0 "" \
    v0.14.0-rc.1 v1.0.0-rc.1 v0.15.0

expect "numeric, not lexical, ordering" 0.14.0 v0.10.0 \
    v0.9.0 v0.10.0 v0.2.0

expect "patch releases count" 0.14.0 v0.13.2 \
    v0.13.0 v0.13.1 v0.13.2 v0.13.2-rc.1

expect "other pre-release and junk ignored" 0.14.0 v0.12.0 \
    v0.12.0 v0.13.0-beta.1 v0.13.0.1 0.13.0 vx.y.z latest

expect "accepts v prefix and rc suffix" v0.14.0-rc.2 v0.13.0 \
    v0.12.0 v0.13.0 v0.14.0-rc.1

expect "unsorted input" 1.0.0 v0.13.0 \
    v0.13.0 v0.1.0 v0.12.5 v1.0.0-rc.1

# Malformed target must fail loudly rather than silently choose nothing.
if printf 'v0.13.0\n' | sh "$SCRIPT" 0.14 >/dev/null 2>&1; then
    _fail=$((_fail + 1))
    echo "FAIL: malformed version rejected"
else
    _pass=$((_pass + 1))
    echo "PASS: malformed version rejected"
fi

# --- --all: every earlier tag, RCs included, newest first ------------------
# expect_all NAME VERSION WANT (space-separated) TAGS...
expect_all() {
    _name="$1"
    _version="$2"
    _want="$3"
    shift 3
    _got=$(printf '%s\n' "$@" | sh "$SCRIPT" --all "$_version" 2>&1 | tr '\n' ' ' | sed 's/ $//')
    _rc=$?
    if [ "$_rc" -eq 0 ] && [ "$_got" = "$_want" ]; then
        _pass=$((_pass + 1))
        echo "PASS: --all $_name"
    else
        _fail=$((_fail + 1))
        echo "FAIL: --all $_name (exit $_rc, got '$_got', want '$_want')"
    fi
}
expect_all "rc target: own earlier rcs first" v0.15.0-rc.3 \
    "v0.15.0-rc.2 v0.15.0-rc.1 v0.14.1 v0.14.0 v0.14.0-rc.1 v0.13.0" \
    v0.13.0 v0.14.0-rc.1 v0.14.0 v0.14.1 v0.15.0-rc.1 v0.15.0-rc.2 v0.15.0-rc.3 \
    v0.15.0-rc.4 v0.15.0 v1.0.0-rc.1
expect_all "ga target: its rcs are earlier" 0.15.0 \
    "v0.15.0-rc.10 v0.15.0-rc.2 v0.14.0" \
    v0.14.0 v0.15.0-rc.2 v0.15.0-rc.10 v0.15.0
expect_all "junk and duplicates ignored" 0.14.0 "v0.13.0 v0.12.0" \
    v0.12.0 v0.13.0 v0.13.0 v0.13.0-beta.1 latest 0.13.0
expect_all "nothing earlier" 0.1.0 "" v0.1.0 v0.2.0-rc.1

echo ""
echo "prev-release-tag: ${_pass} passed, ${_fail} failed"
[ "$_fail" -eq 0 ]

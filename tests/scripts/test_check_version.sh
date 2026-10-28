#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Regression tests for scripts/check-version.sh: a release run must fail
# when the requested version disagrees with CMake project(VERSION) or with
# what the built binary reports via --version.
#
# Run:
#   sh tests/scripts/test_check_version.sh
# or via ctest (name: script_test_check_version).

set -u

_SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
SCRIPT="${_SELF_DIR}/../../scripts/check-version.sh"

_pass=0
_fail=0
_tmp=$(mktemp -d "${TMPDIR:-/tmp}/check-version-test.XXXXXX")
trap 'rm -rf "$_tmp"' EXIT

# expect NAME WANT_EXIT ARGS...
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

cml="$_tmp/CMakeLists.txt"
cat >"$cml" <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(den VERSION 0.14.0 LANGUAGES C CXX)
CMAKE

fake_bin() {
    # fake_bin PATH OUTPUT
    printf '#!/bin/sh\necho "%s"\n' "$2" >"$1"
    chmod +x "$1"
}
fake_bin "$_tmp/den-0.14.0" "0.14.0"
fake_bin "$_tmp/den-0.13.0" "0.13.0"
printf '#!/bin/sh\nexit 3\n' >"$_tmp/den-broken"
chmod +x "$_tmp/den-broken"

# --- CMake project(VERSION) vs requested version ---
expect "cmake match"                    0 --cmakelists "$cml" 0.14.0
expect "cmake match with v prefix"      0 --cmakelists "$cml" v0.14.0
expect "cmake match with rc suffix"     0 --cmakelists "$cml" v0.14.0-rc.3
expect "cmake mismatch (stale bump)"    1 --cmakelists "$cml" 0.15.0
expect "cmake mismatch (older)"         1 --cmakelists "$cml" 0.13.0
expect "cmake mismatch (patch differs)" 1 --cmakelists "$cml" 0.14.1
expect "malformed version"              2 --cmakelists "$cml" 0.14
expect "missing CMakeLists"             2 --cmakelists "$_tmp/nope.txt" 0.14.0
expect "no version argument"            2 --cmakelists "$cml"

# --- binary --version vs requested version ---
expect "binary match"                   0 --cmakelists "$cml" --binary "$_tmp/den-0.14.0" 0.14.0
expect "binary mismatch"                1 --cmakelists "$cml" --binary "$_tmp/den-0.13.0" 0.14.0
expect "binary mismatch, skip cmake"    1 --skip-cmake --binary "$_tmp/den-0.13.0" v0.14.0
expect "binary match, skip cmake"       0 --skip-cmake --binary "$_tmp/den-0.14.0" v0.14.0-rc.1
expect "binary missing"                 1 --skip-cmake --binary "$_tmp/absent" 0.14.0
expect "binary --version fails"         1 --skip-cmake --binary "$_tmp/den-broken" 0.14.0
expect "cmake mismatch wins over binary" 1 --cmakelists "$cml" --binary "$_tmp/den-0.14.0" 0.15.0

echo ""
echo "check-version: ${_pass} passed, ${_fail} failed"
[ "$_fail" -eq 0 ]

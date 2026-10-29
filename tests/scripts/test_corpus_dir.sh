#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Regression tests for cmake/corpus_dir.cmake: where the tests find
# homebrew-core. DEN_CORPUS_DIR (-D cache value, then environment) names a
# shared checkout, which must be at the commit the source tree pins; unset,
# the tests/corpus/homebrew-core submodule is used. Also covers
# cmake/check_corpus.cmake (the corpus_pin ctest).
#
# Fixtures are throwaway git repos: a "corpus" with two commits and a
# "source tree" whose index pins the first as tests/corpus/homebrew-core.
#
# Run:
#   sh tests/scripts/test_corpus_dir.sh
# or via ctest (name: script_test_corpus_dir).

set -u

_SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
CMAKE_DIR="${_SELF_DIR}/../../cmake"

_pass=0
_fail=0
_tmp=$(mktemp -d "${TMPDIR:-/tmp}/corpus-dir-test.XXXXXX")
trap 'rm -rf "$_tmp"' EXIT
_tmp=$(CDPATH= cd -- "$_tmp" && pwd -P)

ok() { _pass=$((_pass + 1)); printf 'ok   %s\n' "$1"; }
not_ok() {
    _fail=$((_fail + 1))
    printf 'FAIL %s\n' "$1"
    [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/     /'
}

g() { git -c user.name=t -c user.email=t@example.com -c init.defaultBranch=main "$@"; }

# Corpus: commit A (pinned) and commit B (newer).
corpus="$_tmp/shared/homebrew-core"
mkdir -p "$corpus/Formula/a"
g init -q "$corpus"
echo 'class A < Formula; end' >"$corpus/Formula/a/a.rb"
g -C "$corpus" add -A && g -C "$corpus" commit -qm A
pin=$(git -C "$corpus" rev-parse HEAD)
echo '# newer' >>"$corpus/Formula/a/a.rb"
g -C "$corpus" commit -qam B
newer=$(git -C "$corpus" rev-parse HEAD)
g -C "$corpus" checkout -q --detach "$pin"

# Source tree pinning commit A, without cloning the submodule.
src="$_tmp/src"
g init -q "$src"
g -C "$src" update-index --add --cacheinfo "160000,$pin,tests/corpus/homebrew-core"
g -C "$src" commit -qm pin

probe="$_tmp/probe.cmake"
cat >"$probe" <<'CM'
include("${CMAKE_DIR}/corpus_dir.cmake")
den_resolve_corpus_dir(dir "${SRC}" "${OVERRIDE}")
message(STATUS "CORPUS=${dir}")
CM

# resolve <override> [env value]: prints cmake's output, exit status in $rc.
resolve() {
    if [ $# -ge 2 ]; then
        out=$(DEN_CORPUS_DIR="$2" cmake -DCMAKE_DIR="$CMAKE_DIR" -DSRC="$src" \
            -DOVERRIDE="$1" -P "$probe" 2>&1)
    else
        out=$(env -u DEN_CORPUS_DIR cmake -DCMAKE_DIR="$CMAKE_DIR" -DSRC="$src" \
            -DOVERRIDE="$1" -P "$probe" 2>&1)
    fi
    rc=$?
}

expect_corpus() { # name expected
    if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qxF -- "-- CORPUS=$2"; then
        ok "$1"
    else
        not_ok "$1" "rc=$rc expected CORPUS=$2
$out"
    fi
}

expect_error() { # name needle...
    name=$1
    shift
    if [ "$rc" -eq 0 ]; then
        not_ok "$name" "expected a configure error
$out"
        return
    fi
    for needle in "$@"; do
        if ! printf '%s\n' "$out" | grep -qF -- "$needle"; then
            not_ok "$name" "missing '$needle' in:
$out"
            return
        fi
    done
    ok "$name"
}

resolve ""
expect_corpus "unset: falls back to the submodule" "$src/tests/corpus/homebrew-core"

resolve "" ""
expect_corpus "empty env: falls back to the submodule" "$src/tests/corpus/homebrew-core"

resolve "" "$corpus"
expect_corpus "env override at the pinned commit is honoured" "$corpus"

resolve "$corpus"
expect_corpus "cache override at the pinned commit is honoured" "$corpus"

resolve "$corpus" "$_tmp/nowhere"
expect_corpus "cache override beats the environment" "$corpus"

resolve "../shared/homebrew-core"
expect_corpus "relative override resolves against the source tree" "$corpus"

resolve "$_tmp/nowhere"
expect_error "override without Formula/ fails" "has no Formula/ directory"

g -C "$corpus" checkout -q --detach "$newer"
resolve "" "$corpus"
expect_error "override at the wrong commit fails" "is at $newer" "pins homebrew-core at $pin" \
    "fetch --depth 1 origin $pin"

out=$(cmake -DCORPUS="$corpus" -DSRC="$src" -P "$CMAKE_DIR/check_corpus.cmake" 2>&1)
rc=$?
expect_error "corpus_pin check fails at the wrong commit" "is at $newer"

g -C "$corpus" checkout -q --detach "$pin"
out=$(cmake -DCORPUS="$corpus" -DSRC="$src" -P "$CMAKE_DIR/check_corpus.cmake" 2>&1)
rc=$?
if [ "$rc" -eq 0 ]; then ok "corpus_pin check passes at the pinned commit"; else
    not_ok "corpus_pin check passes at the pinned commit" "$out"; fi

# A source tree that is not a git checkout (a release tarball) cannot say
# what it pins: the override is used, with a warning.
plain="$_tmp/plain"
mkdir -p "$plain"
out=$(env -u DEN_CORPUS_DIR cmake -DCMAKE_DIR="$CMAKE_DIR" -DSRC="$plain" \
    -DOVERRIDE="$corpus" -P "$probe" 2>&1)
rc=$?
if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qF "cannot compare its commit" &&
    printf '%s\n' "$out" | grep -qxF -- "-- CORPUS=$corpus"; then
    ok "unknown pin: override used with a warning"
else
    not_ok "unknown pin: override used with a warning" "$out"
fi

printf '\n%d passed, %d failed\n' "$_pass" "$_fail"
[ "$_fail" -eq 0 ]

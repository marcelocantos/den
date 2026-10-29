#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Tests for scripts/check-no-brew-release.sh: the real tree must pass, and a
# scratch tree seeded with each kind of violation (in a workflow or the
# Makefile) must fail and name the offending line.
#
# Run:
#   sh tests/scripts/test_no_brew_release.sh
# or via ctest (name: script_test_no_brew_release).

set -u

_SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT=$(CDPATH= cd -- "${_SELF_DIR}/../.." && pwd)
SCRIPT="${ROOT}/scripts/check-no-brew-release.sh"

_pass=0
_fail=0
ok() { _pass=$((_pass + 1)); echo "PASS: $1"; }
bad() { _fail=$((_fail + 1)); echo "FAIL: $1"; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# A clean fixture tree: a workflow and a Makefile that mention den's runtime
# use of bottles (allowed) but nothing from the retired release path.
clean_tree() {
    rm -rf "$work/t"
    mkdir -p "$work/t/.github/workflows"
    cat >"$work/t/.github/workflows/ci.yml" <<'YML'
name: CI
jobs:
  check:
    steps:
      - run: brew install cmake ninja libarchive
      # downloads the bottle from GHCR and re-verifies it
      # the actual bottle downloaded from GHCR
      - run: gh release upload "$TAG" den-1.0.0-darwin-aarch64.tar.gz
YML
    printf 'verify-release:\n\tscripts/verify-release.sh $(RELEASE_TAG)\n' >"$work/t/Makefile"
}

# The real repository is clean.
if out=$(sh "$SCRIPT" "$ROOT" 2>&1); then ok "repo is clean"; else bad "repo is clean: $out"; fi

clean_tree
if out=$(sh "$SCRIPT" "$work/t" 2>&1); then ok "clean fixture passes"; else bad "clean fixture passes: $out"; fi

# expect_hit NAME FILE LINE: append LINE to FILE in a fresh clean tree; the
# guard must exit 1 and report FILE.
expect_hit() {
    clean_tree
    printf '%s\n' "$3" >>"$work/t/$2"
    out=$(sh "$SCRIPT" "$work/t" 2>&1)
    rc=$?
    if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -qF "$2:"; then
        ok "$1"
    else
        bad "$1 (exit $rc): $out"
    fi
}

expect_hit "tapper in a workflow" .github/workflows/ci.yml '      - run: tapper push --version "$TAG"'
expect_hit "release-tap in the Makefile" Makefile 'release-tap:'
expect_hit "release-tap script in a workflow" .github/workflows/ci.yml '      - run: scripts/release-tap.sh v1.0.0'
expect_hit "marcelocantos/tap" .github/workflows/ci.yml '      - run: brew install marcelocantos/tap/den'
expect_hit "marcelocantos/homebrew-tap" Makefile '# publish to marcelocantos/homebrew-tap'
expect_hit "homebrew-bottles.py" .github/workflows/ci.yml '      - run: python3 scripts/homebrew-bottles.py build'
expect_hit "bottle upload" .github/workflows/ci.yml '      - run: gh release upload "$TAG" dist/den-1.0.0.arm64_sonoma.bottle.tar.gz'
expect_hit "bottle block" Makefile '#   bottle do'
expect_hit "case-insensitive" .github/workflows/ci.yml '      - name: Publish via Tapper'

# scripts/verify-release.sh is scanned: its tap step must use the fixture tap.
clean_tree
mkdir -p "$work/t/scripts"
printf 's_tap() {\n    den tap add den-fixture/verify-release "$src"\n}\n' >"$work/t/scripts/verify-release.sh"
if out=$(sh "$SCRIPT" "$work/t" 2>&1); then ok "clean verify-release.sh passes"; else bad "clean verify-release.sh passes: $out"; fi
printf 'VR_TAP=${VR_TAP:-marcelocantos/tap}\n' >>"$work/t/scripts/verify-release.sh"
out=$(sh "$SCRIPT" "$work/t" 2>&1); rc=$?
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -qF "scripts/verify-release.sh:4:"; then
    ok "marcelocantos/tap in verify-release.sh"
else
    bad "marcelocantos/tap in verify-release.sh (exit $rc): $out"
fi

# A .yaml workflow is scanned too.
clean_tree
printf 'x: tapper\n' >"$work/t/.github/workflows/other.yaml"
if sh "$SCRIPT" "$work/t" >/dev/null 2>&1; then bad ".yaml workflow scanned"; else ok ".yaml workflow scanned"; fi

# No workflows and no Makefile is a usage error, not a pass.
mkdir -p "$work/empty"
sh "$SCRIPT" "$work/empty" >/dev/null 2>&1
rc=$?
if [ "$rc" -eq 2 ]; then ok "empty root is an error"; else bad "empty root is an error (exit $rc)"; fi

echo "${_pass} passed, ${_fail} failed"
[ "$_fail" -eq 0 ]

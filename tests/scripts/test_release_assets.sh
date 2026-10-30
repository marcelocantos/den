#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Tests for scripts/check-release-assets.py: the real tree must pass, and a
# scratch copy of the real workflows with a release step widened back to a
# bare glob, a non-den asset added, or a den asset dropped must fail and name
# the offending file.
#
# Run:
#   sh tests/scripts/test_release_assets.sh
# or via ctest (name: script_test_release_assets).

set -u

_SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT=$(CDPATH= cd -- "${_SELF_DIR}/../.." && pwd)
SCRIPT="${ROOT}/scripts/check-release-assets.py"

if ! command -v python3 >/dev/null 2>&1; then
    echo "FAIL: python3 not found (the release-asset guard needs it)"
    exit 1
fi

_pass=0
_fail=0
ok() { _pass=$((_pass + 1)); echo "PASS: $1"; }
bad() { _fail=$((_fail + 1)); echo "FAIL: $1"; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fresh_tree() {
    rm -rf "$work/t"
    mkdir -p "$work/t/.github" "$work/t/scripts"
    cp -R "$ROOT/.github/workflows" "$work/t/.github/workflows"
}

# swap FILE OLD NEW: replace the one occurrence of OLD in FILE (under the
# scratch tree) with NEW; aborts the test if OLD is not there exactly once,
# so a workflow edit cannot silently turn a case into a no-op.
swap() {
    SWAP_FILE="$work/t/$1" SWAP_OLD="$2" SWAP_NEW="$3" python3 - <<'PY' || exit 3
import os, sys
p, a, b = os.environ["SWAP_FILE"], os.environ["SWAP_OLD"], os.environ["SWAP_NEW"]
s = open(p).read()
if s.count(a) != 1:
    sys.exit(f"swap: {a!r} occurs {s.count(a)} times in {p}")
open(p, "w").write(s.replace(a, b))
PY
}

# expect_fail NAME FILE TEXT: the guard on the scratch tree must exit 1 and
# report FILE with a message containing TEXT.
expect_fail() {
    out=$(python3 "$SCRIPT" "$work/t" 2>&1)
    rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -F "$2:" | grep -qF "$3"; then
        ok "$1"
    else
        bad "$1 (exit $rc): $out"
    fi
}

P=.github/workflows/promote-rc.yml
RC=.github/workflows/release-candidate.yml
R=.github/workflows/release.yml

# The real repository is clean.
if out=$(python3 "$SCRIPT" "$ROOT" 2>&1); then ok "repo is clean"; else bad "repo is clean: $out"; fi

fresh_tree
if out=$(python3 "$SCRIPT" "$work/t" 2>&1); then ok "copied workflows pass"; else bad "copied workflows pass: $out"; fi

# The v0.15.0 promote path: '*.tar.gz' also fetched and published the RC's
# harness log.
fresh_tree
swap "$P" '--pattern "den-${VERSION}-*.tar.gz" \' "--pattern '*.tar.gz' \\"
expect_fail "promote download *.tar.gz" "$P" "harness-linux-logs-v0.15.0-rc.5.tar.gz"

fresh_tree
swap "$P" 'rc-assets/den-"${VERSION}"-*.tar.gz \' 'rc-assets/*.tar.gz \'
expect_fail "promote create *.tar.gz" "$P" "harness-linux-logs-v0.15.0-rc.5.tar.gz"

fresh_tree
swap "$P" 'rc-assets/den-"${VERSION}"-*.tar.gz.sha256 \' 'rc-assets/*.sha256 \'
expect_fail "promote create *.sha256" "$P" "also matches non-den assets"

fresh_tree
swap "$RC" 'dist/den-"${VERSION}"-*.tar.gz \' 'dist/*.tar.gz \'
expect_fail "rc create dist/*.tar.gz" "$RC" "harness-linux-logs"

# The harness log is allowed on the RC by name, never on GA.
fresh_tree
swap "$P" '"rc-assets/den-${VERSION}-checksums.txt"' '"rc-assets/den-${VERSION}-checksums.txt" \
            "rc-assets/harness-linux-logs-${RC_TAG}.tar.gz"'
expect_fail "harness log named on GA" "$P" "is not a den release asset"

# Dropping a den asset is caught too.
fresh_tree
swap "$P" ' \
            "rc-assets/den-${VERSION}-checksums.txt"' ''
expect_fail "GA without checksums" "$P" "do not publish den-0.15.0-checksums.txt"

fresh_tree
swap "$RC" '
            dist/den-"${VERSION}"-*.tar.gz.sha256 \' ''
expect_fail "RC without .sha256 files" "$RC" "den-0.15.0-linux-x86_64.tar.gz.sha256"

# The old release.yml loop downloaded whatever `gh release view` listed.
fresh_tree
swap "$R" '-p "den-${version}-*.tar.gz.sha256"' '-p "$asset"'
expect_fail "unresolvable download pattern" "$R" "cannot be resolved"

fresh_tree
swap "$R" '-p "den-${version}-*.tar.gz.sha256" ' ''
expect_fail "download without --pattern" "$R" "without --pattern"

# Any workflow or script that uploads is scanned.
fresh_tree
printf 'jobs:\n  x:\n    steps:\n      - run: gh release upload "$TAG" notes.md\n' \
    >"$work/t/.github/workflows/extra.yaml"
expect_fail "other workflow uploads a non-den file" .github/workflows/extra.yaml "is not a den release asset"

fresh_tree
printf '#!/bin/sh\ngh release upload "$1" \\\n    out/*.tar.gz\n' >"$work/t/scripts/publish.sh"
expect_fail "script uploads *.tar.gz" scripts/publish.sh "also matches non-den assets"

# Nothing to scan is a usage error, not a pass.
mkdir -p "$work/empty"
python3 "$SCRIPT" "$work/empty" >/dev/null 2>&1
rc=$?
if [ "$rc" -eq 2 ]; then ok "empty root is an error"; else bad "empty root is an error (exit $rc)"; fi

echo "${_pass} passed, ${_fail} failed"
[ "$_fail" -eq 0 ]

#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Tests for scripts/verify-release.sh's tap step:
#   * the script never refers to marcelocantos/tap (or homebrew-tap), and the
#     tap name, formula and source are not caller-overridable knobs;
#   * the fixture tap (tests/fixtures/den-fixture-tap) is in this checkout;
#   * with DEN_TEST_BIN (the den under test, set by ctest), the tap step runs
#     for real — tap add, --list, info, install, uninstall, tap --remove —
#     against that den in a scratch DEN_HOME/prefix under a temp dir, with a
#     stub installer that puts DEN_TEST_BIN where install.sh would. No
#     network and no Homebrew are needed: the fixture's download is file://.
#
# Run:
#   DEN_TEST_BIN=build/den sh tests/scripts/test_verify_release_tap.sh
# or via ctest (name: script_test_verify_release_tap).

set -u

_SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT=$(CDPATH= cd -- "${_SELF_DIR}/../.." && pwd)
VR="${ROOT}/scripts/verify-release.sh"
FIXTURE="${ROOT}/tests/fixtures/den-fixture-tap"

_pass=0
_fail=0
ok() { _pass=$((_pass + 1)); echo "PASS: $1"; }
bad() { _fail=$((_fail + 1)); echo "FAIL: $1"; }

# --- static --------------------------------------------------------------
if grep -n -i -E 'marcelocantos/(homebrew-)?tap' "$VR"; then
    bad "verify-release.sh mentions marcelocantos/tap"
else
    ok "verify-release.sh never mentions marcelocantos/tap"
fi
# The tap step's body (s_tap and render_fixture_tap) uses the fixture.
body=$(sed -n '/^render_fixture_tap() {/,/^}/p; /^s_tap() {/,/^}/p' "$VR")
if [ -n "$body" ] && printf '%s\n' "$body" | grep -q 'den tap add "\$VR_TAP" "\$src"'; then
    ok "s_tap adds the rendered fixture tap"
else
    bad "s_tap adds the rendered fixture tap"
fi
if printf '%s\n' "$body" | grep -i -E 'github|https?://|git clone|marcelocantos/'; then
    bad "tap step has no remote source"
else
    ok "tap step has no remote source"
fi
# No caller knob: VR_TAP and friends are assigned outright.
if grep -E '^VR_TAP(_FORMULA|_FIXTURE|_VERSION)?=\$\{' "$VR"; then
    bad "tap settings are not caller-overridable"
else
    ok "tap settings are not caller-overridable"
fi
for f in Formula/den-vr-hello.rb.in payload/den-vr-hello; do
    [ -f "$FIXTURE/$f" ] && ok "fixture has $f" || bad "fixture has $f"
done
[ -x "$FIXTURE/payload/den-vr-hello" ] && ok "fixture payload is executable" || bad "fixture payload is executable"

# --- dynamic: the real tap step against DEN_TEST_BIN ----------------------
if [ -z "${DEN_TEST_BIN:-}" ] || [ ! -x "${DEN_TEST_BIN:-}" ]; then
    echo "SKIP: DEN_TEST_BIN unset; tap step not run"
elif ! command -v bash >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: bash or python3 missing; tap step not run"
else
    den_bin=$(CDPATH= cd -- "$(dirname "$DEN_TEST_BIN")" && pwd)/$(basename "$DEN_TEST_BIN")
    _tmp=$(mktemp -d "${TMPDIR:-/tmp}/vr-tap-test.XXXXXX")
    _tmp=$(CDPATH= cd -P -- "$_tmp" && pwd)
    trap 'chmod -R u+w "$_tmp" 2>/dev/null; rm -rf "$_tmp"' EXIT
    # Stub installer: a wrapper exec'ing the den under test, plus the trust
    # replica the install step checks for.
    cat >"$_tmp/install.sh" <<INST
mkdir -p "\$DEN_INSTALL_DIR/bin" "\$DEN_INSTALL_DIR/share/den"
printf '#!/bin/sh\nexec "%s" "\$@"\n' "$den_bin" >"\$DEN_INSTALL_DIR/bin/den"
chmod 755 "\$DEN_INSTALL_DIR/bin/den"
echo '{}' >"\$DEN_INSTALL_DIR/share/den/known_hashes.json"
INST
    ver=$("$den_bin" --version)
    # A caller VR_TAP must not redirect the step.
    VR_ROOT="$_tmp/vr" VR_KEEP=1 VR_INSTALL_SH="$_tmp/install.sh" VR_PREV_TAG= \
        VR_ONLY=tap VR_TAP=marcelocantos/tap VR_TAP_FORMULA=cv \
        bash "$VR" "v$ver" >"$_tmp/out" 2>&1
    rc=$?
    if [ "$rc" -eq 0 ] && grep -q '^PASS  tap ' "$_tmp/out"; then
        ok "tap step passes against the real den ($ver)"
    else
        bad "tap step passes against the real den (exit $rc)"
        sed 's/^/    | /' "$_tmp/out"
        for l in "$_tmp"/vr/logs/*tap*.log; do [ -f "$l" ] && sed 's/^/    log| /' "$l"; done
    fi
    log=$(ls "$_tmp"/vr/logs/*-tap.log 2>/dev/null | head -1)
    if [ -n "$log" ] && grep -q '^den-vr-hello 1.0.0$' "$log"; then
        ok "installed fixture formula ran"
    else
        bad "installed fixture formula ran"
    fi
    if grep -R -i -E 'marcelocantos/(homebrew-)?tap' "$_tmp/vr/denhome/config.json" 2>/dev/null; then
        bad "no marcelocantos/tap registered"
    else
        ok "no marcelocantos/tap registered"
    fi
    if [ ! -e "$_tmp/vr/denhome/taps/den-fixture/verify-release" ] && [ ! -e "$_tmp/vr/prefix/bin/den-vr-hello" ]; then
        ok "tap and formula gone after the step"
    else
        bad "tap and formula gone after the step"
    fi
fi

echo
echo "${_pass} passed, ${_fail} failed"
[ "$_fail" -eq 0 ]

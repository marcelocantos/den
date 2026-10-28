#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Tests for verify-release's brew handling, against a stubbed brew and den:
#   * brew >= 7: the one formula under test is trusted (never the tap) before
#     any brew lookup, and a failed or missing trust stops with the command
#     to run; brew < 7 and official formulae are left alone.
#   * every brew call sees the caller's HOME (brew 7 keeps its trust store
#     there), not verify-release's scratch HOME.
#   * brew-test is SKIP only when brew refuses over outdated Xcode/CLT;
#     any other brew test failure is FAIL.
#   * formula-test runs den.rb's test commands directly.
#
# Run:
#   sh tests/scripts/test_verify_release_brew.sh
# or via ctest (name: script_test_verify_release_brew).

set -u

_SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT="${_SELF_DIR}/../.."
VR="${ROOT}/scripts/verify-release.sh"

for tool in bash python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "SKIP: $tool not found"
        exit 0
    fi
done

_pass=0
_fail=0
_tmp=$(mktemp -d "${TMPDIR:-/tmp}/vr-brew-test.XXXXXX")
_tmp=$(CDPATH= cd -P -- "$_tmp" && pwd)
trap 'chmod -R u+w "$_tmp" 2>/dev/null; rm -rf "$_tmp"' EXIT

ok() { _pass=$((_pass + 1)); echo "PASS: $1"; }
bad() { _fail=$((_fail + 1)); echo "FAIL: $1"; [ -f "$_tmp/out" ] && sed 's/^/    /' "$_tmp/out"; }

# Fake keg: den prints its version, a config under the scratch root, and an
# empty list (overridable with FAKE_DEN_LIST).
KEG="$_tmp/Cellar/den/1.2.0"
mkdir -p "$KEG/bin" "$_tmp/brewprefix"
cat >"$KEG/bin/den" <<'DEN'
#!/bin/sh
case "$1" in
    --version) echo 1.2.0 ;;
    config) [ "${FAKE_DEN_ESCAPE:-0}" = 1 ] && DEN_PREFIX=/opt/elsewhere
            printf 'den_home: %s\nstore: %s/store\nprefix: %s\n' "$DEN_HOME" "$DEN_HOME" "$DEN_PREFIX" ;;
    list) echo "${FAKE_DEN_LIST:-No packages installed}" ;;
    *) exit 0 ;;
esac
DEN
chmod 755 "$KEG/bin/den"

# Fake brew, driven by FAKE_BREW_VERSION, FAKE_TRUST (ok|fail) and
# FAKE_TEST (ok|xcode|clt|other|writetrust). Like brew 7 it keeps trust in
# ${XDG_CONFIG_HOME:-$HOME/.homebrew-ish}/trust.json for the environment it
# runs in, and refuses an untrusted third-party formula in --prefix, info and
# test. Every call is logged with the HOME it saw.
cat >"$_tmp/brew" <<'BREW'
#!/bin/sh
echo "HOME=$HOME ARGS=$*" >>"$FAKE_LOG"
if [ -n "${XDG_CONFIG_HOME:-}" ]; then cfg=$XDG_CONFIG_HOME/homebrew; else cfg=$HOME/.homebrew; fi
v=${FAKE_BREW_VERSION:-7.0.11}
need() { # need FORMULA: refuse it if brew >= 7, third-party and untrusted
    case "$1" in homebrew/*|*/*/*) ;; *) return 0 ;; esac
    case "$v" in [0-6].*) return 0 ;; esac
    grep -qx "$1" "$cfg/trust.json" 2>/dev/null && return 0
    echo "Error: Refusing to load formula $1 from untrusted tap ${1%/*}." >&2
    echo "Run \`brew trust --formula $1\` or \`brew trust ${1%/*}\` to trust it." >&2
    exit 1
}
case "$1" in
    --version) echo "Homebrew $v" ;;
    --prefix) [ $# -eq 1 ] && { echo "$FAKE_ROOT/brewprefix"; exit 0; }
              need "$2"; echo "$FAKE_KEG" ;;
    list) exit 0 ;;
    trust)
        [ "${FAKE_TRUST:-ok}" = ok ] || { echo "Error: No available formula with the name \"$3\"." >&2; exit 1; }
        if grep -qx "$3" "$cfg/trust.json" 2>/dev/null; then echo "Already trusted formula: $3"; exit 0; fi
        mkdir -p "$cfg" && echo "$3" >>"$cfg/trust.json" || exit 1
        echo "Trusted formula: $3" ;;
    info) need "$3"; echo '{"formulae":[],"casks":[]}' ;;
    test)
        need "$2"
        case "${FAKE_TEST:-ok}" in
            ok) echo "==> Testing $2" ;;
            xcode) echo "Error: Your Xcode (26.6) at /Applications/Xcode.app is too outdated." >&2
                   echo "Please update to Xcode 27.0 (or delete it)." >&2; exit 1 ;;
            clt) echo "Error: Your Command Line Tools are too outdated." >&2
                 echo "Update them from Software Update in System Settings." >&2; exit 1 ;;
            other) echo "Error: den: failed" >&2; echo "Expected: 1.2.0 Actual: 1.1.0" >&2; exit 1 ;;
            writetrust) # a trust write from scratch must not land anywhere
                if (echo "evil/tap/x" >>"$cfg/trust.json") 2>/dev/null \
                    || (: >"$cfg/new.json") 2>/dev/null; then
                    echo "Error: wrote to the trust store at $cfg" >&2; exit 1
                fi
                echo "==> trust store at $cfg is read-only" ;;
        esac ;;
    *) exit 0 ;;
esac
BREW
chmod 755 "$_tmp/brew"

# run NAME WANT_EXIT [VAR=VAL...] — verify-release limited to the brew steps.
# The caller's trust store starts empty unless PRETRUST=1 (already trusted).
run() {
    _name=$1; _want=$2; shift 2
    chmod -R u+w "$_tmp/vr" 2>/dev/null
    rm -rf "$_tmp/calls" "$_tmp/callerhome" "$_tmp/xdg" "$_tmp/vr"
    mkdir -p "$_tmp/callerhome"
    case " $* " in *" PRETRUST=1 "*) pretrust "$_tmp/callerhome/.homebrew" ;; esac
    case " $* " in *" XDG_CONFIG_HOME="*) pretrust "$_tmp/xdg/homebrew" ;; esac
    # Drop any ambient XDG config (GitHub's Linux runners set XDG_CONFIG_HOME),
    # so each case's trust store lives under its own caller HOME.
    env -u XDG_CONFIG_HOME -u HOMEBREW_XDG_CONFIG_HOME HOME="$_tmp/callerhome" BREW="$_tmp/brew" FAKE_LOG="$_tmp/calls" FAKE_ROOT="$_tmp" FAKE_KEG="$KEG" \
        VR_ROOT="$_tmp/vr" VR_FORMULA=marcelocantos/den-rc/den RELEASE_TAG=v1.2.0 VR_POURED=0 \
        VR_ONLY="brew-trust brew-test formula-test" "$@" bash "$VR" >"$_tmp/out" 2>&1
    _got=$?
    [ "$_got" -eq "$_want" ] || { bad "$_name (exit $_got, want $_want)"; return 1; }
    return 0
}
has() { grep -Eq "$1" "$_tmp/out"; }
called() { grep -Eq "$1" "$_tmp/calls" 2>/dev/null; }
pretrust() { mkdir -p "$1" && echo marcelocantos/den-rc/den >"$1/trust.json"; }
line_of() { grep -En "$1" "$_tmp/out" | head -1 | cut -d: -f1; }

# --- trust ---------------------------------------------------------------
if run "brew 7 trusts the formula" 0; then
    if called 'ARGS=trust --formula marcelocantos/den-rc/den$' \
        && [ "$(grep -c 'ARGS=trust' "$_tmp/calls")" -eq 1 ] \
        && has '^PASS  brew-trust'; then ok "brew 7 trusts the formula"; else bad "brew 7 trusts the formula"; fi
fi
run "trust is one formula, never the tap" 0 \
    && { called 'ARGS=trust (--tap )?marcelocantos/den-rc$' && bad "trust is one formula, never the tap" || ok "trust is one formula, never the tap"; }
# trust must precede the first formula lookup.
run "trust precedes brew --prefix" 0 && {
    t=$(grep -n 'ARGS=trust' "$_tmp/calls" | head -1 | cut -d: -f1)
    p=$(grep -n 'ARGS=--prefix marcelocantos' "$_tmp/calls" | head -1 | cut -d: -f1)
    [ -n "$t" ] && [ -n "$p" ] && [ "$t" -lt "$p" ] && ok "trust precedes brew --prefix" || bad "trust precedes brew --prefix"
}
run "pre-scratch brew calls see the caller's HOME" 0 && {
    if grep -E 'ARGS=(--version|trust|list|--prefix)' "$_tmp/calls" | grep -v "^HOME=$_tmp/callerhome " | grep -q .; then
        bad "pre-scratch brew calls see the caller's HOME"; sed 's/^/    /' "$_tmp/calls"
    else ok "pre-scratch brew calls see the caller's HOME"; fi
}
run "brew test runs under the scratch HOME" 0 && {
    if called 'ARGS=test ' && ! grep 'ARGS=test ' "$_tmp/calls" | grep -q "^HOME=$_tmp/callerhome "; then
        ok "brew test runs under the scratch HOME"
    else bad "brew test runs under the scratch HOME"; sed 's/^/    /' "$_tmp/calls"; fi
}

# --- trust handoff to the scratch HOME (read-only) -------------------------
# The den-test-mac case: trusted under the real HOME, Xcode too old. Must be
# brew-trust PASS, then brew-test SKIP for the Xcode reason (never refused as
# untrusted), then the rest of the run.
run "trusted + old Xcode: brew-trust PASS, then brew-test SKIP (Xcode)" 0 PRETRUST=1 FAKE_TEST=xcode && {
    a=$(line_of '^PASS  brew-trust'); b=$(line_of '^SKIP  brew-test +\(brew refuses to test: Your Xcode'); c=$(line_of '^PASS  formula-test')
    if [ -n "$a" ] && [ -n "$b" ] && [ -n "$c" ] && [ "$a" -lt "$b" ] && [ "$b" -lt "$c" ] && ! has 'untrusted' && has 'Already trusted formula'; then
        ok "trusted + old Xcode: brew-trust PASS, then brew-test SKIP (Xcode)"
    else bad "trusted + old Xcode: brew-trust PASS, then brew-test SKIP (Xcode)"; fi
}
run "fresh trust + old Xcode: SKIP for Xcode, not trust" 0 FAKE_TEST=xcode \
    && { has '^PASS  brew-trust' && has '^SKIP  brew-test .*Xcode' && ! has 'untrusted' \
        && ok "fresh trust + old Xcode: SKIP for Xcode, not trust" || bad "fresh trust + old Xcode: SKIP for Xcode, not trust"; }
run "trust under XDG_CONFIG_HOME is handed off too" 0 XDG_CONFIG_HOME="$_tmp/xdg" VR_BREW_TRUST=0 \
    && { has '^PASS  brew-test' && has "handed to scratch read-only \\($_tmp/xdg/homebrew/trust.json\\)" \
        && ok "trust under XDG_CONFIG_HOME is handed off too" || bad "trust under XDG_CONFIG_HOME is handed off too"; }
run "scratch brew cannot write either trust store" 0 PRETRUST=1 FAKE_TEST=writetrust \
    && { has '^PASS  brew-test' && [ "$(cat "$_tmp/callerhome/.homebrew/trust.json")" = marcelocantos/den-rc/den ] \
        && ok "scratch brew cannot write either trust store" || bad "scratch brew cannot write either trust store"; }
run "handoff leaves the caller's trust store unchanged" 0 PRETRUST=1 \
    && { [ "$(cat "$_tmp/callerhome/.homebrew/trust.json")" = marcelocantos/den-rc/den ] \
        && [ "$(ls -A "$_tmp/callerhome/.homebrew")" = trust.json ] \
        && ok "handoff leaves the caller's trust store unchanged" || bad "handoff leaves the caller's trust store unchanged"; }

run "failed trust stops with the command to run" 2 FAKE_TRUST=fail \
    && { has "brew trust --formula marcelocantos/den-rc/den" && has "failed" \
        && ok "failed trust stops with the command to run" || bad "failed trust stops with the command to run"; }
run "VR_BREW_TRUST=0 + untrusted stops clearly" 2 VR_BREW_TRUST=0 \
    && { has "not trusted" && has "Run: brew trust --formula marcelocantos/den-rc/den" && ! called 'ARGS=trust' \
        && ok "VR_BREW_TRUST=0 + untrusted stops clearly" || bad "VR_BREW_TRUST=0 + untrusted stops clearly"; }
run "VR_BREW_TRUST=0 + already trusted passes" 0 VR_BREW_TRUST=0 PRETRUST=1 \
    && { ! called 'ARGS=trust' && has '^PASS  brew-trust' \
        && ok "VR_BREW_TRUST=0 + already trusted passes" || bad "VR_BREW_TRUST=0 + already trusted passes"; }
run "brew < 7 is unchanged (no trust)" 0 FAKE_BREW_VERSION=5.1.1 \
    && { ! called 'ARGS=trust' && has 'brew 5 < 7' \
        && ok "brew < 7 is unchanged (no trust)" || bad "brew < 7 is unchanged (no trust)"; }
run "official formula needs no trust" 0 VR_FORMULA=den \
    && { ! called 'ARGS=trust' && ok "official formula needs no trust" || bad "official formula needs no trust"; }

# --- brew-test skip detection -------------------------------------------
run "brew test passes" 0 && { has '^PASS  brew-test' && ok "brew test passes" || bad "brew test passes"; }
run "outdated Xcode -> SKIP" 0 FAKE_TEST=xcode \
    && { has '^SKIP  brew-test +\(brew refuses to test: Your Xcode \(26.6\) at /Applications/Xcode.app is too outdated\.\)' \
        && ok "outdated Xcode -> SKIP" || bad "outdated Xcode -> SKIP"; }
run "outdated CLT -> SKIP" 0 FAKE_TEST=clt \
    && { has '^SKIP  brew-test +\(brew refuses to test: Your Command Line Tools are too outdated\.\)' \
        && ok "outdated CLT -> SKIP" || bad "outdated CLT -> SKIP"; }
run "unrelated brew test failure stays FAIL" 1 FAKE_TEST=other \
    && { has '^FAIL  brew-test' && ! has '^SKIP  brew-test' && ! has 'aborting' && has '^PASS  formula-test' \
        && ok "unrelated brew test failure stays FAIL" || bad "unrelated brew test failure stays FAIL"; }

# --- formula-test ----------------------------------------------------------
run "formula-test passes after an Xcode skip" 0 FAKE_TEST=xcode \
    && { has '^PASS  formula-test' && ok "formula-test passes after an Xcode skip" || bad "formula-test passes after an Xcode skip"; }
run "formula-test fails on a wrong den list" 1 FAKE_DEN_LIST="tree 2.1" \
    && { has '^FAIL  formula-test' && ok "formula-test fails on a wrong den list" || bad "formula-test fails on a wrong den list"; }
run "formula-test fails on a wrong version" 1 RELEASE_TAG=v9.9.9 \
    && { has '^FAIL  formula-test' && ok "formula-test fails on a wrong version" || bad "formula-test fails on a wrong version"; }

# --- isolation abort ------------------------------------------------------
# Only a wrong binary or an unconfined den aborts the run, and says which.
run "brew-test FAIL does not trigger the isolation abort" 1 FAKE_TEST=other \
    && { has '^PASS  isolation' && ! has 'aborting' && has 'failed:.* brew-test' \
        && ok "brew-test FAIL does not trigger the isolation abort" || bad "brew-test FAIL does not trigger the isolation abort"; }
run "brew-test SKIP does not trigger the isolation abort" 0 FAKE_TEST=clt \
    && { ! has 'aborting' && has '^PASS  formula-test' \
        && ok "brew-test SKIP does not trigger the isolation abort" || bad "brew-test SKIP does not trigger the isolation abort"; }
run "wrong binary aborts, naming the version check" 1 RELEASE_TAG=v9.9.9 VR_ONLY="version brew-test formula-test" \
    && { has '^FAIL  version' && has 'version failed \(den under test is not 9\.9\.9\) — aborting before den writes anything' \
        && ! has 'formula-test' && ! has 'isolation failed' \
        && ok "wrong binary aborts, naming the version check" || bad "wrong binary aborts, naming the version check"; }
run "escaped den config aborts, naming isolation" 1 FAKE_DEN_ESCAPE=1 \
    && { has '^FAIL  isolation' && has 'isolation failed \(den config escapes .*\) — aborting before den writes anything' \
        && ! has 'formula-test' \
        && ok "escaped den config aborts, naming isolation" || bad "escaped den config aborts, naming isolation"; }

# formula-test mirrors den.rb's test block (TEST in homebrew-bottles.py).
if grep -q 'den --version' "$ROOT/scripts/homebrew-bottles.py" \
    && grep -q '"No packages installed", shell_output("#{bin}/den list")' "$ROOT/scripts/homebrew-bottles.py" \
    && grep -q 'No packages installed' "$VR"; then
    ok "formula-test mirrors the formula's test block"
else
    bad "formula-test mirrors the formula's test block"
fi

echo ""
echo "Results: ${_pass} passed, ${_fail} failed"
[ "$_fail" -eq 0 ]

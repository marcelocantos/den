#!/usr/bin/env bash
# verify-release.sh — end-to-end battery against a release-installed den.
#
# Default target is the Homebrew formula (brew install marcelocantos/tap/den).
# Everything den touches lives under a scratch root; the user's real
# DEN_HOME, prefix, Homebrew prefix and LaunchAgents are never written.
#
# Knobs (all optional):
#   DEN_BIN            den binary under test (default: $(brew --prefix $VR_FORMULA)/bin/den)
#   RELEASE_TAG        expected release tag, e.g. v0.14.0 (default: v<CMakeLists project version>)
#   BREW               brew executable used for read-only lookups/`brew test` (default: brew)
#   VR_FORMULA         formula name (default: marcelocantos/tap/den)
#   VR_BREW_TEST       1 = run `brew test $VR_FORMULA` (default: 1 when DEN_BIN came from brew)
#   VR_BREW_TRUST      brew >= 7 only: 1 = run `brew trust --formula $VR_FORMULA`
#                      before any brew lookup (default); 0 = do not trust, just
#                      fail clearly if brew refuses to load the formula
#   VR_POURED          1 = require the formula to have been poured from a bottle
#                      (default: 1 when DEN_BIN came from brew on macOS; Linux
#                      has no bottles and installs from the release tarball)
#   VR_ROOT            scratch root (default: mktemp -d /tmp/den-verify-release.XXXXXX)
#   VR_HOME VR_DEN_HOME VR_PREFIX VR_APPDIR VR_FONTDIR VR_PKG_ROOT
#                      individual scratch paths (default: under VR_ROOT)
#   VR_BOTTLE          plain bottle formula           (default: tree)
#   VR_DEP_PARENT      formula with a dependency      (default: jq)
#   VR_DEP_CHILD       its dependency                 (default: oniguruma)
#   VR_TAP / VR_TAP_FORMULA  tap and formula from it  (default: marcelocantos/tap, cv)
#   VR_FONT_CASK       font cask                      (default: font-hack)
#   VR_APP_CASK        app cask                       (default: macdown)
#   VR_APP_BUNDLE      bundle the app cask installs   (default: MacDown.app)
#   VR_ONLY            space-separated step names to run (default: all)
#   VR_SKIP            space-separated step names to skip
#   VR_SKIP_LAUNCHD    1 = skip the LaunchAgent step (same as VR_SKIP=launchd)
#   VR_SERVICE_PLIST   service plist to exercise (default: <formula prefix>/{sh.brew,homebrew.mxcl}.den.plist)
#   VR_LAUNCHD_SECS    how long the agent must stay up (default: 60)
#   VR_REPLICA_ROOT    install root the trust replica must ship under (default: dirname(DEN_BIN)/..)
#   VR_DOCTOR_ALLOW    optional ERE of `den doctor` [error] lines to tolerate (default:
#                      none; a clean release needs no allowlist)
#   VR_KEEP            1 = keep the scratch root (default: kept only on failure)
#
# Homebrew 7 refuses to load formulae from a non-official tap until they are
# trusted, and keeps that trust list under the caller's $HOME
# (~/.homebrew/trust.json, or $XDG_CONFIG_HOME/homebrew). verify-release
# therefore trusts the one formula under test (never the whole tap) before
# its first brew lookup, then hands a read-only copy of that trust store to
# the scratch HOME, under which the later brew commands run. brew-test is reported SKIP, not FAIL, only
# when brew refuses because Xcode or the Command Line Tools are below its
# minimum (brew 7 on macOS 27 refuses `brew test` for every formula without
# Xcode/CLT 27); formula-test runs the formula's test commands directly so
# that coverage is kept either way.
#
# Steps: version brew-trust brew-test formula-test isolation poured update bottle
#        opt-link trust pin upgrade dependents tap font-cask app-cask uninstall
#        doctor cleanup launchd
set -uo pipefail

die() { echo "verify-release: $*" >&2; exit 2; }
realpath_() { python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$1"; }
under() { case "$(realpath_ "$1")/" in "$(realpath_ "$2")"/*) return 0 ;; esac; return 1; }

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
# The account's home from the password database, not $HOME (which a caller
# may already have pointed at scratch).
REAL_HOME=$(python3 -c 'import os,pwd; print(pwd.getpwuid(os.getuid()).pw_dir)')
# The caller's HOME before scratch takes over: brew 7 keeps its trust store
# relative to it (see brew_trust_file).
BREW_HOME=${HOME:-$REAL_HOME}
BREW=${BREW:-brew}
VR_FORMULA=${VR_FORMULA:-marcelocantos/tap/den}
VR_BREW_TRUST=${VR_BREW_TRUST:-1}
# Every brew call, minus the scratch prefix overrides. Before scratch is set
# up (trust preflight, keg discovery) brew sees the caller's own HOME and
# config. Once scratch is set up, brew_scratch_ runs it under the scratch
# HOME, with brew's config dir pinned there too, so it reads the read-only
# trust handoff and can write nothing back to the caller's brew config.
brew_() { env -u HOMEBREW_PREFIX -u HOMEBREW_CELLAR HOMEBREW_NO_AUTO_UPDATE=1 "$BREW" "$@"; }
brew_scratch_() { env -u XDG_CONFIG_HOME -u HOMEBREW_XDG_CONFIG_HOME -u HOMEBREW_PREFIX -u HOMEBREW_CELLAR HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_COLOR=1 "$BREW" "$@"; }
# Where brew 7 reads its trust store for the current environment: under
# $XDG_CONFIG_HOME/homebrew (or $HOMEBREW_XDG_CONFIG_HOME/homebrew), else
# $HOME/.homebrew (Library/Homebrew/trust.rb trust_file; bin/brew sets
# HOMEBREW_USER_CONFIG_HOME).
brew_trust_file() {
    if [ -n "${XDG_CONFIG_HOME:-}" ]; then echo "$XDG_CONFIG_HOME/homebrew/trust.json"
    elif [ -n "${HOMEBREW_XDG_CONFIG_HOME:-}" ]; then echo "$HOMEBREW_XDG_CONFIG_HOME/homebrew/trust.json"
    else echo "$BREW_HOME/.homebrew/trust.json"; fi
}

# ---- brew >= 7: trust the formula under test ----------------------------
# Needed before the first lookup: `brew --prefix`, `info` and `test` all
# load the formula, and brew 7 refuses an untrusted non-official one.
TRUST_NOTE=""
TRUST_PROBE=0 # 1 = brew >= 7 and a third-party formula: re-check under scratch
TRUST_SRC=$(brew_trust_file)
brew_major() { brew_ --version 2>/dev/null | sed -n 's/^Homebrew \([0-9][0-9]*\)\..*/\1/p' | head -1; }
needs_trust() { # fully-qualified formula from a non-official tap
    case "$1" in
        homebrew/*|Homebrew/*) return 1 ;;
        */*/*) return 0 ;;
    esac
    return 1
}
trust_formula() {
    local major out rc=0
    major=$(brew_major)
    if [ -z "$major" ] || [ "$major" -lt 7 ]; then
        TRUST_NOTE="brew ${major:-unknown} < 7: no tap trust needed"; return 0
    fi
    if ! needs_trust "$VR_FORMULA"; then
        TRUST_NOTE="$VR_FORMULA is not from a third-party tap: no trust needed"; return 0
    fi
    if [ "$VR_BREW_TRUST" = 1 ]; then
        out=$(brew_ trust --formula "$VR_FORMULA" 2>&1) || rc=$?
        [ "$rc" -eq 0 ] || die "brew $major: 'brew trust --formula $VR_FORMULA' failed (exit $rc):
$out
Fix the tap (brew tap ${VR_FORMULA%/*}), then run: brew trust --formula $VR_FORMULA"
        TRUST_NOTE="brew $major: $out"
    fi
    TRUST_PROBE=1
    # Prove brew will now load it (also covers VR_BREW_TRUST=0).
    out=$(brew_ info --json=v2 "$VR_FORMULA" 2>&1) || true
    if echo "$out" | grep -Eq 'Refusing to load .* from untrusted tap|untrusted tap'; then
        die "brew $major refuses to load $VR_FORMULA because it is not trusted.
Run: brew trust --formula $VR_FORMULA
(or rerun verify-release with VR_BREW_TRUST=1, the default)"
    fi
    TRUST_NOTE="${TRUST_NOTE:-brew $major: $VR_FORMULA already trusted}; brew loads $VR_FORMULA"
}
if [ -z "${DEN_BIN:-}" ] || [ "${VR_BREW_TEST:-0}" = 1 ] || [ "${VR_POURED:-0}" = 1 ]; then
    command -v "$BREW" >/dev/null 2>&1 || die "DEN_BIN unset and no brew on PATH"
    trust_formula
fi

# ---- binary under test --------------------------------------------------
FORMULA_PREFIX=""
if [ -z "${DEN_BIN:-}" ]; then
    brew_ list --versions "$VR_FORMULA" >/dev/null 2>&1 \
        || die "$VR_FORMULA is not installed; run: brew install $VR_FORMULA (or set DEN_BIN)"
    FORMULA_PREFIX=$(brew_ --prefix "$VR_FORMULA")
    DEN_BIN="$FORMULA_PREFIX/bin/den"
    : "${VR_BREW_TEST:=1}"
    [ "$(uname)" = Darwin ] && : "${VR_POURED:=1}"
fi
[ -x "$DEN_BIN" ] || die "DEN_BIN not executable: $DEN_BIN"
DEN_BIN=$(cd "$(dirname "$DEN_BIN")" && pwd)/$(basename "$DEN_BIN")
: "${VR_BREW_TEST:=0}"
: "${VR_POURED:=0}"
[ "$VR_POURED" = 0 ] || [ -n "$FORMULA_PREFIX" ] || die "VR_POURED=1 needs a brew-installed den (unset DEN_BIN)"
if [ -z "${VR_SERVICE_PLIST:-}" ] && [ -n "$FORMULA_PREFIX" ]; then
    # Homebrew names the generated plist after its service label, which has
    # been homebrew.mxcl.<name> and, more recently, sh.brew.<name>.
    for f in "$FORMULA_PREFIX/sh.brew.den.plist" "$FORMULA_PREFIX/homebrew.mxcl.den.plist"; do
        if [ -f "$f" ]; then VR_SERVICE_PLIST=$f; break; fi
    done
fi
VR_SERVICE_PLIST=${VR_SERVICE_PLIST:-}
if [ -z "${RELEASE_TAG:-}" ]; then
    v=$(sed -n 's/^project(den VERSION \([0-9.]*\).*/\1/p' "$REPO_DIR/CMakeLists.txt" 2>/dev/null)
    [ -n "$v" ] || die "RELEASE_TAG unset and no version in CMakeLists.txt"
    RELEASE_TAG="v$v"
fi
VERSION=${RELEASE_TAG#v}

# ---- scratch layout & guard ---------------------------------------------
VR_ROOT=${VR_ROOT:-$(mktemp -d /tmp/den-verify-release.XXXXXX)}
VR_ROOT=$(realpath_ "$VR_ROOT")
VR_HOME=${VR_HOME:-$VR_ROOT/home}
VR_DEN_HOME=${VR_DEN_HOME:-$VR_ROOT/denhome}
VR_PREFIX=${VR_PREFIX:-$VR_ROOT/prefix}
VR_APPDIR=${VR_APPDIR:-$VR_ROOT/apps}
VR_FONTDIR=${VR_FONTDIR:-$VR_ROOT/fonts}
VR_PKG_ROOT=${VR_PKG_ROOT:-$VR_ROOT/pkgroot}
LOGDIR=$VR_ROOT/logs

FORBIDDEN="/opt/homebrew /usr/local /home/linuxbrew/.linuxbrew $REAL_HOME"
if command -v "$BREW" >/dev/null 2>&1; then
    FORBIDDEN="$FORBIDDEN $(brew_ --prefix 2>/dev/null)"
fi
for p in "$VR_ROOT" "$VR_HOME" "$VR_DEN_HOME" "$VR_PREFIX" "$VR_APPDIR" "$VR_FONTDIR" "$VR_PKG_ROOT"; do
    for f in $FORBIDDEN; do
        [ -n "$f" ] && [ -e "$f" ] || continue
        if under "$p" "$f"; then die "refusing scratch path $p (under $f)"; fi
    done
done

# Only create anything once every path has cleared the guard.
mkdir -p "$VR_HOME" "$VR_DEN_HOME" "$VR_PREFIX" "$VR_APPDIR" "$VR_FONTDIR" "$VR_PKG_ROOT" "$LOGDIR"

unset DEN_ENV DEN_STORE DEN_CONFIG
export HOME="$VR_HOME" DEN_HOME="$VR_DEN_HOME" DEN_PREFIX="$VR_PREFIX"
export HOMEBREW_PREFIX="$VR_PREFIX" HOMEBREW_CELLAR="$VR_PREFIX/Cellar"
export DEN_APPDIR="$VR_APPDIR" DEN_FONTDIR="$VR_FONTDIR" DEN_CASK_PKG_ROOT="$VR_PKG_ROOT"
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_ANALYTICS=1

# Hand brew's trust store to the scratch HOME, read-only. brew under the
# scratch HOME (brew_scratch_) reads $VR_HOME/.homebrew/trust.json; without
# it brew 7 refuses the formula trusted in the caller's store. The file is
# 0400 in a 0500 dir: brew reads the store with no ownership or lock checks
# (trust.rb trust_store), and any attempted trust write (atomic_write into
# the dir, plus its .lock file) fails rather than touching either store.
TRUST_HANDOFF=""
if [ -f "$TRUST_SRC" ]; then
    mkdir -p "$VR_HOME/.homebrew"
    cp "$TRUST_SRC" "$VR_HOME/.homebrew/trust.json"
    chmod 0400 "$VR_HOME/.homebrew/trust.json"
    chmod 0500 "$VR_HOME/.homebrew"
    TRUST_HANDOFF="trust store handed to scratch read-only ($TRUST_SRC)"
fi

VR_BOTTLE=${VR_BOTTLE:-tree}
VR_DEP_PARENT=${VR_DEP_PARENT:-jq}
VR_DEP_CHILD=${VR_DEP_CHILD:-oniguruma}
VR_TAP=${VR_TAP:-marcelocantos/tap}
VR_TAP_FORMULA=${VR_TAP_FORMULA:-cv}
VR_FONT_CASK=${VR_FONT_CASK:-font-hack}
VR_APP_CASK=${VR_APP_CASK:-macdown}
VR_APP_BUNDLE=${VR_APP_BUNDLE:-MacDown.app}
VR_LAUNCHD_SECS=${VR_LAUNCHD_SECS:-60}
VR_REPLICA_ROOT=${VR_REPLICA_ROOT:-$(dirname "$(dirname "$(realpath_ "$DEN_BIN")")")}
[ "${VR_SKIP_LAUNCHD:-0}" = 1 ] && VR_SKIP="${VR_SKIP:-} launchd"

# ---- step runner ----------------------------------------------------------
PASSED=0; FAILED=0; SKIPPED=0; FAILS=""; N=0
SKIP_RC=77
skip_because() { echo "skip-reason: $*"; exit "$SKIP_RC"; }
den() { "$DEN_BIN" "$@"; }
wanted() {
    [ "$1" = isolation ] && return 0 # safety check: never skippable
    case " ${VR_SKIP:-} " in *" $1 "*) return 1 ;; esac
    [ -z "${VR_ONLY:-}" ] && return 0
    case " $VR_ONLY " in *" $1 "*) return 0 ;; esac
    return 1
}
step() {
    local name=$1; shift
    N=$((N + 1))
    if ! wanted "$name"; then
        printf 'SKIP  %-12s\n' "$name"; SKIPPED=$((SKIPPED + 1)); return 0
    fi
    local log; log=$(printf '%s/%02d-%s.log' "$LOGDIR" "$N" "$name")
    local t0=$SECONDS
    # Not inside `if`: bash ignores set -e in a tested subshell.
    local rc=0
    ( set -e; "$@" ) >"$log" 2>&1 || rc=$?
    if [ "$rc" -eq "$SKIP_RC" ]; then
        # A step may decline with `skip_because <reason>` (exit $SKIP_RC).
        printf 'SKIP  %-12s (%s)\n' "$name" "$(sed -n 's/^skip-reason: //p' "$log" | tail -1)"
        SKIPPED=$((SKIPPED + 1))
    elif [ "$rc" -eq 0 ]; then
        printf 'PASS  %-12s (%ds)\n' "$name" $((SECONDS - t0)); PASSED=$((PASSED + 1))
    else
        printf 'FAIL  %-12s (%ds) — log: %s\n' "$name" $((SECONDS - t0)) "$log"
        sed 's/^/      | /' "$log" | tail -15
        FAILED=$((FAILED + 1)); FAILS="$FAILS $name"
    fi
}
expect_fail() { # expect_fail <ERE> cmd...: cmd must exit non-zero and print ERE
    local re=$1; shift; local out rc=0
    out=$("$@" 2>&1) || rc=$?
    echo "$out"
    [ "$rc" -ne 0 ] || { echo "expected non-zero exit from: $*"; return 1; }
    echo "$out" | grep -Eq "$re" || { echo "expected output matching /$re/"; return 1; }
}
listed() { den list 2>/dev/null | grep -Eq "(^|[[:space:]/])$1([[:space:]]|$)"; }

# ---- steps ----------------------------------------------------------------
s_version() {
    local got; got=$(den --version)
    echo "den --version: $got (expected $VERSION)"
    [ "$got" = "$VERSION" ]
}
s_brew_trust() {
    echo "$TRUST_NOTE"
    [ -z "$TRUST_HANDOFF" ] || echo "$TRUST_HANDOFF"
    [ "$TRUST_PROBE" = 1 ] || return 0
    # Under the scratch HOME, as brew-test and poured will run brew.
    local out; out=$(brew_scratch_ info --json=v2 "$VR_FORMULA" 2>&1) || true
    if echo "$out" | grep -q 'untrusted tap'; then
        echo "brew under the scratch HOME still refuses $VR_FORMULA as untrusted"
        echo "(trust store: ${TRUST_SRC}; handoff: ${TRUST_HANDOFF:-none})"
        echo "$out" | grep 'untrusted tap' | head -1
        return 1
    fi
    echo "brew under the scratch HOME loads $VR_FORMULA"
}
# brew refuses to set up any test (or build) environment when Xcode or the
# CLT is below its minimum (Homebrew::Diagnostic fatal_setup_build_environment_checks).
# That is the host, not den: report SKIP with brew's reason. Anything else fails.
BREW_DEVTOOLS_RE='^(Error: )?Your (Xcode \(.*\).* is too outdated|Command Line Tools are too outdated)'
# brew 7's `brew test` errors can arrive coloured and CRLF-terminated even
# when captured: "\e[31mError:\e[0m Your Xcode (26.6) ... is too outdated.\r".
# brew_scratch_ sets HOMEBREW_NO_COLOR, but don't rely on it: strip ANSI
# escape sequences and trailing CRs before matching.
plain_text_() {
    local esc cr; esc=$(printf '\033'); cr=$(printf '\r')
    sed -e "s/${esc}\[[0-9;?]*[A-Za-z]//g" -e "s/${cr}*\$//"
}
s_brew_test() {
    local out rc=0
    out=$(brew_scratch_ test "$VR_FORMULA" 2>&1) || rc=$?
    echo "$out"
    [ "$rc" -ne 0 ] || return 0
    local why; why=$(printf '%s\n' "$out" | plain_text_ | grep -E "$BREW_DEVTOOLS_RE" | head -1 | sed 's/^Error: //' || true)
    [ -z "$why" ] || skip_because "brew refuses to test: $why"
    echo "brew test exit $rc"; return 1
}
# The formula's own test block, run directly (mirrors TEST in
# scripts/homebrew-bottles.py), so den.rb's checks still run when brew-test
# is skipped.
s_formula_test() {
    local t=$VR_ROOT/formula-test got
    mkdir -p "$t"
    got=$(den --version); echo "den --version: $got"
    [ "$got" = "$VERSION" ] || { echo "expected $VERSION"; return 1; }
    got=$(DEN_HOME="$t/.den" DEN_PREFIX="$t/prefix" den list); echo "den list: $got"
    echo "$got" | grep -q "No packages installed" || { echo "den list did not report 'No packages installed'"; return 1; }
}
# INSTALL_RECEIPT.json and `brew info --json=v2` must both say the keg was
# poured from a bottle (a tarball/source install records false). brew
# derives its own prefix, but drop the scratch overrides anyway.
s_poured() {
    env -u HOMEBREW_PREFIX -u HOMEBREW_CELLAR -u XDG_CONFIG_HOME -u HOMEBREW_XDG_CONFIG_HOME BREW="$BREW" \
        sh "$SCRIPT_DIR/check-poured.sh" "$FORMULA_PREFIX" "$VR_FORMULA"
}
s_isolation() {
    local cfg; cfg=$(den config); echo "$cfg"
    local key val
    for key in den_home store prefix; do
        val=$(echo "$cfg" | sed -n "s/^${key}:[[:space:]]*//p" | head -1)
        [ -n "$val" ] || { echo "den config has no $key"; return 1; }
        under "$val" "$VR_ROOT" || { echo "$key=$val escapes $VR_ROOT"; return 1; }
    done
}
s_update() { den update; }
s_bottle() {
    den install "$VR_BOTTLE"
    listed "$VR_BOTTLE"
    [ -x "$VR_PREFIX/bin/$VR_BOTTLE" ]
    "$VR_PREFIX/bin/$VR_BOTTLE" --version
}
s_opt_link() {
    local opt=$VR_PREFIX/opt/$VR_BOTTLE
    [ -L "$opt" ] || { echo "no opt link $opt"; return 1; }
    ls -l "$opt"
    under "$opt" "$VR_PREFIX/Cellar/$VR_BOTTLE"
    [ -x "$opt/bin/$VR_BOTTLE" ]
}
s_trust() {
    local doc src arch sha bad
    doc=$(den doctor 2>&1 || true); echo "$doc" | grep '\[trust\]'
    echo "$doc" | grep -q '\[trust\] replica: active' || { echo "trust replica not active"; return 1; }
    src=$(echo "$doc" | sed -n 's/.*\[trust\] replica source: //p' | head -1)
    [ -f "$src" ] || { echo "replica source missing: $src"; return 1; }
    under "$src" "$VR_REPLICA_ROOT" || { echo "replica $src not shipped under $VR_REPLICA_ROOT"; return 1; }
    arch=$(ls "$VR_DEN_HOME"/cache/archives/*.tar.gz 2>/dev/null | head -1)
    [ -n "$arch" ] || { echo "no cached bottle archive"; return 1; }
    sha=$(basename "$arch" .tar.gz)
    den replica-verify --name "$VR_BOTTLE" --version 0 --tag local --sha256 "$sha" --bottle "$arch"
    bad=$VR_ROOT/tampered.tar.gz; cp "$arch" "$bad"; printf 'x' >>"$bad"
    expect_fail 'MISMATCH' den replica-verify --name "$VR_BOTTLE" --version 0 --tag local --sha256 "$sha" --bottle "$bad"
}
s_pin() {
    den pin "$VR_BOTTLE"
    expect_fail 'pinned' den upgrade "$VR_BOTTLE"
    den unpin "$VR_BOTTLE"
    den upgrade "$VR_BOTTLE"
}
s_upgrade() { den upgrade; den outdated; }
s_dependents() {
    den install "$VR_DEP_PARENT"
    listed "$VR_DEP_CHILD" || { echo "$VR_DEP_CHILD not installed as a dependency"; return 1; }
    expect_fail "required by $VR_DEP_PARENT" den uninstall "$VR_DEP_CHILD"
    listed "$VR_DEP_CHILD"
    den uninstall "$VR_DEP_PARENT"
    den uninstall "$VR_DEP_CHILD"
    if listed "$VR_DEP_PARENT"; then echo "$VR_DEP_PARENT still listed"; return 1; fi
}
s_tap() {
    den tap add "$VR_TAP"
    den install "$VR_TAP/$VR_TAP_FORMULA"
    listed "$VR_TAP_FORMULA"
    "$VR_PREFIX/bin/$VR_TAP_FORMULA" --version
    den uninstall "$VR_TAP/$VR_TAP_FORMULA"
    [ ! -e "$VR_PREFIX/bin/$VR_TAP_FORMULA" ]
}
s_font_cask() {
    den install "$VR_FONT_CASK"
    local n; n=$(find "$VR_FONTDIR" -type f | wc -l | tr -d ' ')
    echo "fonts in DEN_FONTDIR: $n"; [ "$n" -gt 0 ]
    den uninstall "$VR_FONT_CASK"
    n=$(find "$VR_FONTDIR" -type f | wc -l | tr -d ' ')
    echo "fonts after uninstall: $n"; [ "$n" -eq 0 ]
}
s_app_cask() {
    den install "$VR_APP_CASK"
    [ -d "$VR_APPDIR/$VR_APP_BUNDLE" ] || { echo "missing $VR_APPDIR/$VR_APP_BUNDLE"; return 1; }
    den uninstall "$VR_APP_CASK"
    [ ! -e "$VR_APPDIR/$VR_APP_BUNDLE" ]
}
s_uninstall() {
    den uninstall "$VR_BOTTLE"
    if listed "$VR_BOTTLE"; then echo "$VR_BOTTLE still listed"; return 1; fi
    [ ! -e "$VR_PREFIX/bin/$VR_BOTTLE" ] && [ ! -e "$VR_PREFIX/opt/$VR_BOTTLE" ]
}
s_doctor() {
    local doc rc=0 errs
    doc=$(den doctor 2>&1) || rc=$?
    echo "$doc"; [ "$rc" -eq 0 ] || { echo "doctor exit $rc"; return 1; }
    errs=$(echo "$doc" | grep '\[error\]' || true)
    # den writes config.json 0600 and doctor agrees on that mode (octal output).
    local cfg="$DEN_HOME/config.json" mode
    if [ -f "$cfg" ]; then
        mode=$(stat -f %Lp "$cfg" 2>/dev/null || stat -c %a "$cfg")
        [ "$mode" = 600 ] || { echo "config.json mode $mode (expected 600)"; return 1; }
    fi
    [ -n "${VR_DOCTOR_ALLOW:-}" ] && errs=$(echo "$errs" | grep -Ev "$VR_DOCTOR_ALLOW" || true)
    [ -z "$errs" ] || { echo "doctor errors:"; echo "$errs"; return 1; }
}
s_cleanup() {
    den cleanup
    [ -z "$(ls -A "$VR_DEN_HOME/cache/archives" 2>/dev/null)" ] || { echo "archive cache not emptied"; return 1; }
}

# Fixed up front so the EXIT trap can always tear the agent down, even when
# the step (which runs in a subshell) dies part-way.
LA_LABEL="den.verify-release.$$"
LA_PLIST="$VR_ROOT/$LA_LABEL.plist"
la_teardown() {
    [ "$(uname)" = Darwin ] || return 0
    launchctl bootout "gui/$(id -u)/$LA_LABEL" >/dev/null 2>&1 || true
    rm -f "$LA_PLIST"
}
s_launchd() {
    [ "$(uname)" = Darwin ] || { echo "launchd step is macOS-only"; return 1; }
    [ -f "$VR_SERVICE_PLIST" ] || { echo "no service plist (VR_SERVICE_PLIST=$VR_SERVICE_PLIST)"; return 1; }
    local prog a1 a2 uid pid0 pid st t=0
    prog=$(plutil -extract ProgramArguments.0 raw "$VR_SERVICE_PLIST")
    a1=$(plutil -extract ProgramArguments.1 raw "$VR_SERVICE_PLIST")
    a2=$(plutil -extract ProgramArguments.2 raw "$VR_SERVICE_PLIST")
    echo "service: $prog $a1 $a2"
    [ "$a1 $a2" = "daemon run" ] || { echo "service does not run 'den daemon run'"; return 1; }
    [ "$(realpath_ "$prog")" = "$(realpath_ "$DEN_BIN")" ] || { echo "service binary $prog is not $DEN_BIN"; return 1; }

    uid=$(id -u)
    trap la_teardown EXIT
    cp "$VR_SERVICE_PLIST" "$LA_PLIST"; chmod u+w "$LA_PLIST"
    plutil -replace Label -string "$LA_LABEL" "$LA_PLIST"
    plutil -remove EnvironmentVariables "$LA_PLIST" 2>/dev/null || true
    plutil -insert EnvironmentVariables -dictionary "$LA_PLIST"
    local k
    for k in HOME DEN_HOME DEN_PREFIX HOMEBREW_PREFIX HOMEBREW_CELLAR DEN_APPDIR DEN_FONTDIR DEN_CASK_PKG_ROOT; do
        plutil -insert "EnvironmentVariables.$k" -string "${!k}" "$LA_PLIST"
    done
    plutil -insert EnvironmentVariables.PATH -string "/usr/bin:/bin:/usr/sbin:/sbin" "$LA_PLIST"
    plutil -replace StandardOutPath -string "$VR_ROOT/launchd.out" "$LA_PLIST"
    plutil -replace StandardErrorPath -string "$VR_ROOT/launchd.err" "$LA_PLIST"
    plutil -remove WorkingDirectory "$LA_PLIST" 2>/dev/null || true
    launchctl bootstrap "gui/$uid" "$LA_PLIST"
    echo "bootstrapped gui/$uid/$LA_LABEL"

    sleep 3
    pid0=$(launchctl print "gui/$uid/$LA_LABEL" | sed -n 's/^[[:space:]]*pid = //p' | head -1)
    [ -n "$pid0" ] || { launchctl print "gui/$uid/$LA_LABEL"; echo "agent not running"; return 1; }
    while [ "$t" -lt "$VR_LAUNCHD_SECS" ]; do
        sleep 5; t=$((t + 5))
        st=$(launchctl print "gui/$uid/$LA_LABEL")
        pid=$(echo "$st" | sed -n 's/^[[:space:]]*pid = //p' | head -1)
        echo "t=${t}s state=$(echo "$st" | sed -n 's/^[[:space:]]*state = //p' | head -1) pid=${pid:-none}"
        [ "$pid" = "$pid0" ] || { echo "agent died or restarted (pid $pid0 -> ${pid:-none})"; cat "$VR_ROOT/launchd.err" 2>/dev/null; return 1; }
    done
    local exe; exe=$(ps -o comm= -p "$pid0")
    echo "process: $exe"
    [ "$(realpath_ "$exe")" = "$(realpath_ "$DEN_BIN")" ] || { echo "running binary is not $DEN_BIN"; return 1; }
    [ "$("$exe" --version)" = "$VERSION" ] || { echo "running binary reports $("$exe" --version)"; return 1; }
    cat "$VR_DEN_HOME/daemon.log"
    grep -q "daemon started (pid $pid0, den $VERSION)" "$VR_DEN_HOME/daemon.log" \
        || { echo "daemon.log lacks start line for pid $pid0 / den $VERSION"; return 1; }
    la_teardown
    ! launchctl print "gui/$uid/$LA_LABEL" >/dev/null 2>&1
}

finish() {
    la_teardown
    if [ "${VR_KEEP:-0}" = 1 ] || [ "$FAILED" -gt 0 ]; then
        echo "scratch kept: $VR_ROOT"
    else
        chmod -R u+w "$VR_ROOT" 2>/dev/null; rm -rf "$VR_ROOT"
    fi
}
trap finish EXIT
trap 'exit 130' INT TERM

echo "verify-release: den=$DEN_BIN tag=$RELEASE_TAG root=$VR_ROOT"
[ -z "$TRUST_NOTE" ] || echo "verify-release: brew-trust: $TRUST_NOTE"
[ -z "$TRUST_HANDOFF" ] || echo "verify-release: brew-trust: $TRUST_HANDOFF"
step version s_version
if [ -n "$TRUST_NOTE" ]; then step brew-trust s_brew_trust; else VR_SKIP="${VR_SKIP:-} brew-trust"; step brew-trust true; fi
if [ "$VR_BREW_TEST" = 1 ]; then step brew-test s_brew_test; else VR_SKIP="${VR_SKIP:-} brew-test"; step brew-test true; fi
step isolation s_isolation
# Abort only on the safety checks themselves (brew-trust/brew-test results
# are reported, not fatal): a wrong binary or an unconfined den.
case " $FAILS " in
    *" version "*) echo "verify-release: version failed (den under test is not $VERSION) — aborting before den writes anything"; exit 1 ;;
    *" isolation "*) echo "verify-release: isolation failed (den config escapes $VR_ROOT) — aborting before den writes anything"; exit 1 ;;
esac
if [ "$VR_POURED" = 1 ]; then step poured s_poured; else VR_SKIP="${VR_SKIP:-} poured"; step poured true; fi
step formula-test s_formula_test
step update s_update
step bottle s_bottle
step opt-link s_opt_link
step trust s_trust
step pin s_pin
step upgrade s_upgrade
step dependents s_dependents
step tap s_tap
if [ "$(uname)" = Darwin ]; then
    step font-cask s_font_cask
    step app-cask s_app_cask
else
    VR_SKIP="${VR_SKIP:-} font-cask app-cask launchd"
    step font-cask true; step app-cask true
fi
step uninstall s_uninstall
step doctor s_doctor
step cleanup s_cleanup
step launchd s_launchd

echo "verify-release: $PASSED passed, $FAILED failed, $SKIPPED skipped${FAILS:+ (failed:$FAILS)}"
[ "$FAILED" -eq 0 ]

#!/usr/bin/env bash
# verify-release.sh — end-to-end battery against a published den release.
#
# Usage: scripts/verify-release.sh [TAG]      (or RELEASE_TAG=TAG)
#
# TAG is a release tag, GA or release candidate: v0.15.0-rc.1, 0.15.0-rc.1,
# v0.14.0 or 0.14.0. The release is installed with install.sh into a scratch
# root — scratch HOME, DEN_HOME, DEN_PREFIX and DEN_INSTALL_DIR, with
# /opt/homebrew (and Linuxbrew) stripped from PATH and every DYLD_* variable
# unset — so Homebrew is neither needed nor consulted. Everything den touches
# lives under the scratch root; the user's real DEN_HOME, prefix, any
# Homebrew prefix and the real LaunchAgents are never written.
#
# Knobs (all optional):
#   RELEASE_TAG        release tag to verify (default: v<CMakeLists project version>)
#   VR_INSTALL_SH      installer (default: install.sh in this checkout)
#   VR_PREV_TAG        GA the self-update step starts from (default: the newest
#                      vX.Y.Z tag below RELEASE_TAG, per scripts/prev-release-tag.sh)
#   VR_RELEASE_URL     release download base (default:
#                      https://github.com/marcelocantos/den/releases/download)
#   VR_ROOT            scratch root (default: mktemp -d /tmp/den-verify-release.XXXXXX)
#   VR_HOME VR_DEN_HOME VR_PREFIX VR_APPDIR VR_FONTDIR VR_PKG_ROOT VR_INSTALL_DIR
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
#   VR_LAUNCHD_SECS    how long the agent must stay up (default: 60)
#   VR_REPLICA_ROOT    install root the trust replica must ship under
#                      (default: VR_INSTALL_DIR)
#   VR_DOCTOR_ALLOW    optional ERE of `den doctor` [error] lines to tolerate (default:
#                      none; a clean release needs no allowlist)
#   VR_KEEP            1 = keep the scratch root (default: kept only on failure)
#
# Steps: install version isolation links checksum self-update empty-store
#        update bottle opt-link trust pin upgrade dependents tap font-cask
#        app-cask uninstall doctor cleanup launchd
# install and isolation can never be skipped; the run aborts before den
# writes anything if install, version or isolation fails.
set -uo pipefail

die() { echo "verify-release: $*" >&2; exit 2; }
realpath_() { python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$1"; }
under() { case "$(realpath_ "$1")/" in "$(realpath_ "$2")"/*) return 0 ;; esac; return 1; }

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
# The account's home from the password database, not $HOME (which a caller
# may already have pointed at scratch).
REAL_HOME=$(python3 -c 'import os,pwd; print(pwd.getpwuid(os.getuid()).pw_dir)')
CALLER_BREW_PREFIX=${HOMEBREW_PREFIX:-}

# ---- release under test -------------------------------------------------
[ $# -le 1 ] || die "usage: $0 [TAG]"
[ $# -eq 0 ] || RELEASE_TAG=$1
if [ -z "${RELEASE_TAG:-}" ]; then
    v=$(sed -n 's/^project(den VERSION \([0-9.]*\).*/\1/p' "$REPO_DIR/CMakeLists.txt" 2>/dev/null)
    [ -n "$v" ] || die "RELEASE_TAG unset and no version in CMakeLists.txt"
    RELEASE_TAG="v$v"
fi
RELEASE_TAG="v${RELEASE_TAG#v}"
echo "$RELEASE_TAG" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+(-rc\.[0-9]+)?$' \
    || die "invalid release tag '$RELEASE_TAG' (expected vX.Y.Z or vX.Y.Z-rc.N)"
# What `den --version` prints and the release's asset names use: an RC
# ships GA-named assets (den-<X.Y.Z>-...).
VERSION=${RELEASE_TAG#v}
VERSION=${VERSION%%-*}
VR_INSTALL_SH=${VR_INSTALL_SH:-$REPO_DIR/install.sh}
[ -f "$VR_INSTALL_SH" ] || die "installer not found: $VR_INSTALL_SH"
VR_RELEASE_URL=${VR_RELEASE_URL:-https://github.com/marcelocantos/den/releases/download}
if [ -z "${VR_PREV_TAG+x}" ]; then
    VR_PREV_TAG=$(git -C "$REPO_DIR" tag -l 'v*' 2>/dev/null | sh "$SCRIPT_DIR/prev-release-tag.sh" "$RELEASE_TAG" 2>/dev/null || true)
fi
case "$(uname -s)" in
    Darwin) REL_OS=darwin ;;
    Linux) REL_OS=linux ;;
    *) die "unsupported OS: $(uname -s)" ;;
esac
case "$(uname -m)" in
    x86_64|amd64) REL_ARCH=x86_64 ;;
    aarch64|arm64) REL_ARCH=aarch64 ;;
    *) die "unsupported architecture: $(uname -m)" ;;
esac

# ---- scratch layout & guard ---------------------------------------------
VR_ROOT=${VR_ROOT:-$(mktemp -d /tmp/den-verify-release.XXXXXX)}
VR_ROOT=$(realpath_ "$VR_ROOT")
VR_HOME=${VR_HOME:-$VR_ROOT/home}
VR_DEN_HOME=${VR_DEN_HOME:-$VR_ROOT/denhome}
VR_PREFIX=${VR_PREFIX:-$VR_ROOT/prefix}
VR_APPDIR=${VR_APPDIR:-$VR_ROOT/apps}
VR_FONTDIR=${VR_FONTDIR:-$VR_ROOT/fonts}
VR_PKG_ROOT=${VR_PKG_ROOT:-$VR_ROOT/pkgroot}
VR_INSTALL_DIR=${VR_INSTALL_DIR:-$VR_ROOT/install}
VR_PREV_DIR=$VR_ROOT/prev
LOGDIR=$VR_ROOT/logs

FORBIDDEN="/opt/homebrew /usr/local /home/linuxbrew/.linuxbrew $REAL_HOME $CALLER_BREW_PREFIX"
for p in "$VR_ROOT" "$VR_HOME" "$VR_DEN_HOME" "$VR_PREFIX" "$VR_APPDIR" "$VR_FONTDIR" "$VR_PKG_ROOT" "$VR_INSTALL_DIR"; do
    for f in $FORBIDDEN; do
        [ -n "$f" ] && [ -e "$f" ] || continue
        if under "$p" "$f"; then die "refusing scratch path $p (under $f)"; fi
    done
done

# Only create anything once every path has cleared the guard.
mkdir -p "$VR_HOME" "$VR_DEN_HOME" "$VR_PREFIX" "$VR_APPDIR" "$VR_FONTDIR" "$VR_PKG_ROOT" "$LOGDIR"

# No Homebrew: drop its bin dirs from PATH and every dynamic-loader override,
# so neither install.sh nor den can pick up a brew tool or library.
clean_path=""
IFS=: read -r -a _path_parts <<<"${PATH:-/usr/bin:/bin}"
for d in "${_path_parts[@]}"; do
    case "$d" in
        ""|/opt/homebrew|/opt/homebrew/*|/home/linuxbrew/.linuxbrew|/home/linuxbrew/.linuxbrew/*) continue ;;
    esac
    clean_path="${clean_path:+$clean_path:}$d"
done
export PATH="${clean_path:-/usr/bin:/bin}"
for v in $(env | sed -n 's/^\(DYLD_[A-Za-z0-9_]*\)=.*/\1/p'); do unset "$v"; done
unset DEN_ENV DEN_STORE DEN_CONFIG
export HOME="$VR_HOME" DEN_HOME="$VR_DEN_HOME" DEN_PREFIX="$VR_PREFIX"
# den reads HOMEBREW_PREFIX only to locate an existing Homebrew (migrate,
# diagnostics); point it at scratch so nothing real is even read.
export HOMEBREW_PREFIX="$VR_PREFIX" HOMEBREW_CELLAR="$VR_PREFIX/Cellar"
export DEN_APPDIR="$VR_APPDIR" DEN_FONTDIR="$VR_FONTDIR" DEN_CASK_PKG_ROOT="$VR_PKG_ROOT"
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_ANALYTICS=1

DEN_BIN=$VR_INSTALL_DIR/bin/den

VR_BOTTLE=${VR_BOTTLE:-tree}
VR_DEP_PARENT=${VR_DEP_PARENT:-jq}
VR_DEP_CHILD=${VR_DEP_CHILD:-oniguruma}
VR_TAP=${VR_TAP:-marcelocantos/tap}
VR_TAP_FORMULA=${VR_TAP_FORMULA:-cv}
VR_FONT_CASK=${VR_FONT_CASK:-font-hack}
VR_APP_CASK=${VR_APP_CASK:-macdown}
VR_APP_BUNDLE=${VR_APP_BUNDLE:-MacDown.app}
VR_LAUNCHD_SECS=${VR_LAUNCHD_SECS:-60}
VR_REPLICA_ROOT=${VR_REPLICA_ROOT:-$VR_INSTALL_DIR}
[ "${VR_SKIP_LAUNCHD:-0}" = 1 ] && VR_SKIP="${VR_SKIP:-} launchd"

# ---- step runner ----------------------------------------------------------
PASSED=0; FAILED=0; SKIPPED=0; FAILS=""; N=0
SKIP_RC=77
skip_because() { echo "skip-reason: $*"; exit "$SKIP_RC"; }
den() { "$DEN_BIN" "$@"; }
wanted() {
    # Safety gates: never skippable.
    case "$1" in install|isolation) return 0 ;; esac
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


sha256_() {
    if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}'
    else sha256sum "$1" | awk '{print $1}'; fi
}
# run_install DIR TAG: install.sh with every location in scratch, no PATH
# edits and no prompts (it never needs sudo: the scratch prefix is writable).
run_install() {
    env DEN_INSTALL_DIR="$1" DEN_VERSION="$2" DEN_NO_MODIFY_PATH=1 DEN_INSTALL_INTERACTIVE=0 \
        sh "$VR_INSTALL_SH" </dev/null
}

# ---- steps ----------------------------------------------------------------
s_install() {
    run_install "$VR_INSTALL_DIR" "$RELEASE_TAG"
    [ -x "$DEN_BIN" ] || { echo "install.sh left no executable $DEN_BIN"; return 1; }
    [ -f "$VR_INSTALL_DIR/share/den/known_hashes.json" ] || { echo "no trust replica under $VR_INSTALL_DIR/share/den"; return 1; }
}
s_version() {
    local got; got=$(den --version)
    echo "den --version: $got (expected $VERSION, tag $RELEASE_TAG)"
    [ "$got" = "$VERSION" ]
}
# The released binary must load nothing a stock OS lacks (no Homebrew libs).
s_links() { sh "$SCRIPT_DIR/check-links.sh" "$DEN_BIN"; }
# The installed bytes are the release asset, and the asset matches both the
# per-asset .sha256 and the release-wide checksums file.
s_checksum() {
    local d=$VR_ROOT/release asset sha want_sum want_side
    asset="den-$VERSION-$REL_OS-$REL_ARCH.tar.gz"
    mkdir -p "$d/x"
    curl -fsSL "$VR_RELEASE_URL/$RELEASE_TAG/$asset" -o "$d/$asset"
    curl -fsSL "$VR_RELEASE_URL/$RELEASE_TAG/$asset.sha256" -o "$d/$asset.sha256"
    curl -fsSL "$VR_RELEASE_URL/$RELEASE_TAG/den-$VERSION-checksums.txt" -o "$d/checksums.txt"
    sha=$(sha256_ "$d/$asset")
    want_side=$(awk '{print $1; exit}' "$d/$asset.sha256")
    want_sum=$(awk -v f="$asset" '$2 == f || $2 == "*" f {print $1; exit}' "$d/checksums.txt")
    echo "sha256 $asset: $sha (sidecar ${want_side:-none}, checksums ${want_sum:-none})"
    [ -n "$want_sum" ] || { echo "$asset missing from den-$VERSION-checksums.txt"; return 1; }
    [ "$sha" = "$want_side" ] || { echo "sidecar .sha256 mismatch"; return 1; }
    [ "$sha" = "$want_sum" ] || { echo "checksums.txt mismatch"; return 1; }
    tar -xzf "$d/$asset" -C "$d/x"
    cmp "$d/x/den" "$DEN_BIN" || { echo "installed den differs from the release asset"; return 1; }
    if [ -f "$d/x/share/den/known_hashes.json" ]; then
        cmp "$d/x/share/den/known_hashes.json" "$VR_INSTALL_DIR/share/den/known_hashes.json" \
            || { echo "installed trust replica differs from the release asset"; return 1; }
    fi
}
# Start from the previous GA (installed by install.sh into its own scratch
# dir) and self-update to the tag under test: the result must be the very
# bytes install.sh put in place, replica included.
s_self_update() {
    [ -n "$VR_PREV_TAG" ] || skip_because "no previous GA tag (set VR_PREV_TAG)"
    [ "v${VR_PREV_TAG#v}" != "$RELEASE_TAG" ] || skip_because "VR_PREV_TAG is the tag under test"
    local prev=$VR_PREV_DIR/bin/den got
    run_install "$VR_PREV_DIR" "$VR_PREV_TAG"
    echo "previous: $("$prev" --version) ($VR_PREV_TAG)"
    if "$prev" self-update --help 2>&1 | grep -q -- '--version'; then
        "$prev" self-update --version "$RELEASE_TAG"
    else
        # Releases before self-update --version can only move to Latest.
        case "$RELEASE_TAG" in
            *-rc.*) skip_because "$VR_PREV_TAG has no 'self-update --version' (needed to reach an RC)" ;;
        esac
        local latest
        latest=$(curl -fsSL https://api.github.com/repos/marcelocantos/den/releases/latest \
            | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -1)
        [ "$latest" = "$RELEASE_TAG" ] || skip_because "$VR_PREV_TAG has no 'self-update --version' and $RELEASE_TAG is not Latest ($latest)"
        "$prev" self-update
    fi
    got=$("$prev" --version); echo "after self-update: $got"
    [ "$got" = "$VERSION" ] || { echo "expected $VERSION"; return 1; }
    cmp "$prev" "$DEN_BIN" || { echo "self-updated den differs from the install.sh one"; return 1; }
    [ -f "$VR_PREV_DIR/share/den/known_hashes.json" ] || { echo "self-update left no trust replica"; return 1; }
    cmp "$VR_PREV_DIR/share/den/known_hashes.json" "$VR_INSTALL_DIR/share/den/known_hashes.json" \
        || { echo "self-updated trust replica differs from the install.sh one"; return 1; }
}
# A fresh store is empty (what the Homebrew formula's `test do` checked).
s_empty_store() {
    local t=$VR_ROOT/empty-store got
    mkdir -p "$t"
    got=$(DEN_HOME="$t/.den" DEN_PREFIX="$t/prefix" den list); echo "den list: $got"
    echo "$got" | grep -q "No packages installed" || { echo "den list did not report 'No packages installed'"; return 1; }
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
# `den daemon install` writes the real agent plist (label dev.den.daemon)
# into the scratch HOME's LaunchAgents and then loads it. That load would
# replace the account's own den agent, so it runs against a stub launchctl;
# the generated plist is then checked, relabelled and loaded for real under
# a unique label.
s_launchd() {
    [ "$(uname)" = Darwin ] || { echo "launchd step is macOS-only"; return 1; }
    local stub=$VR_ROOT/stub-bin src=$VR_HOME/Library/LaunchAgents/dev.den.daemon.plist
    mkdir -p "$stub"
    printf '#!/bin/sh\necho "launchctl $*" >>"%s/launchctl.log"\n' "$VR_ROOT" >"$stub/launchctl"
    chmod +x "$stub/launchctl"
    PATH="$stub:$PATH" den daemon install
    [ -f "$src" ] || { echo "den daemon install wrote no $src"; return 1; }
    grep -q bootstrap "$VR_ROOT/launchctl.log" || { echo "den daemon install did not bootstrap its agent"; return 1; }
    local label prog a1 a2 uid pid0 pid st t=0
    label=$(plutil -extract Label raw "$src")
    prog=$(plutil -extract ProgramArguments.0 raw "$src")
    a1=$(plutil -extract ProgramArguments.1 raw "$src")
    a2=$(plutil -extract ProgramArguments.2 raw "$src")
    echo "service: $label: $prog $a1 $a2"
    [ "$label" = dev.den.daemon ] || { echo "unexpected label $label"; return 1; }
    [ "$a1 $a2" = "daemon run" ] || { echo "service does not run 'den daemon run'"; return 1; }
    [ "$(realpath_ "$prog")" = "$(realpath_ "$DEN_BIN")" ] || { echo "service binary $prog is not $DEN_BIN"; return 1; }
    [ "$(plutil -extract EnvironmentVariables.DEN_HOME raw "$src")" = "$VR_DEN_HOME" ] || { echo "plist DEN_HOME is not $VR_DEN_HOME"; return 1; }
    [ "$(plutil -extract EnvironmentVariables.DEN_PREFIX raw "$src")" = "$VR_PREFIX" ] || { echo "plist DEN_PREFIX is not $VR_PREFIX"; return 1; }
    echo "plist PATH: $(plutil -extract EnvironmentVariables.PATH raw "$src" 2>/dev/null || echo none)"

    uid=$(id -u)
    trap la_teardown EXIT
    cp "$src" "$LA_PLIST"; chmod u+w "$LA_PLIST"
    plutil -replace Label -string "$LA_LABEL" "$LA_PLIST"
    local k
    for k in HOME HOMEBREW_PREFIX HOMEBREW_CELLAR DEN_APPDIR DEN_FONTDIR DEN_CASK_PKG_ROOT; do
        plutil -replace "EnvironmentVariables.$k" -string "${!k}" "$LA_PLIST"
    done
    # No Homebrew on the agent's PATH either.
    plutil -replace EnvironmentVariables.PATH -string "/usr/bin:/bin:/usr/sbin:/sbin" "$LA_PLIST"
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
        [ "$pid" = "$pid0" ] || { echo "agent died or restarted (pid $pid0 -> ${pid:-none})"; tail -20 "$VR_DEN_HOME/daemon.log" 2>/dev/null; return 1; }
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

echo "verify-release: tag=$RELEASE_TAG version=$VERSION prev=${VR_PREV_TAG:-none} root=$VR_ROOT"
echo "verify-release: installer=$VR_INSTALL_SH PATH=$PATH"
step install s_install
step version s_version
step isolation s_isolation
# Abort on the gates: no binary, the wrong binary, or an unconfined den.
case " $FAILS " in
    *" install "*) echo "verify-release: install.sh failed for $RELEASE_TAG — aborting"; exit 1 ;;
    *" version "*) echo "verify-release: version failed (den under test is not $VERSION) — aborting before den writes anything"; exit 1 ;;
    *" isolation "*) echo "verify-release: isolation failed (den config escapes $VR_ROOT) — aborting before den writes anything"; exit 1 ;;
esac
step links s_links
step checksum s_checksum
step self-update s_self_update
step empty-store s_empty_store
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

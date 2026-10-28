#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Tests for scripts/verify-release.sh against a stub installer, curl and den
# (no network, no Homebrew, nothing outside a temp dir):
#   * the tag argument / RELEASE_TAG: GA and RC, with or without "v"; a
#     malformed tag is refused before anything is created.
#   * install.sh is run with every location in scratch (DEN_INSTALL_DIR,
#     HOME, DEN_HOME, DEN_PREFIX), no PATH edits, no prompts, Homebrew dirs
#     stripped from PATH and DYLD_* unset.
#   * the install, version and isolation gates abort the run.
#   * scratch paths under a forbidden root are refused.
#   * checksum compares the installed bytes with the release asset and both
#     checksum files; self-update starts from VR_PREV_TAG.
#   * VR_ONLY / VR_SKIP select steps; install and isolation always run.
#
# Run:
#   sh tests/scripts/test_verify_release.sh
# or via ctest (name: script_test_verify_release).

set -u

_SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT="${_SELF_DIR}/../.."
VR="${ROOT}/scripts/verify-release.sh"

for tool in bash python3 tar; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "SKIP: $tool not found"
        exit 0
    fi
done
if command -v shasum >/dev/null 2>&1; then SHA="shasum -a 256"
elif command -v sha256sum >/dev/null 2>&1; then SHA=sha256sum
else echo "SKIP: no sha256 tool"; exit 0; fi

_pass=0
_fail=0
_tmp=$(mktemp -d "${TMPDIR:-/tmp}/vr-test.XXXXXX")
_tmp=$(CDPATH= cd -P -- "$_tmp" && pwd)
trap 'chmod -R u+w "$_tmp" 2>/dev/null; rm -rf "$_tmp"' EXIT

ok() { _pass=$((_pass + 1)); echo "PASS: $1"; }
bad() { _fail=$((_fail + 1)); echo "FAIL: $1"; [ -f "$_tmp/out" ] && sed 's/^/    /' "$_tmp/out"; }

case "$(uname -s)" in Darwin) OS=darwin ;; *) OS=linux ;; esac
case "$(uname -m)" in x86_64|amd64) ARCH=x86_64 ;; *) ARCH=aarch64 ;; esac

# Stub den. FAKE_VERSION is baked in per release; config reports the scratch
# store unless FAKE_DEN_ESCAPE=1; self-update --version copies the release's
# den (and replica) over itself, as the real one does.
mkden() { # mkden DIR VERSION
    mkdir -p "$1"
    cat >"$1/den" <<DEN
#!/bin/sh
case "\$1" in
    --version) echo $2 ;;
    config) [ "\${FAKE_DEN_ESCAPE:-0}" = 1 ] && DEN_PREFIX=/opt/elsewhere
            printf 'den_home: %s\nstore: %s/store\nprefix: %s\n' "\$DEN_HOME" "\$DEN_HOME" "\$DEN_PREFIX" ;;
    list) echo "No packages installed" ;;
    self-update)
        [ "\$2" = --help ] && { echo "  --version TEXT  release to install"; exit 0; }
        [ "\$2" = --version ] || exit 1
        echo "self-update \$3" >>"\$FAKE_LOG"
        here=\$(cd "\$(dirname "\$0")" && pwd)
        cp "\$FAKE_REL/\$3/x/den" "\$here/den.new" && mv "\$here/den.new" "\$here/den"
        mkdir -p "\$here/../share/den" && cp "\$FAKE_REL/\$3/x/share/den/known_hashes.json" "\$here/../share/den/" ;;
    *) exit 0 ;;
esac
DEN
    chmod 755 "$1/den"
}
# Fake release: FAKE_REL/<tag>/{x/ (unpacked), den-<v>-<os>-<arch>.tar.gz, .sha256, checksums.txt}
mkrel() { # mkrel TAG VERSION
    d="$_tmp/rel/$1"
    mkden "$d/x" "$2"
    mkdir -p "$d/x/share/den"; echo "{\"tag\":\"$1\"}" >"$d/x/share/den/known_hashes.json"
    a="den-$2-$OS-$ARCH.tar.gz"
    (cd "$d/x" && tar -czf "../$a" den share)
    (cd "$d" && $SHA "$a" >"$a.sha256" && { echo "0000  den-$2-other.tar.gz"; cat "$a.sha256"; } >"den-$2-checksums.txt")
}
mkrel v1.2.0-rc.1 1.2.0
mkrel v1.2.0 1.2.0
mkrel v1.1.0 1.1.0

# Stub installer: records what it saw, then unpacks FAKE_REL/<tag> into
# DEN_INSTALL_DIR (FAKE_INSTALL_FAIL=1 fails instead).
cat >"$_tmp/install.sh" <<'INST'
tag="v${DEN_VERSION#v}"
{
    echo "install tag=$tag dir=$DEN_INSTALL_DIR home=$HOME den_home=$DEN_HOME prefix=$DEN_PREFIX"
    echo "  nomodify=${DEN_NO_MODIFY_PATH:-} interactive=${DEN_INSTALL_INTERACTIVE:-} path=$PATH"
    env | grep '^DYLD_' | sed 's/^/  dyld: /'
} >>"$FAKE_LOG"
[ "${FAKE_INSTALL_FAIL:-0}" = 1 ] && { echo "boom" >&2; exit 1; }
mkdir -p "$DEN_INSTALL_DIR/bin" "$DEN_INSTALL_DIR/share/den"
cp "$FAKE_REL/$tag/x/den" "$DEN_INSTALL_DIR/bin/den"
cp "$FAKE_REL/$tag/x/share/den/known_hashes.json" "$DEN_INSTALL_DIR/share/den/"
INST

# Stub curl serving FAKE_REL by URL tail (.../<tag>/<asset>).
mkdir -p "$_tmp/bin"
cat >"$_tmp/bin/curl" <<'CURL'
#!/bin/sh
out=""; url=""
while [ $# -gt 0 ]; do
    case "$1" in -o) out=$2; shift ;; -*) ;; *) url=$1 ;; esac
    shift
done
echo "curl $url" >>"$FAKE_LOG"
asset=${url##*/}; rest=${url%/*}; tag=${rest##*/}
[ -f "$FAKE_REL/$tag/$asset" ] || exit 22
cp "$FAKE_REL/$tag/$asset" "$out"
CURL
chmod 755 "$_tmp/bin/curl"

# run NAME WANT_RC [VAR=VAL...] -- [ARGS...]: run verify-release (stubs first
# on PATH, plus Homebrew dirs and a DYLD_ variable that must be stripped).
run() {
    _name=$1; _want=$2; shift 2
    : >"$_tmp/log"
    rm -rf "$_tmp/vr"
    (
        export PATH="/opt/homebrew/bin:$_tmp/bin:/home/linuxbrew/.linuxbrew/bin:$PATH" \
            DYLD_LIBRARY_PATH=/opt/homebrew/lib \
            FAKE_LOG="$_tmp/log" FAKE_REL="$_tmp/rel" \
            VR_INSTALL_SH="$_tmp/install.sh" VR_ROOT="$_tmp/vr" VR_PREV_TAG=v1.1.0 \
            VR_ONLY="version"
        while [ $# -gt 0 ] && [ "$1" != -- ]; do export "${1?}"; shift; done
        [ $# -gt 0 ] && shift
        exec bash "$VR" "$@"
    ) >"$_tmp/out" 2>&1
    _rc=$?
    if [ "$_rc" -eq "$_want" ]; then return 0; fi
    bad "$_name: exit $_rc, want $_want"; return 1
}
has() { grep -Eq -- "$2" "$_tmp/out" && ok "$1" || bad "$1"; }
lacks() { grep -Eq -- "$2" "$_tmp/out" && bad "$1" || ok "$1"; }
logged() { grep -Eq -- "$2" "$_tmp/log" && ok "$1" || { bad "$1"; sed 's/^/    log| /' "$_tmp/log"; }; }
unlogged() { grep -Eq -- "$2" "$_tmp/log" && { bad "$1"; sed 's/^/    log| /' "$_tmp/log"; } || ok "$1"; }

# --- tags ----------------------------------------------------------------
if run "rc tag" 0 -- v1.2.0-rc.1; then
    has "rc: version 1.2.0 for tag v1.2.0-rc.1" 'tag=v1.2.0-rc.1 version=1.2.0 '
    has "rc: install/version/isolation pass" 'PASS  install'
    has "rc: version passes" 'PASS  version'
    has "rc: isolation passes" 'PASS  isolation'
    has "rc: unselected steps skipped" 'SKIP  bottle'
    logged "rc: installer asked for the tag" "install tag=v1.2.0-rc.1 dir=$_tmp/vr/install "
    logged "rc: installer HOME/DEN_HOME/DEN_PREFIX in scratch" "home=$_tmp/vr/home den_home=$_tmp/vr/denhome prefix=$_tmp/vr/prefix"
    logged "rc: no PATH edits, non-interactive" 'nomodify=1 interactive=0 '
    unlogged "rc: /opt/homebrew stripped from PATH" 'path=.*/opt/homebrew'
    unlogged "rc: linuxbrew stripped from PATH" 'path=.*linuxbrew'
    unlogged "rc: DYLD_* unset" 'dyld:'
fi
run "rc tag without v via RELEASE_TAG" 0 RELEASE_TAG=1.2.0-rc.1 -- \
    && logged "bare rc tag gets its v" 'install tag=v1.2.0-rc.1 '
run "ga tag without v" 0 -- 1.2.0 && logged "ga tag" 'install tag=v1.2.0 '
for t in 1.2 v1.2.0-beta1 1.2.0-rc. "1.2.0;rm"; do
    run "malformed tag $t" 2 -- "$t" && has "malformed $t: message" 'invalid release tag' \
        && { [ ! -e "$_tmp/vr" ] && ok "malformed $t: nothing created" || bad "malformed $t: nothing created"; }
done
run "two args" 2 -- v1.2.0 v1.1.0

# --- gates ---------------------------------------------------------------
run "install failure aborts" 1 FAKE_INSTALL_FAIL=1 -- v1.2.0 \
    && has "install failure: message" 'install.sh failed for v1.2.0 — aborting' \
    && lacks "install failure: no later steps" 'links'
# v1.1.0 installs a 1.1.0 binary, so pin the expected version mismatch by
# installing v1.2.0 bytes under the v1.3.0 tag.
mkdir -p "$_tmp/rel/v1.3.0"; cp -R "$_tmp/rel/v1.2.0/x" "$_tmp/rel/v1.3.0/x"
run "version mismatch aborts" 1 -- v1.3.0 \
    && has "version mismatch: message" 'version failed .* not 1.3.0'
run "isolation escape aborts" 1 FAKE_DEN_ESCAPE=1 VR_SKIP="isolation install" -- v1.2.0 \
    && has "isolation can't be skipped" 'FAIL  isolation' \
    && has "isolation escape: message" 'isolation failed'
run "forbidden scratch root refused" 2 VR_ROOT=/usr/local/den-vr-test -- v1.2.0 \
    && has "forbidden: message" 'refusing scratch path'
run "scratch under real home refused" 2 VR_ROOT="$(python3 -c 'import os,pwd; print(pwd.getpwuid(os.getuid()).pw_dir)')/den-vr-test-$$" -- v1.2.0 \
    && has "real home: message" 'refusing scratch path'

# --- checksum / self-update / empty-store --------------------------------
if run "checksum + self-update + empty-store pass" 0 VR_ONLY="checksum self-update empty-store" -- v1.2.0-rc.1; then
    has "checksum passes" 'PASS  checksum'
    has "self-update passes" 'PASS  self-update'
    has "empty-store passes" 'PASS  empty-store'
    logged "checksum: rc tag, GA asset names" "curl .*/v1.2.0-rc.1/den-1.2.0-$OS-$ARCH.tar.gz"
    logged "checksum: checksums file" 'curl .*/v1.2.0-rc.1/den-1.2.0-checksums.txt'
    logged "self-update: previous GA installed separately" "install tag=v1.1.0 dir=$_tmp/vr/prev "
    logged "self-update: to the tag" 'self-update v1.2.0-rc.1'
fi
# Tamper with the published checksum: the step must fail.
cp "$_tmp/rel/v1.2.0/den-1.2.0-checksums.txt" "$_tmp/ck.bak"
sed "s/^[0-9a-f]\{64\}/$(printf '%064d' 0)/" "$_tmp/ck.bak" >"$_tmp/rel/v1.2.0/den-1.2.0-checksums.txt"
run "checksum mismatch fails" 1 VR_ONLY=checksum -- v1.2.0 && has "checksum mismatch: message" 'checksums.txt mismatch'
cp "$_tmp/ck.bak" "$_tmp/rel/v1.2.0/den-1.2.0-checksums.txt"
run "no previous GA skips self-update" 0 VR_ONLY=self-update VR_PREV_TAG= -- v1.2.0 \
    && has "no prev: skip reason" 'SKIP  self-update .*no previous GA'

# --- step selection ------------------------------------------------------
run "VR_SKIP" 0 VR_ONLY="version empty-store" VR_SKIP="empty-store" -- v1.2.0 \
    && has "VR_SKIP skips" 'SKIP  empty-store'
run "VR_SKIP_LAUNCHD" 0 VR_ONLY="launchd" VR_SKIP_LAUNCHD=1 -- v1.2.0 \
    && has "VR_SKIP_LAUNCHD skips launchd" 'SKIP  launchd'

echo
echo "${_pass} passed, ${_fail} failed"
[ "$_fail" -eq 0 ]

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
#     checksum files; self-update starts from VR_PREV_TAG, or (unset) from
#     the newest earlier tag, RCs included, that can self-update --version.
#   * release downloads retry transient curl errors, never a 404 or a
#     checksum mismatch.
#   * a step fails when any of its commands fails, not just its last one
#     (set -e stays live inside step functions).
#   * the app-cask step defaults to hex-fiend and fails by name, before any
#     install, when the index marks its fixture cask disabled; its binary link
#     (hexf) must appear on install and be gone after uninstall (macOS only).
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

# verify-release picks its previous-GA fallback from the checkout's git tags
# (`git -C <repo> tag -l`). Point git at an empty repository so the real
# release tags of a full checkout don't leak into the fake releases below:
# every run sees no tags, as in a shallow CI checkout.
git init -q --bare "$_tmp/notags.git" || { echo "FAIL: git init"; exit 1; }
GIT_DIR="$_tmp/notags.git"; export GIT_DIR
unset GIT_WORK_TREE GIT_INDEX_FILE GIT_COMMON_DIR
if [ -n "$(git -C "$ROOT" tag -l 'v*')" ]; then
    echo "FAIL: git tags still visible to verify-release"; exit 1
fi

ok() { _pass=$((_pass + 1)); echo "PASS: $1"; }
bad() { _fail=$((_fail + 1)); echo "FAIL: $1"; [ -f "$_tmp/out" ] && sed 's/^/    /' "$_tmp/out"; }

case "$(uname -s)" in Darwin) OS=darwin ;; *) OS=linux ;; esac
case "$(uname -m)" in x86_64|amd64) ARCH=x86_64 ;; *) ARCH=aarch64 ;; esac

# Stub den. FAKE_VERSION is baked in per release; config reports the scratch
# store unless FAKE_DEN_ESCAPE=1; self-update --version copies the release's
# den (and replica) over itself, as the real one does.
# With OLD=1 it predates `self-update --version` (help lacks the flag).
# FAKE_DEN_FAIL lists subcommands that fail (exit 1).
mkden() { # mkden DIR VERSION [old]
    mkdir -p "$1"
    _help='  --version TEXT  release to install'
    [ "${3:-}" = old ] && _help='  --check  only report'
    cat >"$1/den" <<DEN
#!/bin/sh
case " \${FAKE_DEN_FAIL:-} " in *" \$1 "*) echo "den \$1: injected failure" >&2; exit 1 ;; esac
case "\$1" in
    --version) echo $2 ;;
    config) [ "\${FAKE_DEN_ESCAPE:-0}" = 1 ] && DEN_PREFIX=/opt/elsewhere
            printf 'den_home: %s\nstore: %s/store\nprefix: %s\n' "\$DEN_HOME" "\$DEN_HOME" "\$DEN_PREFIX" ;;
    list) echo "No packages installed" ;;
    self-update)
        [ "\$2" = --help ] && { echo "$_help"; exit 0; }
        [ "\$2" = --version ] || exit 1
        echo "self-update \$3" >>"\$FAKE_LOG"
        here=\$(cd "\$(dirname "\$0")" && pwd)
        cp "\$FAKE_REL/\$3/x/den" "\$here/den.new" && mv "\$here/den.new" "\$here/den"
        mkdir -p "\$here/../share/den" && cp "\$FAKE_REL/\$3/x/share/den/known_hashes.json" "\$here/../share/den/" ;;
    update) # FAKE_INDEX stands in for the index den update writes.
        [ -n "\${FAKE_INDEX:-}" ] || exit 0
        mkdir -p "\$DEN_HOME/cache" && cp "\$FAKE_INDEX" "\$DEN_HOME/cache/index.json" ;;
    install|uninstall) # an app cask's bundle is FAKE_APP_BUNDLE, its binary
        # link FAKE_APP_BINARY (left behind on uninstall if FAKE_KEEP_BINARY=1)
        echo "den \$1 \$2" >>"\$FAKE_LOG"
        b="\$DEN_APPDIR/\${FAKE_APP_BUNDLE:-none.app}"
        l="\$DEN_HOME/envs/default/bin/\${FAKE_APP_BINARY:-none}"
        if [ "\$1" = install ]; then
            mkdir -p "\$b"
            [ -z "\${FAKE_APP_BINARY:-}" ] || { mkdir -p "\${l%/*}"; ln -sf "\$b/x" "\$l"; }
        else
            rm -rf "\$b"
            [ "\${FAKE_KEEP_BINARY:-0}" = 1 ] || rm -f "\$l"
        fi ;;
    *) exit 0 ;;
esac
DEN
    chmod 755 "$1/den"
}
# Fake release: FAKE_REL/<tag>/{x/ (unpacked), den-<v>-<os>-<arch>.tar.gz, .sha256, checksums.txt}
mkrel() { # mkrel TAG VERSION [old]
    d="$_tmp/rel/$1"
    mkden "$d/x" "$2" "${3:-}"
    mkdir -p "$d/x/share/den"; echo "{\"tag\":\"$1\"}" >"$d/x/share/den/known_hashes.json"
    a="den-$2-$OS-$ARCH.tar.gz"
    (cd "$d/x" && tar -czf "../$a" den share)
    (cd "$d" && $SHA "$a" >"$a.sha256" && { echo "0000  den-$2-other.tar.gz"; cat "$a.sha256"; } >"den-$2-checksums.txt")
}
mkrel v1.2.0-rc.1 1.2.0
mkrel v1.2.0 1.2.0
mkrel v1.1.0 1.1.0
mkrel v1.0.0 1.0.0 old

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
out=""; url=""; w=""
while [ $# -gt 0 ]; do
    case "$1" in -o) out=$2; shift ;; -w) w=$2; shift ;; -*) ;; *) url=$1 ;; esac
    shift
done
echo "curl $url" >>"$FAKE_LOG"
# Failure injection: the first FAKE_CURL_FAIL_TIMES requests for a URL
# containing FAKE_CURL_FAIL_MATCH exit FAKE_CURL_FAIL_CODE.
if [ -n "${FAKE_CURL_FAIL_CODE:-}" ]; then
    case "$url" in *"$FAKE_CURL_FAIL_MATCH"*)
        n=$(cat "$FAKE_CURL_FAIL_STATE" 2>/dev/null || echo 0)
        if [ "$n" -lt "$FAKE_CURL_FAIL_TIMES" ]; then
            echo $((n + 1)) >"$FAKE_CURL_FAIL_STATE"
            [ -n "$w" ] && printf '%s' "${FAKE_CURL_FAIL_HTTP:-000}"
            exit "$FAKE_CURL_FAIL_CODE"
        fi ;;
    esac
fi
asset=${url##*/}; rest=${url%/*}; tag=${rest##*/}
[ -f "$FAKE_REL/$tag/$asset" ] || { [ -n "$w" ] && printf 404; exit 22; }
cp "$FAKE_REL/$tag/$asset" "$out"
[ -n "$w" ] && printf 200
exit 0
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
        # -VAR unsets VAR (e.g. -VR_PREV_TAG for the automatic pick).
        while [ $# -gt 0 ] && [ "$1" != -- ]; do
            case "$1" in -*) unset "${1#-}" ;; *) export "${1?}" ;; esac
            shift
        done
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

# --- automatic previous release ------------------------------------------
# Newest earlier tag, RCs included, whose binary can self-update --version.
if run "auto prev: newest capable RC" 0 VR_ONLY=self-update -VR_PREV_TAG \
    VR_PREV_CANDIDATES="v1.2.0-rc.1 v1.1.0 v1.0.0" -- v1.2.0; then
    has "auto prev: self-update passes" 'PASS  self-update'
    logged "auto prev: rc installed as the start" "install tag=v1.2.0-rc.1 dir=$_tmp/vr/prev "
    unlogged "auto prev: older candidates untouched" 'install tag=v1.1.0'
    logged "auto prev: to the tag" 'self-update v1.2.0$'
    has "auto prev: banner" 'prev=auto \(3 candidates'
fi
# The newest candidate predates self-update --version; a missing release is
# skipped too; the walk lands on v1.1.0.
mkdir -p "$_tmp/rel/v1.1.5-rc.1"
if run "auto prev: skips incapable and missing" 0 VR_ONLY=self-update -VR_PREV_TAG \
    VR_PREV_CANDIDATES="v1.1.9 v1.0.0 v1.1.0" -- v1.2.0-rc.1; then
    logged "auto prev: tried the missing tag" 'install tag=v1.1.9 '
    logged "auto prev: tried the old one" 'install tag=v1.0.0 '
    logged "auto prev: picked v1.1.0" "install tag=v1.1.0 dir=$_tmp/vr/prev "
    logged "auto prev: self-update to the rc" 'self-update v1.2.0-rc.1'
fi
# None capable: fall back to the GA path; an RC target then skips.
if run "auto prev: none capable" 0 VR_ONLY=self-update -VR_PREV_TAG \
    VR_PREV_CANDIDATES="v1.0.0" -- v1.2.0-rc.1; then
    has "auto prev: none capable skips for an rc" "SKIP  self-update .*no 'self-update --version'"
fi
if run "auto prev: VR_PREV_MAX bounds the walk" 0 VR_ONLY=self-update -VR_PREV_TAG \
    VR_PREV_CANDIDATES="v1.0.0 v1.1.0" VR_PREV_MAX=1 -- v1.2.0-rc.1; then
    unlogged "auto prev: VR_PREV_MAX=1 stops after one" "install tag=v1.1.0 dir=$_tmp/vr/prev "
fi
# An explicit VR_PREV_TAG overrides the walk.
if run "VR_PREV_TAG overrides" 0 VR_ONLY=self-update VR_PREV_TAG=v1.1.0 \
    VR_PREV_CANDIDATES="v1.2.0-rc.1" -- v1.2.0; then
    logged "override: v1.1.0 used" "install tag=v1.1.0 dir=$_tmp/vr/prev "
    unlogged "override: candidates ignored" 'install tag=v1.2.0-rc.1'
fi

# --- transient download errors -------------------------------------------
_ck_url="/v1.2.0/den-1.2.0-$OS-$ARCH.tar.gz"
rm -f "$_tmp/fail.state"
if run "checksum: curl 56 once is retried" 0 VR_ONLY=checksum VR_FETCH_DELAY=0 \
    FAKE_CURL_FAIL_CODE=56 FAKE_CURL_FAIL_TIMES=1 FAKE_CURL_FAIL_MATCH="$_ck_url" \
    FAKE_CURL_FAIL_STATE="$_tmp/fail.state" -- v1.2.0; then
    has "retry: checksum passes" 'PASS  checksum'
    [ "$(grep -c "curl .*$_ck_url\$" "$_tmp/log")" -eq 2 ] && ok "retry: fetched twice" || bad "retry: fetched twice"
fi
rm -f "$_tmp/fail.state"
if run "checksum: curl 56 always fails after 3" 1 VR_ONLY=checksum VR_FETCH_DELAY=0 \
    FAKE_CURL_FAIL_CODE=56 FAKE_CURL_FAIL_TIMES=99 FAKE_CURL_FAIL_MATCH="$_ck_url" \
    FAKE_CURL_FAIL_STATE="$_tmp/fail.state" -- v1.2.0; then
    [ "$(grep -c "curl .*$_ck_url\$" "$_tmp/log")" -eq 3 ] && ok "retry: bounded at 3" || bad "retry: bounded at 3"
fi
rm -rf "$_tmp/rel/v1.4.0"; mkdir -p "$_tmp/rel/v1.4.0"; cp -R "$_tmp/rel/v1.2.0/x" "$_tmp/rel/v1.4.0/x"
run "checksum: 404 not retried" 1 VR_ONLY=checksum VR_FETCH_DELAY=0 VR_SKIP=version -- v1.4.0 \
    && { [ "$(grep -c 'curl .*/v1.4.0/den-1.4.0' "$_tmp/log")" -eq 1 ] && ok "404: one fetch" || bad "404: one fetch"; }
# A checksum mismatch is a verdict, not a transport error: one fetch each.
sed "s/^[0-9a-f]\{64\}/$(printf '%064d' 0)/" "$_tmp/ck.bak" >"$_tmp/rel/v1.2.0/den-1.2.0-checksums.txt"
run "checksum mismatch not retried" 1 VR_ONLY=checksum VR_FETCH_DELAY=0 -- v1.2.0 \
    && { [ "$(grep -c "curl .*$_ck_url\$" "$_tmp/log")" -eq 1 ] && ok "mismatch: one fetch" || bad "mismatch: one fetch"; }
cp "$_tmp/ck.bak" "$_tmp/rel/v1.2.0/den-1.2.0-checksums.txt"

# --- errexit inside steps -----------------------------------------------
# The upgrade step is `den upgrade; den outdated`: a failing `den upgrade`
# must fail the step even though the final `den outdated` succeeds.
run "upgrade step passes" 0 VR_ONLY=upgrade -- v1.2.0 \
    && has "upgrade: passes" 'PASS  upgrade'
run "non-final command failure fails the step" 1 VR_ONLY=upgrade FAKE_DEN_FAIL=upgrade -- v1.2.0 \
    && has "errexit: upgrade fails" 'FAIL  upgrade' \
    && has "errexit: log shows the failing command" 'den upgrade: injected failure'

# --- app-cask fixture ----------------------------------------------------
# The default app cask is hex-fiend; a fixture cask the index marks disabled
# fails the step at once, by name, before den is asked to install it.
cat >"$_tmp/index.json" <<'JSON'
{"packages": [
  {"name": "hex-fiend", "artifact_type": "app", "disabled": false},
  {"name": "macdown", "artifact_type": "app", "disabled": true,
   "disable_reason": "fails_gatekeeper_check", "disable_date": "2026-09-01"}
]}
JSON
if [ "$OS" = darwin ]; then
    if run "app-cask: default hex-fiend" 0 VR_ONLY="update app-cask" FAKE_INDEX="$_tmp/index.json" \
        "FAKE_APP_BUNDLE=Hex Fiend.app" FAKE_APP_BINARY=hexf -VR_APP_CASK -VR_APP_BUNDLE -VR_APP_BINARY -- v1.2.0; then
        has "app-cask: default passes" 'PASS  app-cask'
        logged "app-cask: default installs hex-fiend" '^den install hex-fiend$'
        logged "app-cask: default uninstalls hex-fiend" '^den uninstall hex-fiend$'
    fi
    if run "app-cask: disabled fixture fails" 1 VR_ONLY="update app-cask" FAKE_INDEX="$_tmp/index.json" \
        VR_APP_CASK=macdown VR_APP_BUNDLE=MacDown.app FAKE_APP_BUNDLE=MacDown.app -- v1.2.0; then
        has "app-cask: disabled fails the step" 'FAIL  app-cask'
        has "app-cask: disabled names the cask and reason" \
            "fixture cask 'macdown' is disabled upstream: fails_gatekeeper_check \\(disabled on 2026-09-01\\)"
        has "app-cask: disabled says what to set" 'set VR_APP_CASK to an enabled app cask'
        unlogged "app-cask: disabled cask never installed" '^den install macdown'
    fi
    # No index (update not run): nothing to check, den decides.
    run "app-cask: no index" 0 VR_ONLY=app-cask "FAKE_APP_BUNDLE=Hex Fiend.app" FAKE_APP_BINARY=hexf \
        -VR_APP_CASK -VR_APP_BUNDLE -VR_APP_BINARY -- v1.2.0 \
        && has "app-cask: no index passes" 'PASS  app-cask'
    # The binary link: required after install, gone after uninstall.
    run "app-cask: hexf link survives uninstall" 1 VR_ONLY=app-cask "FAKE_APP_BUNDLE=Hex Fiend.app" \
        FAKE_APP_BINARY=hexf FAKE_KEEP_BINARY=1 -VR_APP_CASK -VR_APP_BUNDLE -VR_APP_BINARY -- v1.2.0 \
        && has "app-cask: surviving link named" 'hexf link survives uninstall'
    run "app-cask: no hexf link" 1 VR_ONLY=app-cask "FAKE_APP_BUNDLE=Hex Fiend.app" \
        -VR_APP_CASK -VR_APP_BUNDLE -VR_APP_BINARY -- v1.2.0 \
        && has "app-cask: missing link named" 'hex-fiend linked no hexf binary'
    run "app-cask: VR_APP_BINARY= skips the link check" 0 VR_ONLY=app-cask "FAKE_APP_BUNDLE=Hex Fiend.app" \
        VR_APP_BINARY= -VR_APP_CASK -VR_APP_BUNDLE -- v1.2.0 \
        && has "app-cask: no binary check passes" 'PASS  app-cask'
else
    echo "SKIP: app-cask fixture tests (macOS only)"
fi

# --- update failure blocks the index-dependent steps ------------------------
# A failed update (the index fetch) reports the steps that need the index as
# SKIP (blocked by update) instead of FAIL; the other steps still run, and the
# update FAIL keeps the exit status non-zero.
_dep="bottle opt-link trust pin upgrade dependents uninstall"
_cask="font-cask app-cask"
_indep="tap doctor cleanup"
run "update failure blocks index steps" 1 FAKE_DEN_FAIL=update \
    VR_ONLY="version empty-store update $_dep $_cask $_indep" -- v1.2.0 && {
    has "blocked: update itself fails" '^FAIL  update '
    for _s in $_dep; do
        has "blocked: $_s skipped" "^SKIP  $_s +\(blocked by update\)$"
    done
    if [ "$OS" = darwin ]; then
        for _s in $_cask; do
            has "blocked: $_s skipped" "^SKIP  $_s +\(blocked by update\)$"
        done
    else
        # Off macOS the cask steps are not wanted at all, so not "blocked".
        for _s in $_cask; do
            has "blocked: $_s plain skip off macOS" "^SKIP  $_s *$"
        done
    fi
    has "blocked: step before update runs" '^PASS  empty-store '
    has "blocked: doctor runs" '^PASS  doctor '
    has "blocked: cleanup runs" '^PASS  cleanup '
    has "blocked: tap runs" '^(PASS|FAIL)  tap '
    lacks "blocked: tap not blocked" 'SKIP  tap'
    unlogged "blocked: no install attempted" '^den install '
    unlogged "blocked: no uninstall attempted" '^den uninstall '
    if [ "$OS" = darwin ]; then _n=9; else _n=7; fi
    has "blocked: summary counts blocked" "skipped \($_n blocked by update\) \(failed: update"
    has "blocked: summary names blocked" "^verify-release: not run because update failed: bottle opt-link "
}
run "update success does not block" 1 VR_ONLY="update bottle" -- v1.2.0 && {
    has "unblocked: update passes" '^PASS  update '
    logged "unblocked: bottle runs" '^den install tree$'
    lacks "unblocked: nothing blocked" 'blocked by update'
}
run "update not selected does not block" 0 VR_ONLY="upgrade" -- v1.2.0 && {
    has "unselected: upgrade runs" '^PASS  upgrade '
    lacks "unselected: nothing blocked" 'blocked by update'
}

# --- step selection ------------------------------------------------------
run "VR_SKIP" 0 VR_ONLY="version empty-store" VR_SKIP="empty-store" -- v1.2.0 \
    && has "VR_SKIP skips" 'SKIP  empty-store'
run "VR_SKIP_LAUNCHD" 0 VR_ONLY="launchd" VR_SKIP_LAUNCHD=1 -- v1.2.0 \
    && has "VR_SKIP_LAUNCHD skips launchd" 'SKIP  launchd'

echo
echo "${_pass} passed, ${_fail} failed"
[ "$_fail" -eq 0 ]

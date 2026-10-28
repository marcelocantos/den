#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Tests for install.sh. Everything external is faked: curl serves canned
# release assets from a scratch dir (and logs each URL), uname reports a
# chosen platform, and sudo only logs. PATH holds just those fakes plus
# symlinks to the plain tools the installer needs, so the real sudo, curl
# and network are unreachable and a "missing sha256 tool" host can be
# simulated by leaving shasum/sha256sum out.
#
# The store.prefix cases that depend on den's own config.json merge run the
# real den binary behind the canned release: DEN_TEST_BIN (set by ctest),
# else build/den. Without one they are skipped; the fallback for a den too
# old for `den get` always runs against the fake.
#
# Run:
#   sh tests/scripts/test_install_sh.sh
# or via ctest (name: script_test_install_sh).

set -u

_SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT="${_SELF_DIR}/../.."
SCRIPT="${ROOT}/install.sh"

_pass=0
_fail=0
_skip=0
_tmp=$(mktemp -d "${TMPDIR:-/tmp}/install-sh-test.XXXXXX")
_tmp=$(CDPATH= cd -- "$_tmp" && pwd -P)
trap 'chmod -R u+w "$_tmp" 2>/dev/null; rm -rf "$_tmp"' EXIT

if command -v shasum >/dev/null 2>&1; then
    _sha() { shasum -a 256 "$1" | awk '{print $1}'; }
    _sha_tool=shasum
elif command -v sha256sum >/dev/null 2>&1; then
    _sha() { sha256sum "$1" | awk '{print $1}'; }
    _sha_tool=sha256sum
else
    echo "SKIP: no sha256 tool on this host"
    exit 0
fi

# --- tool dirs ---------------------------------------------------------------
_tools="$_tmp/tools"     # plain tools, no sha256
_shabin="$_tmp/shabin"   # just the sha256 tool
_fake="$_tmp/fake"       # curl, uname, sudo
mkdir -p "$_tools" "$_shabin" "$_fake"
for t in sh tar gzip awk grep sed mktemp rm mv cp chmod mkdir cat dirname \
    basename id tr head ls env sleep; do
    _p=$(command -v "$t" 2>/dev/null) || continue
    case "$_p" in /*) ln -s "$_p" "$_tools/$t" ;; esac
done
ln -s "$(command -v "$_sha_tool")" "$_shabin/$_sha_tool"
# perl backs shasum on macOS.
if _p=$(command -v perl 2>/dev/null); then ln -s "$_p" "$_shabin/perl"; fi

cat >"$_fake/curl" <<'TOOL'
#!/bin/sh
out=""; url=""; w=""
while [ $# -gt 0 ]; do
    case "$1" in
        -o) out="$2"; shift 2 ;;
        -w) w="$2"; shift 2 ;;
        -*) shift ;;
        *) url="$1"; shift ;;
    esac
done
echo "$url" >>"$FAKE_CURL_LOG"
# Failure injection: the first FAKE_CURL_FAIL_TIMES requests whose URL
# contains FAKE_CURL_FAIL_MATCH exit FAKE_CURL_FAIL_CODE (printing
# FAKE_CURL_FAIL_HTTP for -w), counted in FAKE_CURL_FAIL_STATE.
if [ -n "${FAKE_CURL_FAIL_CODE:-}" ]; then
    case "$url" in
        *"$FAKE_CURL_FAIL_MATCH"*)
            n=$(cat "$FAKE_CURL_FAIL_STATE" 2>/dev/null || echo 0)
            if [ "$n" -lt "$FAKE_CURL_FAIL_TIMES" ]; then
                echo $((n + 1)) >"$FAKE_CURL_FAIL_STATE"
                [ -n "$w" ] && printf '%s' "${FAKE_CURL_FAIL_HTTP:-000}"
                exit "$FAKE_CURL_FAIL_CODE"
            fi ;;
    esac
fi
case "$url" in
    */releases/latest)
        [ -n "$w" ] && printf 'https://github.com/marcelocantos/den/releases/tag/%s' "$FAKE_LATEST"
        exit 0 ;;
    */releases/download/*)
        f="$FAKE_REL/${url#*/releases/download/}"
        [ -f "$f" ] || exit 22
        cp "$f" "$out" ;;
    *) exit 22 ;;
esac
TOOL
cat >"$_fake/uname" <<'TOOL'
#!/bin/sh
case "$1" in
    -s) echo "$FAKE_OS" ;;
    -m) echo "$FAKE_ARCH" ;;
    *) echo "$FAKE_OS" ;;
esac
TOOL
# Never escalates: logs, then (for tests that accept the prompt) creates the
# directory after unlocking its parent. chown is only logged.
cat >"$_fake/sudo" <<'TOOL'
#!/bin/sh
echo "$*" >>"$FAKE_SUDO_LOG"
case "$1" in
    mkdir)
        [ -n "${FAKE_SUDO_UNLOCK:-}" ] && chmod u+w "$FAKE_SUDO_UNLOCK"
        shift; mkdir "$@" ;;
    chown) exit 0 ;;
    *) exit 1 ;;
esac
TOOL
chmod +x "$_fake/curl" "$_fake/uname" "$_fake/sudo"

# --- canned releases ---------------------------------------------------------
_real_den="${DEN_TEST_BIN:-}"
if [ -z "$_real_den" ] && [ -x "$ROOT/build/den" ]; then
    _real_den="$ROOT/build/den"
fi
if [ -n "$_real_den" ]; then
    _real_den=$(CDPATH= cd -- "$(dirname "$_real_den")" && pwd -P)/$(basename "$_real_den")
    "$_real_den" get --help 2>/dev/null | grep -q 'den get' || _real_den=""
fi

# mkrel TAG ASSET_VERSION OS ARCH [real]
# The default fake den is "old": it has `--version` and `set` (logged, and
# writing a minimal config.json) but no `get`. Like the real CLI, it answers
# --help anywhere (so `den get --help`) with its top-level help and exit 0. With "real", everything except
# --version runs the real den binary.
mkrel() {
    _d="$_tmp/rel/$1"
    _s="$_tmp/stage/$1-$3-$4"
    mkdir -p "$_d" "$_s/share/den"
    if [ "${5:-}" = real ]; then
        cat >"$_s/den" <<DEN
#!/bin/sh
case "\$1" in
    --version) echo "$2" ;;
    *) exec "$_real_den" "\$@" ;;
esac
DEN
    else
        cat >"$_s/den" <<DEN
#!/bin/sh
[ -n "\${FAKE_DEN_LOG:-}" ] && echo "\$*" >>"\$FAKE_DEN_LOG"
for a in "\$@"; do
    [ "\$a" = --help ] && { echo "Usage: den [OPTIONS] [SUBCOMMAND]"; exit 0; }
done
case "\$1" in
    --version) echo "$2" ;;
    set) mkdir -p "\$DEN_HOME" && printf '{"store":{"prefix":"%s"}}\\n' "\$3" >"\$DEN_HOME/config.json" ;;
    *) echo "unknown command: \$1" >&2; exit 109 ;;
esac
DEN
    fi
    chmod +x "$_s/den"
    echo "{\"tag\":\"$1\"}" >"$_s/share/den/known_hashes.json"
    _a="den-$2-$3-$4.tar.gz"
    (cd "$_s" && tar czf "$_d/$_a" den share)
    printf '%s  %s\n' "$(_sha "$_d/$_a")" "$_a" >>"$_d/den-$2-checksums.txt"
}
mkrel v0.14.0 0.14.0 linux x86_64
mkrel v0.14.0-rc.3 0.14.0 linux x86_64
mkrel v0.15.0-rc.1 0.15.0 linux x86_64
mkrel v0.15.0-rc.1 0.15.0 darwin aarch64
mkrel v0.13.0 0.13.0 linux x86_64
[ -n "$_real_den" ] && mkrel v0.16.0 0.16.0 linux x86_64 real
# A release whose checksum does not match its tarball.
mkrel v0.12.0 0.12.0 linux x86_64
printf '%s  %s\n' 0000000000000000000000000000000000000000000000000000000000000000 \
    den-0.12.0-linux-x86_64.tar.gz >"$_tmp/rel/v0.12.0/den-0.12.0-checksums.txt"

# --- harness -----------------------------------------------------------------
_n=0
# newhome: a fresh scratch HOME in $H.
newhome() {
    _n=$((_n + 1))
    H="$_tmp/home$_n"
    mkdir -p "$H"
}

# inst [VAR=VAL ...]: run install.sh in $H, output in $_tmp/out, exit in $_rc.
inst() {
    : >"$_tmp/curl.log"
    : >"$_tmp/sudo.log"
    : >"$_tmp/den.log"
    env -i PATH="$_fake:$_tools:${INST_SHA:-$_shabin}" HOME="$H" SHELL=/bin/sh \
        DEN_NO_MODIFY_PATH=1 DEN_INSTALL_INTERACTIVE=0 \
        FAKE_CURL_LOG="$_tmp/curl.log" FAKE_SUDO_LOG="$_tmp/sudo.log" FAKE_DEN_LOG="$_tmp/den.log" \
        FAKE_REL="$_tmp/rel" FAKE_LATEST=v0.14.0 FAKE_OS=Linux FAKE_ARCH=x86_64 \
        "$@" sh "$SCRIPT" </dev/null >"$_tmp/out" 2>&1
    _rc=$?
}

ok() {
    _pass=$((_pass + 1))
    echo "PASS: $1"
}
bad() {
    _fail=$((_fail + 1))
    echo "FAIL: $1"
    sed 's/^/    out: /' "$_tmp/out"
    sed 's/^/    curl: /' "$_tmp/curl.log"
    sed 's/^/    sudo: /' "$_tmp/sudo.log"
}
# check NAME COND...: COND is evaluated by the shell.
check() {
    _name="$1"
    shift
    if eval "$*"; then ok "$_name"; else bad "$_name ($*)"; fi
}
out_has() { grep -qF -- "$1" "$_tmp/out"; }
curl_has() { grep -qF -- "$1" "$_tmp/curl.log"; }

# --- GA, with and without a leading v ----------------------------------------
for v in v0.14.0 0.14.0; do
    newhome
    inst DEN_VERSION="$v"
    check "GA $v: exit 0" '[ "$_rc" -eq 0 ]'
    check "GA $v: tag URL" 'curl_has "/releases/download/v0.14.0/den-0.14.0-linux-x86_64.tar.gz"'
    check "GA $v: checksums URL" 'curl_has "/releases/download/v0.14.0/den-0.14.0-checksums.txt"'
    check "GA $v: binary installed" '[ "$("$H/.den/bin/den" --version)" = 0.14.0 ]'
    check "GA $v: replica installed" 'grep -q v0.14.0 "$H/.den/share/den/known_hashes.json"'
    check "GA $v: tag recorded" '[ "$(cat "$H/.den/share/den/installed-tag")" = v0.14.0 ]'
    check "GA $v: default prefix created" '[ -d "$H/.den/prefix" ]'
    check "GA $v: no sudo" '[ ! -s "$_tmp/sudo.log" ]'
done

# --- RC, with and without a leading v ----------------------------------------
for v in 0.15.0-rc.1 v0.15.0-rc.1; do
    newhome
    inst DEN_VERSION="$v"
    check "RC $v: exit 0" '[ "$_rc" -eq 0 ]'
    check "RC $v: RC tag, GA-named asset" \
        'curl_has "/releases/download/v0.15.0-rc.1/den-0.15.0-linux-x86_64.tar.gz"'
    check "RC $v: GA-named checksums" 'curl_has "/releases/download/v0.15.0-rc.1/den-0.15.0-checksums.txt"'
    check "RC $v: tag recorded" '[ "$(cat "$H/.den/share/den/installed-tag")" = v0.15.0-rc.1 ]'
done

newhome
inst DEN_VERSION=0.15.0-rc.1 FAKE_OS=Darwin FAKE_ARCH=arm64
check "RC on darwin/arm64: aarch64 asset" \
    'curl_has "/releases/download/v0.15.0-rc.1/den-0.15.0-darwin-aarch64.tar.gz"'
check "RC on darwin/arm64: exit 0" '[ "$_rc" -eq 0 ]'

# --- default: latest GA ------------------------------------------------------
newhome
inst
check "latest: resolves via releases/latest" 'curl_has "/releases/latest"'
check "latest: installs v0.14.0" 'curl_has "/releases/download/v0.14.0/den-0.14.0-linux-x86_64.tar.gz"'

# --- invalid versions --------------------------------------------------------
for v in 0.14 latest 0.14.0-beta.1 1.2.3.4 0.14.0-rc. 0.14.0-rc.x vv0.14.0 '0.14.0;x' 0..14.0; do
    newhome
    inst DEN_VERSION="$v"
    check "invalid $v: rejected" '[ "$_rc" -ne 0 ] && out_has "invalid version format"'
    check "invalid $v: nothing downloaded" '[ ! -s "$_tmp/curl.log" ]'
done

# --- already-installed check (den --version prints the bare version) ---------
newhome
inst DEN_VERSION=0.14.0
inst DEN_VERSION=v0.14.0
check "installed: same GA is a no-op" '[ "$_rc" -eq 0 ] && out_has "den v0.14.0 is already installed."'
check "installed: no download" '[ ! -s "$_tmp/curl.log" ]'

inst DEN_VERSION=0.14.0-rc.3
check "installed: RC of the same base reinstalls" \
    '[ "$_rc" -eq 0 ] && curl_has "/releases/download/v0.14.0-rc.3/"'
inst DEN_VERSION=0.14.0-rc.3
check "installed: same RC is a no-op" '[ "$_rc" -eq 0 ] && [ ! -s "$_tmp/curl.log" ]'
inst DEN_VERSION=0.14.0
check "installed: GA over its RC reinstalls" \
    '[ "$_rc" -eq 0 ] && curl_has "/releases/download/v0.14.0/"'
inst DEN_VERSION=0.13.0
check "installed: different version replaces" \
    '[ "$_rc" -eq 0 ] && out_has "replacing den v0.14.0 -> v0.13.0" && [ "$("$H/.den/bin/den" --version)" = 0.13.0 ]'

# An install from before the tag was recorded.
rm -f "$H/.den/share/den/installed-tag"
inst DEN_VERSION=0.13.0
check "installed: legacy GA install is a no-op" '[ "$_rc" -eq 0 ] && [ ! -s "$_tmp/curl.log" ]'

# --- missing sha256 tool is a hard failure -----------------------------------
newhome
INST_SHA="$_tmp/empty"
inst DEN_VERSION=0.14.0
unset INST_SHA
check "no sha tool: fails" '[ "$_rc" -ne 0 ] && out_has "need '"'"'shasum'"'"' or '"'"'sha256sum'"'"'"'
check "no sha tool: nothing downloaded" '[ ! -s "$_tmp/curl.log" ]'
check "no sha tool: nothing installed" '[ ! -e "$H/.den/bin/den" ]'

# --- checksum mismatch -------------------------------------------------------
newhome
inst DEN_VERSION=0.12.0
check "bad checksum: fails" '[ "$_rc" -ne 0 ] && out_has "checksum mismatch"'
check "bad checksum: nothing installed" '[ ! -e "$H/.den/bin/den" ]'

check "bad checksum: archive fetched once (never retried)" \
    '[ "$(grep -c "/den-0.12.0-linux-x86_64.tar.gz" "$_tmp/curl.log")" -eq 1 ]'

# --- transient download errors -----------------------------------------------
_asset_url="/releases/download/v0.14.0/den-0.14.0-linux-x86_64.tar.gz"
# fails N times with curl CODE (and HTTP status) on the archive, then serves it.
flaky() { # flaky TIMES CODE [HTTP] [VAR=VAL...]
    _t=$1 _c=$2 _h=${3:-000}
    shift 2; [ $# -gt 0 ] && shift
    rm -f "$_tmp/fail.state"
    inst DEN_VERSION=0.14.0 DEN_DOWNLOAD_RETRY_DELAY=0 FAKE_CURL_FAIL_CODE="$_c" \
        FAKE_CURL_FAIL_TIMES="$_t" FAKE_CURL_FAIL_HTTP="$_h" \
        FAKE_CURL_FAIL_MATCH="$_asset_url" FAKE_CURL_FAIL_STATE="$_tmp/fail.state" "$@"
}
fetches() { grep -c -- "$_asset_url" "$_tmp/curl.log"; }

newhome
flaky 1 56
check "curl 56 once: retried, install succeeds" '[ "$_rc" -eq 0 ] && [ "$(fetches)" -eq 2 ]'
check "curl 56 once: says it is retrying" 'out_has "retrying (1 of 2)"'
check "curl 56 once: binary installed" '[ "$("$H/.den/bin/den" --version)" = 0.14.0 ]'

newhome
flaky 2 35
check "TLS error twice: third try succeeds" '[ "$_rc" -eq 0 ] && [ "$(fetches)" -eq 3 ]'

newhome
flaky 9 56
check "curl 56 always: gives up after 3 tries" '[ "$_rc" -ne 0 ] && [ "$(fetches)" -eq 3 ]'
check "curl 56 always: reports the failure" 'out_has "download failed"'
check "curl 56 always: nothing installed" '[ ! -e "$H/.den/bin/den" ]'

newhome
flaky 9 56 000 DEN_DOWNLOAD_ATTEMPTS=1
check "DEN_DOWNLOAD_ATTEMPTS=1: no retry" '[ "$_rc" -ne 0 ] && [ "$(fetches)" -eq 1 ]'

newhome
flaky 1 22 503
check "HTTP 503: retried" '[ "$_rc" -eq 0 ] && [ "$(fetches)" -eq 2 ]'

newhome
flaky 1 22 404
check "HTTP 404: not retried" '[ "$_rc" -ne 0 ] && [ "$(fetches)" -eq 1 ]'

# --- DEN_PREFIX / DEN_HOME, writable ------------------------------------------
newhome
inst DEN_VERSION=0.14.0 DEN_PREFIX="$H/custom/prefix"
check "DEN_PREFIX writable: created without sudo" \
    '[ "$_rc" -eq 0 ] && [ -d "$H/custom/prefix" ] && [ ! -s "$_tmp/sudo.log" ]'
check "DEN_PREFIX, old den, no config: den set records it" \
    'grep -qxF "set store.prefix $H/custom/prefix" "$_tmp/den.log"'
check "DEN_PREFIX, old den, no config: config written" \
    'grep -qF "\"prefix\":\"$H/custom/prefix\"" "$H/.den/config.json"'
check "DEN_PREFIX, old den, no config: no export hint" '! out_has "export DEN_PREFIX="'

newhome
mkdir -p "$H/.den"
printf '{"daemon":{"auto_upgrade":true}}\n' >"$H/.den/config.json"
cp "$H/.den/config.json" "$_tmp/cfg.before"
inst DEN_VERSION=0.14.0 DEN_PREFIX="$H/custom/prefix"
check "DEN_PREFIX, old den, existing config: install succeeds" '[ "$_rc" -eq 0 ]'
check "DEN_PREFIX, old den, existing config: not touched" 'cmp -s "$_tmp/cfg.before" "$H/.den/config.json"'
check "DEN_PREFIX, old den, existing config: den set not run" '! grep -q "^set " "$_tmp/den.log"'
check "DEN_PREFIX, old den, existing config: prints the command" \
    'out_has "\"$H/.den/bin/den\" set store.prefix \"$H/custom/prefix\""'
check "DEN_PREFIX, old den, existing config: tells the user to keep it exported" \
    'out_has "export DEN_PREFIX=\"$H/custom/prefix\""'

for _dp in unset default; do
    newhome
    if [ "$_dp" = unset ]; then
        inst DEN_VERSION=0.14.0
    else
        inst DEN_VERSION=0.14.0 DEN_PREFIX="$H/.den/prefix"
    fi
    check "DEN_PREFIX $_dp, old den: no config written" '[ "$_rc" -eq 0 ] && [ ! -e "$H/.den/config.json" ]'
    check "DEN_PREFIX $_dp, old den: den set not run" '! grep -q "^set " "$_tmp/den.log"'
done
newhome
inst DEN_VERSION=0.14.0 DEN_HOME="$H/dh"
check "DEN_HOME: binary and prefix follow it" \
    '[ "$_rc" -eq 0 ] && [ -x "$H/dh/bin/den" ] && [ -d "$H/dh/prefix" ]'

# --- DEN_PREFIX persisted by the real den (config.json merge) -----------------
if [ -z "$_real_den" ]; then
    _skip=$((_skip + 1))
    echo "SKIP: store.prefix persistence with the real den (no DEN_TEST_BIN or build/den)"
else
    # cfg_prefix: the store.prefix den itself reads back.
    cfg_prefix() { env -i HOME="$H" DEN_HOME="$H/.den" "$_real_den" get store.prefix 2>/dev/null; }
    cfg_mode() { ls -l "$H/.den/config.json" | cut -c1-10; }
    newhome
    P="$H/custom/prefix"
    inst DEN_VERSION=0.16.0 DEN_PREFIX="$P"
    check "persist, no config: exit 0" '[ "$_rc" -eq 0 ]'
    check "persist, no config: store.prefix recorded" '[ "$(cfg_prefix)" = "$P" ]'
    check "persist, no config: written 0600" '[ "$(cfg_mode)" = "-rw-------" ]'
    check "persist, no config: reported" 'out_has "recorded store.prefix = $P"'
    check "persist, no config: no export hint" '! out_has "export DEN_PREFIX="'

    # Re-running with the same prefix (already installed) is a no-op.
    cp "$H/.den/config.json" "$_tmp/cfg.before"
    inst DEN_VERSION=0.16.0 DEN_PREFIX="$P"
    check "persist, same prefix: exit 0" '[ "$_rc" -eq 0 ] && out_has "already installed"'
    check "persist, same prefix: says so" 'out_has "store.prefix is already $P"'
    check "persist, same prefix: file unchanged" 'cmp -s "$_tmp/cfg.before" "$H/.den/config.json"'
    check "persist, same prefix: no export hint" '! out_has "export DEN_PREFIX="'

    newhome
    P="$H/custom/prefix"
    mkdir -p "$H/.den"
    printf '%s\n' '{"daemon":{"auto_upgrade":true,"interval_secs":60},"taps":{"a/b":"https://example.invalid/b"},"casks":{"c":"1"},"future":{"x":[1,2]}}' \
        >"$H/.den/config.json"
    inst DEN_VERSION=0.16.0 DEN_PREFIX="$P"
    _k() { env -i HOME="$H" DEN_HOME="$H/.den" "$_real_den" get "$1" 2>/dev/null; }
    check "persist, existing config: exit 0" '[ "$_rc" -eq 0 ]'
    check "persist, existing config: store.prefix added" '[ "$(cfg_prefix)" = "$P" ]'
    check "persist, existing config: other keys kept" \
        '[ "$(_k daemon.auto_upgrade)" = true ] && [ "$(_k daemon.interval_secs)" = 60 ] &&
         [ "$(_k taps.a/b)" = https://example.invalid/b ] && [ "$(_k casks.c)" = 1 ] &&
         [ "$(_k future.x)" = "[1,2]" ]'

    newhome
    P="$H/custom/prefix"
    mkdir -p "$H/.den"
    printf '%s\n' '{"store":{"prefix":"/elsewhere"},"daemon":{"auto_upgrade":true}}' >"$H/.den/config.json"
    cp "$H/.den/config.json" "$_tmp/cfg.before"
    inst DEN_VERSION=0.16.0 DEN_PREFIX="$P"
    check "persist, different prefix: exit 0" '[ "$_rc" -eq 0 ]'
    check "persist, different prefix: not overwritten" 'cmp -s "$_tmp/cfg.before" "$H/.den/config.json"'
    check "persist, different prefix: explains" \
        'out_has "already sets store.prefix to /elsewhere; leaving it." && out_has "DEN_PREFIX_OVERWRITE=1"'
    check "persist, different prefix: export hint" 'out_has "export DEN_PREFIX=\"$P\""'

    inst DEN_VERSION=0.16.0 DEN_PREFIX="$P" DEN_PREFIX_OVERWRITE=1
    check "persist, DEN_PREFIX_OVERWRITE=1: replaced" \
        '[ "$_rc" -eq 0 ] && [ "$(cfg_prefix)" = "$P" ] && out_has "replacing store.prefix /elsewhere -> $P"'
    check "persist, DEN_PREFIX_OVERWRITE=1: other keys kept" \
        '[ "$(env -i HOME="$H" DEN_HOME="$H/.den" "$_real_den" get daemon.auto_upgrade)" = true ]'

    newhome
    P="$H/custom/prefix"
    mkdir -p "$H/.den"
    printf '{"daemon": {\n' >"$H/.den/config.json"
    cp "$H/.den/config.json" "$_tmp/cfg.before"
    inst DEN_VERSION=0.16.0 DEN_PREFIX="$P"
    check "persist, invalid config: install still succeeds" '[ "$_rc" -eq 0 ]'
    check "persist, invalid config: left untouched" 'cmp -s "$_tmp/cfg.before" "$H/.den/config.json"'
    check "persist, invalid config: explains" 'out_has "could not read $H/.den/config.json"'

    for _dp in unset default; do
        newhome
        if [ "$_dp" = unset ]; then
            inst DEN_VERSION=0.16.0
        else
            inst DEN_VERSION=0.16.0 DEN_PREFIX="$H/.den/prefix"
        fi
        check "persist, DEN_PREFIX $_dp: no config written" '[ "$_rc" -eq 0 ] && [ ! -e "$H/.den/config.json" ]'
    done
fi

# --- missing prefix that needs privileges ------------------------------------
if [ "$(id -u)" -eq 0 ]; then
    _skip=$((_skip + 1))
    echo "SKIP: unwritable-prefix cases (running as root)"
else
    # $L stands in for a root-owned / (so $L/opt/homebrew cannot be created).
    newlocked() {
        newhome
        L="$H/locked"
        mkdir -p "$L"
        chmod 555 "$L"
        P="$L/opt/homebrew"
    }

    newlocked
    inst DEN_VERSION=0.14.0 DEN_PREFIX="$P"
    check "prefix, non-interactive: fails" '[ "$_rc" -ne 0 ]'
    check "prefix, non-interactive: prints the command" \
        'out_has "sudo mkdir -p $P && sudo chown $(id -un):$(id -gn) $P"'
    check "prefix, non-interactive: no sudo" '[ ! -s "$_tmp/sudo.log" ]'
    check "prefix, non-interactive: nothing downloaded" '[ ! -s "$_tmp/curl.log" ]'

    newlocked
    inst DEN_VERSION=0.14.0 DEN_PREFIX="$P" FAKE_OS=Darwin FAKE_ARCH=arm64
    check "prefix, darwin: chown to the admin group" 'out_has "sudo chown $(id -un):admin $P"'

    newlocked
    printf 'n\n' >"$_tmp/tty"
    inst DEN_VERSION=0.14.0 DEN_PREFIX="$P" DEN_INSTALL_INTERACTIVE=1 DEN_INSTALL_TTY="$_tmp/tty"
    check "prefix, interactive decline: fails" '[ "$_rc" -ne 0 ] && out_has "not creating $P"'
    check "prefix, interactive decline: prompted" 'out_has "run it now with sudo?"'
    check "prefix, interactive decline: no sudo" '[ ! -s "$_tmp/sudo.log" ]'
    check "prefix, interactive decline: nothing downloaded" '[ ! -s "$_tmp/curl.log" ]'

    newlocked
    : >"$_tmp/tty"
    inst DEN_VERSION=0.14.0 DEN_PREFIX="$P" DEN_INSTALL_INTERACTIVE=1 DEN_INSTALL_TTY="$_tmp/tty"
    check "prefix, interactive EOF: treated as decline" '[ "$_rc" -ne 0 ] && [ ! -s "$_tmp/sudo.log" ]'

    newlocked
    printf 'y\n' >"$_tmp/tty"
    inst DEN_VERSION=0.14.0 DEN_PREFIX="$P" DEN_INSTALL_INTERACTIVE=1 DEN_INSTALL_TTY="$_tmp/tty" \
        FAKE_SUDO_UNLOCK="$L"
    check "prefix, interactive accept: installs" '[ "$_rc" -eq 0 ] && [ -d "$P" ] && [ -x "$H/.den/bin/den" ]'
    check "prefix, interactive accept: sudo mkdir then chown" \
        '[ "$(cat "$_tmp/sudo.log")" = "mkdir -p $P
chown $(id -un):$(id -gn) $P" ]'

    # Already installed, but the prefix has since gone: still enforced.
    chmod u+w "$L"
    rm -rf "$L/opt"
    chmod 555 "$L"
    inst DEN_VERSION=0.14.0 DEN_PREFIX="$P"
    check "prefix, already installed: still required" \
        '[ "$_rc" -ne 0 ] && out_has "sudo mkdir -p $P"'
fi


# --- shell integration is idempotent ------------------------------------------
# init_lines FILE: lines that run den's init (the installer's own quoted-path
# form included).
init_lines() { grep -Ec '^[^#]*den["'"'"']?[[:space:]]+init' "$1" 2>/dev/null || true; }

newhome
inst DEN_VERSION=0.13.0 SHELL=/bin/zsh DEN_NO_MODIFY_PATH=0
inst DEN_VERSION=0.14.0 SHELL=/bin/zsh DEN_NO_MODIFY_PATH=0
check "shell integration: re-install adds no second init line" \
    '[ "$_rc" -eq 0 ] && [ "$(init_lines "$H/.zshrc")" -eq 1 ] && out_has "shell integration already in"'

newhome
printf '# den shell integration (managed)\neval "$("$HOME/.den/bin/den" init --shell zsh)"\n' >"$H/.zshrc"
inst DEN_VERSION=0.14.0 SHELL=/bin/zsh DEN_NO_MODIFY_PATH=0
check "shell integration: existing --shell zsh line recognised" \
    '[ "$_rc" -eq 0 ] && [ "$(init_lines "$H/.zshrc")" -eq 1 ]'

newhome
printf '# TODO: add den init here\n' >"$H/.zshrc"
inst DEN_VERSION=0.14.0 SHELL=/bin/zsh DEN_NO_MODIFY_PATH=0
check "shell integration: a comment mentioning den init is not integration" \
    '[ "$_rc" -eq 0 ] && [ "$(init_lines "$H/.zshrc")" -eq 1 ] && out_has "added shell integration"'

echo ""
echo "Results: ${_pass} passed, ${_fail} failed, ${_skip} skipped"
[ "$_fail" -eq 0 ]

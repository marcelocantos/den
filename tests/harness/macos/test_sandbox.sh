#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Regression tests for tests/harness/macos/sandbox.sh.
#
# Proves that `sandbox.sh run` points toolchain caches, TMPDIR and Go
# telemetry inside the sandbox, and that `sandbox.sh cleanup` removes
# read-only trees, refuses paths outside the sandbox root, and fails loudly
# (non-zero, message on stderr) instead of claiming success when something
# is left behind.
#
# Run:
#   sh tests/harness/macos/test_sandbox.sh
# or via ctest (name: harness_macos_sandbox).

set -u

_SELF_DIR=$(CDPATH='' cd -- "$(dirname "$0")" && pwd)
SANDBOX_SH="${_SELF_DIR}/sandbox.sh"
RUN_SH="${_SELF_DIR}/run.sh"

_pass=0
_fail=0
pass() { echo "PASS: $1"; _pass=$((_pass + 1)); }
fail() { echo "FAIL: $1${2:+ ($2)}"; _fail=$((_fail + 1)); }

WORK=$(mktemp -d)
WORK=$(CDPATH='' cd -- "$WORK" && pwd -P)
cleanup_work() { chmod -R u+w "$WORK" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup_work EXIT INT TERM

ROOT="$WORK/den-harness"
mkdir -p "$ROOT"
export HARNESS_SANDBOX_ROOT="$ROOT"

# Fake go that reports what the real go would see.
FAKEBIN="$WORK/fakebin"
mkdir -p "$FAKEBIN"
cat > "$FAKEBIN/go" <<'GO'
#!/bin/sh
printf 'HOME=%s\n' "$HOME"
printf 'GOENV=%s\n' "${GOENV:-}"
printf 'ARGS=%s\n' "$*"
GO
chmod +x "$FAKEBIN/go"

# --- run: environment redirection -------------------------------------------

SB="$ROOT/run1"
ENV_OUT=$(PATH="$FAKEBIN:$PATH" HOME="$WORK/realhome" GOENV='' \
    sh "$SANDBOX_SH" run "$SB" -- env 2>&1)
rc=$?
if [ "$rc" -eq 0 ]; then pass "run exits 0"; else fail "run exits 0" "rc=$rc: $ENV_OUT"; fi

E="$SB/.harness-env"
for var in TMPDIR NODE_COMPILE_CACHE npm_config_cache UV_CACHE_DIR \
        PIP_CACHE_DIR XDG_CACHE_HOME XDG_DATA_HOME XDG_STATE_HOME GOCACHE \
        GOMODCACHE GOPATH CARGO_HOME; do
    val=$(printf '%s\n' "$ENV_OUT" | sed -n "s/^${var}=//p" | head -n 1)
    case "$val" in
        "$E"/?*)
            if [ -d "$val" ]; then pass "$var inside sandbox"; else fail "$var inside sandbox" "$val not created"; fi
            ;;
        *) fail "$var inside sandbox" "got '$val'" ;;
    esac
done

if printf '%s\n' "$ENV_OUT" | grep -qx 'npm_config_update_notifier=false'; then
    pass "npm update notifier disabled"
else
    fail "npm update notifier disabled"
fi

if printf '%s\n' "$ENV_OUT" | grep -qx "RUSTUP_HOME=$WORK/realhome/.rustup"; then
    pass "RUSTUP_HOME pinned to the real rustup home"
else
    fail "RUSTUP_HOME pinned to the real rustup home" \
        "$(printf '%s\n' "$ENV_OUT" | grep '^RUSTUP_HOME=')"
fi

if printf '%s\n' "$ENV_OUT" | grep -qx "HOME=$WORK/realhome"; then
    pass "HOME itself is not changed for the smoke run"
else
    fail "HOME itself is not changed for the smoke run"
fi

# --- run: go shim keeps telemetry in the sandbox ----------------------------

# XDG_CONFIG_HOME is cleared so the default (non-macOS) GOENV derives from HOME;
# CI runners export their own XDG_CONFIG_HOME.
GO_OUT=$(PATH="$FAKEBIN:$PATH" HOME="$WORK/realhome" GOENV='' XDG_CONFIG_HOME='' \
    sh "$SANDBOX_SH" run "$SB" -- go version -m x 2>&1)
if printf '%s\n' "$GO_OUT" | grep -qx "HOME=$E/home"; then
    pass "go runs with HOME inside the sandbox"
else
    fail "go runs with HOME inside the sandbox" "$GO_OUT"
fi
case "$(uname -s)" in
    Darwin) WANT_GOENV="$WORK/realhome/Library/Application Support/go/env" ;;
    *) WANT_GOENV="$WORK/realhome/.config/go/env" ;;
esac
if [ "$(uname -s)" != Darwin ]; then
    XDG_OUT=$(PATH="$FAKEBIN:$PATH" HOME="$WORK/realhome" GOENV='' \
        XDG_CONFIG_HOME="$WORK/xdgconf" sh "$SANDBOX_SH" run "$SB" -- go 2>&1)
    if printf '%s\n' "$XDG_OUT" | grep -qxF "GOENV=$WORK/xdgconf/go/env"; then
        pass "go honours the host's XDG_CONFIG_HOME for GOENV"
    else
        fail "go honours the host's XDG_CONFIG_HOME for GOENV" "$XDG_OUT"
    fi
fi
if printf '%s\n' "$GO_OUT" | grep -qxF "GOENV=$WANT_GOENV"; then
    pass "go keeps the host's GOENV file"
else
    fail "go keeps the host's GOENV file" "$GO_OUT"
fi
if printf '%s\n' "$GO_OUT" | grep -qx 'ARGS=version -m x'; then
    pass "go shim passes arguments through"
else
    fail "go shim passes arguments through" "$GO_OUT"
fi
GOENV_OUT=$(PATH="$FAKEBIN:$PATH" HOME="$WORK/realhome" GOENV="$WORK/my go env" \
    sh "$SANDBOX_SH" run "$SB" -- go 2>&1)
if printf '%s\n' "$GOENV_OUT" | grep -qxF "GOENV=$WORK/my go env"; then
    pass "an explicit GOENV is preserved"
else
    fail "an explicit GOENV is preserved" "$GOENV_OUT"
fi
if [ ! -e "$WORK/realhome" ]; then
    pass "nothing written under the real HOME"
else
    fail "nothing written under the real HOME" "$(find "$WORK/realhome" | head -n 5)"
fi

# --- run: exit status and argument checks -----------------------------------

sh "$SANDBOX_SH" run "$SB" -- sh -c 'exit 7' >/dev/null 2>&1
rc=$?
if [ "$rc" -eq 7 ]; then pass "run propagates the command's exit status"; else fail "run propagates the command's exit status" "rc=$rc"; fi

if sh "$SANDBOX_SH" run "$SB" env >/dev/null 2>&1; then
    fail "run requires --"
else
    pass "run requires --"
fi

if sh "$SANDBOX_SH" run "$WORK/elsewhere" -- true >/dev/null 2>&1; then
    fail "run refuses a path outside the sandbox root"
else
    pass "run refuses a path outside the sandbox root"
fi

# --- cleanup ----------------------------------------------------------------

# A read-only tree like Go's module cache: plain rm -rf fails on it.
SB2="$ROOT/ro"
mkdir -p "$SB2/cache/go/modcache/example.com/m@v1.0.0/sub"
echo x > "$SB2/cache/go/modcache/example.com/m@v1.0.0/sub/f.go"
chmod -R a-w "$SB2/cache/go/modcache"
if sh "$SANDBOX_SH" cleanup "$SB2" >/dev/null 2>&1 && [ ! -e "$SB2" ]; then
    pass "cleanup removes a read-only tree"
else
    fail "cleanup removes a read-only tree"
fi

if sh "$SANDBOX_SH" cleanup "$ROOT/does-not-exist" >/dev/null 2>&1; then
    pass "cleanup of a missing sandbox succeeds"
else
    fail "cleanup of a missing sandbox succeeds"
fi

# rm that "succeeds" but leaves the directory: must not report success.
LIARBIN="$WORK/liarbin"
mkdir -p "$LIARBIN"
printf '#!/bin/sh\nexit 0\n' > "$LIARBIN/rm"
chmod +x "$LIARBIN/rm"
SB3="$ROOT/stuck"
mkdir -p "$SB3/x"
ERR=$(PATH="$LIARBIN:$PATH" sh "$SANDBOX_SH" cleanup "$SB3" 2>&1 >/dev/null)
rc=$?
if [ "$rc" -ne 0 ] && printf '%s' "$ERR" | grep -q 'failed to remove'; then
    pass "cleanup fails loudly when the sandbox survives rm"
else
    fail "cleanup fails loudly when the sandbox survives rm" "rc=$rc err=$ERR"
fi

# rm that fails and leaves the directory.
FAILBIN="$WORK/failbin"
mkdir -p "$FAILBIN"
printf '#!/bin/sh\nexit 1\n' > "$FAILBIN/rm"
chmod +x "$FAILBIN/rm"
ERR=$(PATH="$FAILBIN:$PATH" sh "$SANDBOX_SH" cleanup "$SB3" 2>&1 >/dev/null)
rc=$?
if [ "$rc" -ne 0 ] && printf '%s' "$ERR" | grep -q 'rm exit 1'; then
    pass "cleanup reports rm's failure"
else
    fail "cleanup reports rm's failure" "rc=$rc err=$ERR"
fi

# Path guards: nothing outside the root, no traversal, no odd characters.
mkdir -p "$WORK/victim"
for bad in "" "$WORK/victim" "$ROOT/../victim" "$ROOT/a b" "$ROOT/a;b" "$ROOT-other" /; do
    if sh "$SANDBOX_SH" cleanup "$bad" >/dev/null 2>&1; then
        fail "cleanup refuses '$bad'"
    else
        pass "cleanup refuses '$bad'"
    fi
done
if [ -d "$WORK/victim" ]; then pass "refused paths are untouched"; else fail "refused paths are untouched"; fi

# --reset cleans the root itself.
mkdir -p "$ROOT/r1/a" "$ROOT/r2"
chmod a-w "$ROOT/r1/a"
if sh "$SANDBOX_SH" cleanup "$ROOT" >/dev/null 2>&1 && [ ! -e "$ROOT" ]; then
    pass "cleanup of the sandbox root (reset) succeeds"
else
    fail "cleanup of the sandbox root (reset) succeeds"
fi

# --- run.sh wiring ----------------------------------------------------------

if grep -n 'rm -rf' "$RUN_SH" >/dev/null; then
    fail "run.sh has no raw remote rm -rf" "$(grep -n 'rm -rf' "$RUN_SH")"
else
    pass "run.sh has no raw remote rm -rf"
fi
if grep -q "sandbox.sh' run '\${DEN_HOME_REMOTE}' --" "$RUN_SH"; then
    pass "run.sh runs smoke.sh through sandbox.sh"
else
    fail "run.sh runs smoke.sh through sandbox.sh"
fi

echo
echo "${_pass} passed, ${_fail} failed"
[ "$_fail" -eq 0 ]

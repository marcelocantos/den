#!/usr/bin/env bash
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# SSH-based macOS test harness for den release-candidate validation.
#
# Usage:
#   run.sh --binary PATH  [--host ALIAS] [--keep-sandbox] [--reset] [--test-pkg NAME]
#   run.sh --release VER  [--host ALIAS] [--keep-sandbox] [--reset] [--test-pkg NAME]
#
# --binary PATH    scp a pre-built local den binary to the test host. RC mode.
# --release VER    run install.sh on the test host with DEN_VERSION=VER.
# --host ALIAS     SSH alias from ~/.ssh/config (default: den-test-mac).
# --keep-sandbox   preserve the remote sandbox dir after the run.
#                  Default: keep on failure, remove on success.
# --reset          wipe any existing /tmp/den-harness/ dirs on the remote and exit.
# --test-pkg NAME  package to exercise via smoke.sh (default: jq).
set -euo pipefail

# ---------------------------------------------------------------------------
# Sanity check: must be run from the repo root.
# ---------------------------------------------------------------------------
if [[ ! -f CMakeLists.txt || ! -d src ]]; then
    printf 'error: run.sh must be invoked from the repository root\n' >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
BINARY_PATH=""
RELEASE_VER=""
SSH_HOST="den-test-mac"
KEEP_SANDBOX=0
RESET=0
TEST_PKG="jq"

usage() {
    printf 'Usage:\n'
    printf '  %s --binary PATH  [--host ALIAS] [--keep-sandbox] [--reset] [--test-pkg NAME]\n' "$0"
    printf '  %s --release VER  [--host ALIAS] [--keep-sandbox] [--reset] [--test-pkg NAME]\n' "$0"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary)
            [[ $# -ge 2 ]] || { printf 'error: --binary requires an argument\n' >&2; usage; }
            BINARY_PATH="$2"
            shift 2
            ;;
        --release)
            [[ $# -ge 2 ]] || { printf 'error: --release requires an argument\n' >&2; usage; }
            RELEASE_VER="$2"
            shift 2
            ;;
        --host)
            [[ $# -ge 2 ]] || { printf 'error: --host requires an argument\n' >&2; usage; }
            SSH_HOST="$2"
            shift 2
            ;;
        --keep-sandbox)
            KEEP_SANDBOX=1
            shift
            ;;
        --reset)
            RESET=1
            shift
            ;;
        --test-pkg)
            [[ $# -ge 2 ]] || { printf 'error: --test-pkg requires an argument\n' >&2; usage; }
            TEST_PKG="$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            printf 'error: unknown argument: %s\n' "$1" >&2
            usage
            ;;
    esac
done

# If --reset, we don't need --binary or --release.
if [[ $RESET -eq 0 ]]; then
    if [[ -z "$BINARY_PATH" && -z "$RELEASE_VER" ]]; then
        printf 'error: one of --binary or --release is required\n' >&2
        usage
    fi
    if [[ -n "$BINARY_PATH" && -n "$RELEASE_VER" ]]; then
        printf 'error: --binary and --release are mutually exclusive\n' >&2
        usage
    fi
fi

# ---------------------------------------------------------------------------
# Resolve and validate inputs
# ---------------------------------------------------------------------------
if [[ -n "$BINARY_PATH" ]]; then
    BINARY_PATH="$(cd "$(dirname "$BINARY_PATH")" && pwd)/$(basename "$BINARY_PATH")"
    if [[ ! -f "$BINARY_PATH" ]]; then
        printf 'error: binary not found: %s\n' "$BINARY_PATH" >&2
        exit 1
    fi
    if [[ ! -x "$BINARY_PATH" ]]; then
        printf 'error: binary is not executable: %s\n' "$BINARY_PATH" >&2
        exit 1
    fi
fi

SMOKE_SH="$(pwd)/tests/harness/smoke.sh"
if [[ ! -f "$SMOKE_SH" ]]; then
    printf 'error: smoke.sh not found at %s\n' "$SMOKE_SH" >&2
    exit 1
fi

INSTALL_SH="$(pwd)/install.sh"
if [[ -n "$RELEASE_VER" && ! -f "$INSTALL_SH" ]]; then
    printf 'error: install.sh not found at %s\n' "$INSTALL_SH" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Run identifiers and log directory
# ---------------------------------------------------------------------------
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
LOGS_DIR="$(pwd)/tests/harness/macos/logs/${RUN_ID}"
mkdir -p "$LOGS_DIR"

printf 'Run ID  : %s\n' "$RUN_ID"
printf 'Host    : %s\n' "$SSH_HOST"
printf 'Logs dir: %s\n' "$LOGS_DIR"
printf '  tail -f %s/run.log\n\n' "$LOGS_DIR"

# Remote sandbox path — unique per run.
DEN_HOME_REMOTE="/tmp/den-harness/${RUN_ID}"

# ---------------------------------------------------------------------------
# --reset: wipe all harness sandboxes and exit.
# ---------------------------------------------------------------------------
if [[ $RESET -eq 1 ]]; then
    printf 'Resetting: removing /tmp/den-harness/ on %s ...\n' "$SSH_HOST"
    ssh -o ConnectTimeout=5 -o BatchMode=yes "$SSH_HOST" \
        'rm -rf /tmp/den-harness && printf "done\n"'
    printf 'Reset complete.\n'
    exit 0
fi

# ---------------------------------------------------------------------------
# PREFLIGHT
# ---------------------------------------------------------------------------

# 1. Loopback guard: resolve the SSH alias hostname and check it is not local.
# ---------------------------------------------------------------------------
printf '=== Preflight ===\n'
printf '[1/5] Loopback guard ...\n'

REMOTE_HOSTNAME="$(ssh -G "$SSH_HOST" | awk '/^hostname / { print $2; exit }')"
if [[ -z "$REMOTE_HOSTNAME" ]]; then
    printf 'error: could not resolve hostname for SSH alias "%s"\n' "$SSH_HOST" >&2
    printf '       Is "%s" defined in ~/.ssh/config?\n' "$SSH_HOST" >&2
    exit 1
fi
printf '  resolved hostname: %s\n' "$REMOTE_HOSTNAME"

CONTROLLER_HOSTNAME="$(hostname)"
PRIMARY_IP="$(ipconfig getifaddr en0 2>/dev/null || hostname -I 2>/dev/null | awk '{print $1}' || true)"

for _guard in localhost 127.0.0.1 "::1" "$CONTROLLER_HOSTNAME" "$PRIMARY_IP"; do
    if [[ -n "$_guard" && "$REMOTE_HOSTNAME" == "$_guard" ]]; then
        printf 'error: SSH host "%s" resolves to "%s" — refusing to run against localhost\n' \
            "$SSH_HOST" "$REMOTE_HOSTNAME" >&2
        printf '       This harness targets a separate physical machine only.\n' >&2
        exit 1
    fi
done
printf '  OK: %s is not the local machine\n' "$REMOTE_HOSTNAME"

# 2. Connectivity + key-based auth check.
# ---------------------------------------------------------------------------
printf '[2/5] Connectivity check ...\n'
if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "$SSH_HOST" true 2>/dev/null; then
    _rc=$?
    printf 'error: cannot connect to %s (exit %d)\n' "$SSH_HOST" "$_rc" >&2
    if [[ $_rc -eq 255 ]]; then
        printf '  hint: is Remote Login enabled on the test machine?\n' >&2
        printf '        System Settings → General → Sharing → Remote Login\n' >&2
        printf '  hint: is key-based auth set up? Try: ssh-copy-id %s\n' "$SSH_HOST" >&2
    fi
    exit 1
fi
printf '  OK: connected\n'

# 3. Sleep advisory: warn if lid-close sleep is enabled.
# ---------------------------------------------------------------------------
printf '[3/5] Sleep advisory ...\n'
_pmset_out="$(ssh -o ConnectTimeout=5 -o BatchMode=yes "$SSH_HOST" \
    'pmset -g | grep -E "SleepDisabled|disksleep"' 2>/dev/null || true)"
if [[ -n "$_pmset_out" ]]; then
    printf '  pmset output:\n'
    while IFS= read -r _line; do printf '    %s\n' "$_line"; done <<< "$_pmset_out"
    if echo "$_pmset_out" | grep -q 'SleepDisabled.*0\b'; then
        printf '  WARNING: SleepDisabled=0 — clamshell sleep is active.\n'
        printf '           If you close the laptop lid the harness will hang.\n'
        printf '           Fix: run on the test machine: sudo pmset -b disablesleep 1\n'
    fi
else
    printf '  (pmset output unavailable — advisory skipped)\n'
fi

# 4. Identity check: must be Darwin.
# ---------------------------------------------------------------------------
printf '[4/5] Identity check ...\n'
_identity="$(ssh -o ConnectTimeout=5 -o BatchMode=yes "$SSH_HOST" \
    'hostname && whoami && uname -s')"
_remote_uname="$(printf '%s\n' "$_identity" | tail -n1)"
printf '  remote identity:\n'
while IFS= read -r _line; do printf '    %s\n' "$_line"; done <<< "$_identity"

if [[ "$_remote_uname" != "Darwin" ]]; then
    printf 'error: remote uname -s is "%s", expected "Darwin"\n' "$_remote_uname" >&2
    printf '       This harness is macOS-only. Use tests/harness/linux/ for Linux.\n' >&2
    exit 1
fi
printf '  OK: Darwin confirmed\n'

# 5. Sandbox path guard: verify DEN_HOME does not collide with protected paths.
# ---------------------------------------------------------------------------
printf '[5/5] Sandbox path guard and mkdir ...\n'

# Compute the absolute path of DEN_HOME_REMOTE on the remote.
_abs_den_home="$(ssh -o ConnectTimeout=5 -o BatchMode=yes "$SSH_HOST" \
    "python3 -c \"import os; print(os.path.realpath('${DEN_HOME_REMOTE}'))\" 2>/dev/null \
     || realpath '${DEN_HOME_REMOTE}' 2>/dev/null \
     || echo '${DEN_HOME_REMOTE}'")"

_remote_home="$(ssh -o ConnectTimeout=5 -o BatchMode=yes "$SSH_HOST" 'echo "$HOME"')"

if [[ "$_abs_den_home" == "${_remote_home}/.den" || \
      "$_abs_den_home" == "/opt/homebrew" ]]; then
    printf 'error: DEN_HOME "%s" resolves to protected path "%s" — refusing to run\n' \
        "$DEN_HOME_REMOTE" "$_abs_den_home" >&2
    exit 1
fi

# Check for collision: the path must not already exist.
if ssh -o ConnectTimeout=5 -o BatchMode=yes "$SSH_HOST" \
        "test -e '${DEN_HOME_REMOTE}'" 2>/dev/null; then
    printf 'error: sandbox path already exists on remote: %s\n' "$DEN_HOME_REMOTE" >&2
    printf '       Run with --reset to wipe all harness sandboxes, then retry.\n' >&2
    exit 1
fi

# Verify the parent is writable by doing a mkdir/rmdir probe.
if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "$SSH_HOST" \
        "mkdir -p '${DEN_HOME_REMOTE}' && rmdir '${DEN_HOME_REMOTE}'" 2>/dev/null; then
    printf 'error: cannot create sandbox dir on remote: %s\n' "$DEN_HOME_REMOTE" >&2
    exit 1
fi
printf '  sandbox path: %s\n' "$DEN_HOME_REMOTE"
printf '  OK: writable\n'

printf '\n=== Preflight passed ===\n\n'

# ---------------------------------------------------------------------------
# SMOKE EXECUTION
# ---------------------------------------------------------------------------
printf '=== Smoke execution ===\n'

# Create the sandbox bin directory.
ssh "$SSH_HOST" "mkdir -p '${DEN_HOME_REMOTE}/bin'"

if [[ -n "$BINARY_PATH" ]]; then
    # RC mode: scp the pre-built binary.
    printf 'Copying binary to remote ...\n'
    scp "$BINARY_PATH" "${SSH_HOST}:${DEN_HOME_REMOTE}/bin/den"
    ssh "$SSH_HOST" "chmod +x '${DEN_HOME_REMOTE}/bin/den'"
    printf '  OK: %s → %s:%s/bin/den\n' "$BINARY_PATH" "$SSH_HOST" "$DEN_HOME_REMOTE"
else
    # Release mode: scp install.sh and run it.
    printf 'Copying install.sh to remote ...\n'
    scp "$INSTALL_SH" "${SSH_HOST}:${DEN_HOME_REMOTE}/install.sh"
    printf '  Running install.sh for version %s ...\n' "$RELEASE_VER"
    ssh "$SSH_HOST" \
        "DEN_INSTALL_DIR='${DEN_HOME_REMOTE}' DEN_NO_MODIFY_PATH=1 DEN_VERSION='${RELEASE_VER}' \
         bash '${DEN_HOME_REMOTE}/install.sh'"
    printf '  OK: den %s installed into %s\n' "$RELEASE_VER" "$DEN_HOME_REMOTE"
fi

# Verify DEN_BIN is inside the sandbox (belt-and-suspenders — it always is by construction).
DEN_BIN_REMOTE="${DEN_HOME_REMOTE}/bin/den"
if [[ "$DEN_BIN_REMOTE" == "${_remote_home}/.den/bin/den" || \
      "$DEN_BIN_REMOTE" == "/opt/homebrew/bin/den" ]]; then
    printf 'error: DEN_BIN "%s" points to a protected path — refusing to run\n' \
        "$DEN_BIN_REMOTE" >&2
    exit 1
fi

# Copy smoke.sh to the remote sandbox.
printf 'Copying smoke.sh to remote ...\n'
scp "$SMOKE_SH" "${SSH_HOST}:${DEN_HOME_REMOTE}/smoke.sh"

# Run smoke on the remote host; tee output to local log.
printf '\nRunning smoke set on %s ...\n\n' "$SSH_HOST"
EXIT_CODE=0
ssh "$SSH_HOST" \
    "DEN_BIN='${DEN_BIN_REMOTE}' DEN_HOME='${DEN_HOME_REMOTE}' TEST_PKG='${TEST_PKG}' \
     bash '${DEN_HOME_REMOTE}/smoke.sh'" \
    2>&1 | tee "${LOGS_DIR}/run.log" || EXIT_CODE=$?

# ---------------------------------------------------------------------------
# Parse and print summary
# ---------------------------------------------------------------------------
printf '\n--- Smoke summary ---\n'
LOG="${LOGS_DIR}/run.log"
TOTAL=0
PASS=0
FAIL=0
SKIP=0

if [[ -f "$LOG" ]]; then
    while IFS= read -r line; do
        case "$line" in
            PASS:*) (( PASS++ )); (( TOTAL++ )) ;;
            FAIL:*) (( FAIL++ )); (( TOTAL++ )) ;;
            SKIP:*) (( SKIP++ )); (( TOTAL++ )) ;;
        esac
    done < "$LOG"
fi

printf 'Total : %d\n' "$TOTAL"
printf 'PASS  : %d\n' "$PASS"
printf 'FAIL  : %d\n' "$FAIL"
printf 'SKIP  : %d\n' "$SKIP"
printf 'Logs  : %s\n' "$LOGS_DIR"

# ---------------------------------------------------------------------------
# Sandbox cleanup
# ---------------------------------------------------------------------------
SMOKE_FAILED=$([[ $EXIT_CODE -ne 0 || $FAIL -gt 0 ]] && echo 1 || echo 0)

if [[ $SMOKE_FAILED -eq 0 && $KEEP_SANDBOX -eq 0 ]]; then
    printf '\nRemoving remote sandbox %s ...\n' "$DEN_HOME_REMOTE"
    ssh "$SSH_HOST" "rm -rf '${DEN_HOME_REMOTE}'" || true
    printf '  OK: cleaned up\n'
elif [[ $SMOKE_FAILED -eq 1 ]]; then
    printf '\nSandbox preserved for inspection (failure):\n'
    printf '  ssh %s\n' "$SSH_HOST"
    printf '  ls %s\n' "$DEN_HOME_REMOTE"
    printf '  (Run with --reset to wipe all harness sandboxes when done)\n'
else
    printf '\nSandbox preserved (--keep-sandbox): %s:%s\n' "$SSH_HOST" "$DEN_HOME_REMOTE"
fi

# ---------------------------------------------------------------------------
# Exit code
# ---------------------------------------------------------------------------
if [[ $EXIT_CODE -ne 0 ]]; then
    printf '\nRESULT: FAIL (remote exited %d)\n' "$EXIT_CODE" >&2
    exit "$EXIT_CODE"
fi

if [[ $FAIL -gt 0 ]]; then
    printf '\nRESULT: FAIL (%d smoke failures)\n' "$FAIL" >&2
    exit 1
fi

printf '\nRESULT: PASS\n'
exit 0

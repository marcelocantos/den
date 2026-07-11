#!/usr/bin/env bash
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Drive the real-home soak set on a remote macOS host over SSH.
#
# Usage (from repo root):
#   tests/harness/macos/run-soak.sh [--binary PATH] [--host ALIAS]
#
# Default --binary is build/den; default --host is den-test-mac.
# Logs land in tests/harness/macos/logs/soak-<timestamp>/
set -euo pipefail

if [[ ! -f CMakeLists.txt || ! -d src ]]; then
    printf 'error: run from the repository root\n' >&2
    exit 1
fi

BINARY_PATH="build/den"
SSH_HOST="den-test-mac"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary)
            BINARY_PATH="$2"
            shift 2
            ;;
        --host)
            SSH_HOST="$2"
            shift 2
            ;;
        -h|--help)
            printf 'Usage: %s [--binary PATH] [--host ALIAS]\n' "$0"
            exit 0
            ;;
        *)
            printf 'error: unknown argument: %s\n' "$1" >&2
            exit 1
            ;;
    esac
done

if [[ ! -x "$BINARY_PATH" ]]; then
    printf 'error: binary not executable: %s (build den first)\n' "$BINARY_PATH" >&2
    exit 1
fi
BINARY_PATH="$(cd "$(dirname "$BINARY_PATH")" && pwd)/$(basename "$BINARY_PATH")"

SOAK_SH="$(pwd)/tests/harness/macos/soak.sh"
if [[ ! -f "$SOAK_SH" ]]; then
    printf 'error: soak.sh not found\n' >&2
    exit 1
fi

TS="$(date -u +%Y%m%dT%H%M%SZ)"
LOGS_DIR="$(pwd)/tests/harness/macos/logs/soak-${TS}"
mkdir -p "$LOGS_DIR"

printf '=== den remote soak ===\n'
printf 'Host   : %s\n' "$SSH_HOST"
printf 'Binary : %s\n' "$BINARY_PATH"
printf 'Logs   : %s\n\n' "$LOGS_DIR"

# Connectivity
if ! ssh -o BatchMode=yes -o ConnectTimeout=8 "$SSH_HOST" true 2>/dev/null; then
    printf 'error: cannot SSH to %s\n' "$SSH_HOST" >&2
    exit 1
fi
_uname="$(ssh -o BatchMode=yes "$SSH_HOST" 'uname -s')"
if [[ "$_uname" != "Darwin" ]]; then
    printf 'error: remote is %s, expected Darwin\n' "$_uname" >&2
    exit 1
fi

REMOTE_DIR="/tmp/den-soak-driver-${TS}"
ssh "$SSH_HOST" "mkdir -p '${REMOTE_DIR}'"
scp -q "$BINARY_PATH" "${SSH_HOST}:${REMOTE_DIR}/den"
scp -q "$SOAK_SH" "${SSH_HOST}:${REMOTE_DIR}/soak.sh"
ssh "$SSH_HOST" "chmod +x '${REMOTE_DIR}/den' '${REMOTE_DIR}/soak.sh'"

# Also install binary into ~/.den/bin so shell-integrated login shells match soak.
ssh "$SSH_HOST" "mkdir -p \"\$HOME/.den/bin\" && cp '${REMOTE_DIR}/den' \"\$HOME/.den/bin/den\" && chmod +x \"\$HOME/.den/bin/den\""

printf 'Running soak on %s (DEN_HOME=~/.den) ...\n\n' "$SSH_HOST"
EXIT_CODE=0
ssh "$SSH_HOST" \
    "export PATH=\"/opt/homebrew/bin:/usr/local/bin:\$PATH\"; \
     DEN_BIN='${REMOTE_DIR}/den' DEN_HOME=\"\$HOME/.den\" \
     bash '${REMOTE_DIR}/soak.sh'" \
    2>&1 | tee "${LOGS_DIR}/run.log" || EXIT_CODE=$?

# Summary
PASS=0
FAIL=0
SKIP=0
while IFS= read -r line || [[ -n "$line" ]]; do
    case "$line" in
        PASS:*) PASS=$((PASS + 1)) ;;
        FAIL:*) FAIL=$((FAIL + 1)) ;;
        SKIP:*) SKIP=$((SKIP + 1)) ;;
    esac
done < "${LOGS_DIR}/run.log"

printf '\n--- Soak summary ---\n'
printf 'PASS  : %d\n' "$PASS"
printf 'FAIL  : %d\n' "$FAIL"
printf 'SKIP  : %d\n' "$SKIP"
printf 'Logs  : %s\n' "$LOGS_DIR"

# Keep remote driver dir on failure for inspection
if [[ $EXIT_CODE -eq 0 && $FAIL -eq 0 ]]; then
    ssh "$SSH_HOST" "rm -rf '${REMOTE_DIR}'" || true
    printf '\nRESULT: PASS\n'
    exit 0
fi

printf '\nRESULT: FAIL (exit=%d fail_lines=%d)\n' "$EXIT_CODE" "$FAIL" >&2
printf 'Remote driver dir preserved: %s:%s\n' "$SSH_HOST" "$REMOTE_DIR" >&2
exit 1

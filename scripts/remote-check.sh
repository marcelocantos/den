#!/usr/bin/env bash
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Full remote conviction loop: isolated harness + real-home soak on den-test-mac.
# Intended for manual runs and scheduled launchd/cron.
#
# Usage (repo root):
#   scripts/remote-check.sh
#   scripts/remote-check.sh --host den-test-mac
set -euo pipefail

if [[ ! -f CMakeLists.txt ]]; then
    printf 'error: run from the repository root\n' >&2
    exit 1
fi

HOST="den-test-mac"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --host) HOST="$2"; shift 2 ;;
        -h|--help)
            printf 'Usage: %s [--host ALIAS]\n' "$0"
            exit 0
            ;;
        *) printf 'error: unknown arg %s\n' "$1" >&2; exit 1 ;;
    esac
done

ROOT="$(pwd)"
TS="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="${ROOT}/tests/harness/macos/logs/remote-check-${TS}"
mkdir -p "$OUT_DIR"

printf '=== remote-check %s ===\n' "$TS"
printf 'Host: %s\n' "$HOST"
printf 'Out : %s\n\n' "$OUT_DIR"

# Build if needed
if [[ ! -x build/den ]]; then
    printf 'Building den ...\n'
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH:-/opt/homebrew/opt/libarchive;/opt/homebrew}"
    cmake --build build
fi

OVERALL=0

printf '\n--- 1/2 isolated harness ---\n'
if tests/harness/macos/run.sh --binary build/den --host "$HOST" \
        2>&1 | tee "${OUT_DIR}/harness.log"; then
    printf 'harness: PASS\n' | tee -a "${OUT_DIR}/summary.txt"
else
    printf 'harness: FAIL\n' | tee -a "${OUT_DIR}/summary.txt"
    OVERALL=1
fi

printf '\n--- 2/2 real-home soak ---\n'
if tests/harness/macos/run-soak.sh --binary build/den --host "$HOST" \
        2>&1 | tee "${OUT_DIR}/soak.log"; then
    printf 'soak: PASS\n' | tee -a "${OUT_DIR}/summary.txt"
else
    printf 'soak: FAIL\n' | tee -a "${OUT_DIR}/summary.txt"
    OVERALL=1
fi

printf '\n=== remote-check done (exit %d) ===\n' "$OVERALL"
cat "${OUT_DIR}/summary.txt"
exit "$OVERALL"

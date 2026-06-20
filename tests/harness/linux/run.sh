#!/usr/bin/env bash
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Docker-based Linux test harness for den release-candidate validation.
#
# Usage:
#   run.sh --binary PATH  [--keep-image] [--test-pkg NAME]
#   run.sh --release VER  [--keep-image] [--test-pkg NAME]
#
# --binary PATH   Copy a pre-built local binary into the container. RC mode.
# --release VER   Run install.sh inside the container for a published release.
# --keep-image    Do not remove the Docker image after the run.
# --test-pkg NAME Override the default smoke package (default: jq).
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
KEEP_IMAGE=0
TEST_PKG="jq"

usage() {
    printf 'Usage:\n'
    printf '  %s --binary PATH  [--keep-image] [--test-pkg NAME]\n' "$0"
    printf '  %s --release VER  [--keep-image] [--test-pkg NAME]\n' "$0"
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
        --keep-image)
            KEEP_IMAGE=1
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

# Exactly one of --binary or --release is required.
if [[ -z "$BINARY_PATH" && -z "$RELEASE_VER" ]]; then
    printf 'error: one of --binary or --release is required\n' >&2
    usage
fi
if [[ -n "$BINARY_PATH" && -n "$RELEASE_VER" ]]; then
    printf 'error: --binary and --release are mutually exclusive\n' >&2
    usage
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

# ---------------------------------------------------------------------------
# Run identifiers and log directory
# ---------------------------------------------------------------------------
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
SHORT_ID="${RUN_ID: -8}"
LOGS_DIR="$(pwd)/tests/harness/linux/logs/${RUN_ID}"
mkdir -p "$LOGS_DIR"

printf 'Run ID  : %s\n' "$RUN_ID"
printf 'Logs dir: %s\n' "$LOGS_DIR"
printf '  tail -f %s/run.log\n\n' "$LOGS_DIR"

# ---------------------------------------------------------------------------
# Build the Docker image
# ---------------------------------------------------------------------------
IMAGE_TAG="den-harness-linux:run-${SHORT_ID}"
PLATFORM="linux/$(uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/')"

printf 'Building image %s for %s ...\n' "$IMAGE_TAG" "$PLATFORM"
docker build \
    --no-cache \
    --platform "$PLATFORM" \
    -t "$IMAGE_TAG" \
    tests/harness/linux/

# ---------------------------------------------------------------------------
# Compose the in-container shell command
# ---------------------------------------------------------------------------
DEN_HOME_INSIDE="/home/tester/.den-sandbox"

if [[ -n "$BINARY_PATH" ]]; then
    # RC mode: copy the staged binary into DEN_HOME and put it on PATH.
    INNER_CMD="$(cat <<'INNER'
set -euo pipefail
mkdir -p "$DEN_HOME/bin"
cp /staged/den "$DEN_HOME/bin/den"
chmod +x "$DEN_HOME/bin/den"
export PATH="$DEN_HOME/bin:$PATH"
exec bash /smoke/smoke.sh
INNER
)"
else
    # Release mode: invoke install.sh, then run the smoke suite.
    INNER_CMD="$(cat <<'INNER'
set -euo pipefail
export DEN_INSTALL_DIR="$DEN_HOME"
export DEN_NO_MODIFY_PATH=1
export DEN_VERSION="__RELEASE_VER__"
bash /install/install.sh
export PATH="$DEN_HOME/bin:$PATH"
exec bash /smoke/smoke.sh
INNER
)"
    # Substitute the actual release version (safe: only alphanumeric+dot+hyphen).
    INNER_CMD="${INNER_CMD/__RELEASE_VER__/$RELEASE_VER}"
fi

# ---------------------------------------------------------------------------
# Build docker run arguments
# ---------------------------------------------------------------------------
DOCKER_ARGS=(
    --rm
    --name "den-harness-${SHORT_ID}"
    --platform "$PLATFORM"
    # Smoke script (read-only)
    -v "${SMOKE_SH}:/smoke/smoke.sh:ro"
    # Logs (read-write)
    -v "${LOGS_DIR}:/logs:rw"
    # Environment for smoke.sh
    -e "DEN_HOME=${DEN_HOME_INSIDE}"
    -e "TEST_PKG=${TEST_PKG}"
    -e "DEN_BIN=${DEN_HOME_INSIDE}/bin/den"
)

if [[ -n "$BINARY_PATH" ]]; then
    DOCKER_ARGS+=(
        # Pre-built binary (read-only); runner copies it into DEN_HOME.
        -v "${BINARY_PATH}:/staged/den:ro"
    )
else
    # Mount install.sh from the repo root.
    DOCKER_ARGS+=(
        -v "$(pwd)/install.sh:/install/install.sh:ro"
    )
fi

# ---------------------------------------------------------------------------
# Run the container, tee output to log
# ---------------------------------------------------------------------------
printf 'Starting container ...\n\n'
EXIT_CODE=0
docker run "${DOCKER_ARGS[@]}" "$IMAGE_TAG" \
    bash -c "$INNER_CMD" \
    2>&1 | tee "${LOGS_DIR}/run.log" || EXIT_CODE=$?

# ---------------------------------------------------------------------------
# Capture sandbox state snapshot
# ---------------------------------------------------------------------------
printf '\nCapturing sandbox state ...\n'
# Run a second, ephemeral container to list what ended up in the sandbox.
# (The first container used --rm, so its filesystem is gone.)
# In binary mode the sandbox was populated inside the first container; we
# reconstruct it only to the extent the logs captured it.  Instead, we record
# the fact that we cannot snapshot post-exit and leave a note.
cat >"${LOGS_DIR}/sandbox-snapshot.txt" <<EOF
# Sandbox snapshot is not available for --rm containers.
# The run.log above contains all stdout/stderr from the container.
# To inspect the sandbox interactively, re-run without --rm and exec in.
EOF

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
    # NB: arithmetic `(( expr ))` returns exit code 1 when expr evaluates to
    # 0 (e.g. `(( PASS++ ))` on the first hit, when PASS was 0).  Under
    # `set -e` that aborts the script before the summary prints — even
    # though smoke itself was green.  Use plain assignment to dodge it.
    while IFS= read -r line; do
        case "$line" in
            PASS:*) PASS=$((PASS + 1)); TOTAL=$((TOTAL + 1)) ;;
            FAIL:*) FAIL=$((FAIL + 1)); TOTAL=$((TOTAL + 1)) ;;
            SKIP:*) SKIP=$((SKIP + 1)); TOTAL=$((TOTAL + 1)) ;;
        esac
    done < "$LOG"
fi

printf 'Total : %d\n' "$TOTAL"
printf 'PASS  : %d\n' "$PASS"
printf 'FAIL  : %d\n' "$FAIL"
printf 'SKIP  : %d\n' "$SKIP"
printf 'Logs  : %s\n' "$LOGS_DIR"

# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------
if [[ $KEEP_IMAGE -eq 0 ]]; then
    printf '\nRemoving image %s ...\n' "$IMAGE_TAG"
    docker rmi "$IMAGE_TAG" >/dev/null 2>&1 || true
else
    printf '\nImage retained: %s\n' "$IMAGE_TAG"
fi

# ---------------------------------------------------------------------------
# Exit code: propagate container's exit code.
# ---------------------------------------------------------------------------
if [[ $EXIT_CODE -ne 0 ]]; then
    printf '\nRESULT: FAIL (container exited %d)\n' "$EXIT_CODE" >&2
    exit "$EXIT_CODE"
fi

if [[ $FAIL -gt 0 ]]; then
    printf '\nRESULT: FAIL (%d smoke failures)\n' "$FAIL" >&2
    exit 1
fi

printf '\nRESULT: PASS\n'
exit 0

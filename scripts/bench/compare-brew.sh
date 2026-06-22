#!/usr/bin/env bash
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# compare-brew.sh — benchmark den vs brew on common operations.
#
# Requires: hyperfine (brew install hyperfine), jq, brew
# Usage:    scripts/bench/compare-brew.sh [options]
#
#   --output-dir DIR    Where to write JSON/CSV (default: bench/results/)
#   --runs N            hyperfine repetitions for read-only ops (default: 10)
#   --install-runs N    repetitions for install/upgrade ops (default: 3)
#   --with-install      Enable install + upgrade benchmarks (mutates the
#                       shared Cellar; cleans up after itself). Off by default
#                       so the read-only ops never touch system state.
#   --pkg NAME          Package used for install/upgrade (default: hello —
#                       tiny, no deps, bottle available).
#   --json              (reserved; output is always JSON+CSV)
#
# Measures: list, info, search, install, upgrade.
#
# Design notes
# ------------
# * Read-only ops (list/info/search) run against an isolated DEN_HOME so the
#   user's environment is untouched, and a copied index so no network is
#   needed.
# * install/upgrade are network-sensitive and mutate the shared Cellar. They
#   are gated behind --with-install. To keep them deterministic and offline
#   after a one-time warm-up:
#     - both download caches are warmed once up front (the only network step);
#     - each timed run is preceded by an uninstall (hyperfine --prepare), so it
#       measures a clean, cache-warm install rather than a no-op re-install;
#     - the package is uninstalled from both tools on exit.
#   If warm-up fails (offline, bottle unavailable), the op degrades to SKIPPED
#   and the script still exits 0.
# * Skipped ops are reported but do not cause a non-zero exit.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
OUTPUT_DIR="${REPO_ROOT}/bench/results"
RUNS=10
INSTALL_RUNS=3
WITH_INSTALL=false
JSON_OUTPUT=false
TIMESTAMP="$(date -u +%Y%m%dT%H%M%SZ)"
TEST_PKG="hello"        # tiny, no deps, bottle available — for install/upgrade
SEARCH_QUERY="json"     # common search term

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir)   OUTPUT_DIR="$2";   shift 2 ;;
        --runs)         RUNS="$2";         shift 2 ;;
        --install-runs) INSTALL_RUNS="$2"; shift 2 ;;
        --with-install) WITH_INSTALL=true; shift ;;
        --pkg)          TEST_PKG="$2";     shift 2 ;;
        --json)         JSON_OUTPUT=true;  shift ;;
        *)              echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

mkdir -p "${OUTPUT_DIR}"

# ---------------------------------------------------------------------------
# Tool checks
# ---------------------------------------------------------------------------
if ! command -v hyperfine &>/dev/null; then
    echo "ERROR: hyperfine not found. Install with: brew install hyperfine" >&2
    exit 1
fi

DEN_BIN="${REPO_ROOT}/build/den"
if [[ ! -x "${DEN_BIN}" ]]; then
    echo "ERROR: den binary not found at ${DEN_BIN}. Build first:" >&2
    echo "  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build" >&2
    exit 1
fi

if ! command -v brew &>/dev/null; then
    echo "ERROR: brew not found. This benchmark requires Homebrew as the reference." >&2
    exit 1
fi

DEN_VERSION="$("${DEN_BIN}" --version 2>/dev/null | tail -1 || echo unknown)"
BREW_VERSION="$(brew --version 2>&1 | head -1 || echo unknown)"
HOST_OS="$(uname -s)-$(uname -m)"

echo "=== den vs brew benchmark ==="
echo "den:          ${DEN_VERSION}"
echo "brew:         ${BREW_VERSION}"
echo "host:         ${HOST_OS}"
echo "runs:         ${RUNS}"
echo "install-runs: ${INSTALL_RUNS}"
echo "pkg:          ${TEST_PKG}"
echo "with-install: ${WITH_INSTALL}"
echo ""

# ---------------------------------------------------------------------------
# Result accumulation
# ---------------------------------------------------------------------------
RESULT_JSON="${OUTPUT_DIR}/bench-${TIMESTAMP}.json"
RESULT_CSV="${OUTPUT_DIR}/bench-${TIMESTAMP}.csv"

declare -a JSON_ENTRIES=()
declare -a CSV_ROWS=()
CSV_HEADER="timestamp,op,tool,mean_s,stddev_s,min_s,max_s,status"
CSV_ROWS+=("${CSV_HEADER}")

# record_skip OP TOOL REASON — note an op we could not measure.
record_skip() {
    local op="$1" tool="$2" reason="$3"
    echo "  SKIPPED: ${tool} ${op} (${reason})"
    JSON_ENTRIES+=("{\"timestamp\":\"${TIMESTAMP}\",\"op\":\"${op}\",\"tool\":\"${tool}\",\"mean_s\":null,\"stddev_s\":null,\"min_s\":null,\"max_s\":null,\"status\":\"skipped: ${reason}\"}")
    CSV_ROWS+=("${TIMESTAMP},${op},${tool},,,,,skipped: ${reason}")
}

# ---------------------------------------------------------------------------
# parse_hf_json TMPFILE OP — append every result row in a hyperfine JSON export.
# hyperfine names each command by its --command-name, e.g. "den-list".
# ---------------------------------------------------------------------------
parse_hf_json() {
    local tmpfile="$1" op="$2"
    if [[ ! -f "${tmpfile}" ]]; then
        echo "  WARNING: no hyperfine output for op '${op}'" >&2
        return 1
    fi
    local n
    n="$(jq '.results | length' "${tmpfile}")"
    local j
    for ((j = 0; j < n; j++)); do
        local name mean stddev min max
        name="$(jq -r ".results[${j}].command" "${tmpfile}")"
        mean="$(jq -r ".results[${j}].mean" "${tmpfile}")"
        stddev="$(jq -r ".results[${j}].stddev // 0" "${tmpfile}")"
        min="$(jq -r ".results[${j}].min" "${tmpfile}")"
        max="$(jq -r ".results[${j}].max" "${tmpfile}")"
        JSON_ENTRIES+=("{\"timestamp\":\"${TIMESTAMP}\",\"op\":\"${op}\",\"tool\":\"${name}\",\"mean_s\":${mean},\"stddev_s\":${stddev},\"min_s\":${min},\"max_s\":${max},\"status\":\"ok\"}")
        CSV_ROWS+=("${TIMESTAMP},${op},${name},${mean},${stddev},${min},${max},ok")
    done
}

# ---------------------------------------------------------------------------
# run_op OP DEN_CMD BREW_CMD — benchmark a read-only op for both tools at once.
# An empty command string skips that tool.
# ---------------------------------------------------------------------------
run_op() {
    local op="$1" den_cmd="$2" brew_cmd="$3"
    echo "--- op: ${op} ---"

    local tmpfile
    tmpfile="$(mktemp /tmp/den-bench-XXXXXX.json)"

    local hf_args=(
        --runs "${RUNS}"
        --warmup 1
        --export-json "${tmpfile}"
        --style basic
        --time-unit millisecond
    )

    if [[ -n "${den_cmd}" ]]; then
        hf_args+=(--command-name "den-${op}" "${den_cmd}")
    fi
    if [[ -n "${brew_cmd}" ]]; then
        hf_args+=(--command-name "brew-${op}" "${brew_cmd}")
    fi

    if hyperfine "${hf_args[@]}"; then
        parse_hf_json "${tmpfile}" "${op}" || true
    else
        echo "  WARNING: hyperfine failed for op '${op}'" >&2
    fi
    rm -f "${tmpfile}"
}

# ---------------------------------------------------------------------------
# bench_install_one TOOL DEN_BIN_OR_BREW INSTALL_CMD UNINSTALL_CMD CHECK_CMD
# Benchmarks a single tool's install of TEST_PKG, uninstalling before each run
# (hyperfine --prepare) so every timed run is a clean, cache-warm install.
# Returns non-zero (and records a skip) if the warm-up install/uninstall fails.
# ---------------------------------------------------------------------------
bench_install_one() {
    local tool="$1" install_cmd="$2" uninstall_cmd="$3"

    # Warm-up: one real install (the only network-touching step) to populate
    # the download cache, then uninstall so the timed runs start clean.
    if ! eval "${install_cmd}" >/dev/null 2>&1; then
        record_skip "install" "${tool}" "warm-up install failed (offline or bottle unavailable)"
        return 1
    fi
    eval "${uninstall_cmd}" >/dev/null 2>&1 || true

    local tmpfile
    tmpfile="$(mktemp /tmp/den-bench-XXXXXX.json)"
    if hyperfine \
        --runs "${INSTALL_RUNS}" \
        --prepare "${uninstall_cmd} >/dev/null 2>&1 || true" \
        --export-json "${tmpfile}" \
        --style basic \
        --time-unit millisecond \
        --command-name "${tool}-install" \
        "${install_cmd} >/dev/null 2>&1"; then
        parse_hf_json "${tmpfile}" "install" || true
    else
        record_skip "install" "${tool}" "hyperfine run failed"
    fi
    rm -f "${tmpfile}"
    # Leave the package uninstalled for the upgrade benchmark / cleanup.
    eval "${uninstall_cmd}" >/dev/null 2>&1 || true
}

# ---------------------------------------------------------------------------
# Prepare a fresh isolated DEN_HOME for the read-only ops.
# ---------------------------------------------------------------------------
DEN_HOME_TMP="$(mktemp -d /tmp/den-bench-home-XXXXXX)"

cleanup() {
    rm -rf "${DEN_HOME_TMP}"
}
trap cleanup EXIT

export DEN_HOME="${DEN_HOME_TMP}"

REAL_DEN_HOME="${HOME}/.den"
REAL_INDEX="${REAL_DEN_HOME}/cache/index.json"
BENCH_CACHE="${DEN_HOME_TMP}/cache"
mkdir -p "${BENCH_CACHE}"

if [[ -f "${REAL_INDEX}" ]]; then
    echo "Using existing den index from ${REAL_INDEX}"
    cp "${REAL_INDEX}" "${BENCH_CACHE}/index.json"
else
    echo "Fetching den index (one-time setup)..."
    "${DEN_BIN}" update || echo "WARNING: 'den update' failed — info/search may be skipped." >&2
fi

INDEX_READY=false
[[ -f "${BENCH_CACHE}/index.json" ]] && INDEX_READY=true

# ---------------------------------------------------------------------------
# OP: list  (always available)
# ---------------------------------------------------------------------------
run_op "list" "${DEN_BIN} list" "brew list"

# ---------------------------------------------------------------------------
# OP: info
# ---------------------------------------------------------------------------
if $INDEX_READY; then
    run_op "info" "${DEN_BIN} info ${TEST_PKG}" "brew info ${TEST_PKG}"
else
    echo "--- op: info ---"
    record_skip "info" "den" "index not available — run 'den update' first"
    record_skip "info" "brew" "skipped to keep comparison fair"
fi

# ---------------------------------------------------------------------------
# OP: search
# ---------------------------------------------------------------------------
if $INDEX_READY; then
    run_op "search" "${DEN_BIN} search ${SEARCH_QUERY}" "brew search ${SEARCH_QUERY}"
else
    echo "--- op: search ---"
    record_skip "search" "den" "index not available"
    record_skip "search" "brew" "skipped to keep comparison fair"
fi

# ---------------------------------------------------------------------------
# OP: install + upgrade  (gated behind --with-install)
# ---------------------------------------------------------------------------
if $WITH_INSTALL; then
    # The install/upgrade ops mutate the shared Cellar; restore it on exit by
    # uninstalling TEST_PKG from both tools (best effort).
    INSTALL_CLEANUP_PKG="${TEST_PKG}"
    cleanup() {
        rm -rf "${DEN_HOME_TMP}"
        "${DEN_BIN}" uninstall "${INSTALL_CLEANUP_PKG}" >/dev/null 2>&1 || true
        brew uninstall --force "${INSTALL_CLEANUP_PKG}" >/dev/null 2>&1 || true
    }
    trap cleanup EXIT

    if ! $INDEX_READY; then
        echo "--- op: install ---"
        record_skip "install" "den" "index not available"
        record_skip "install" "brew" "skipped (paired with den)"
        echo "--- op: upgrade ---"
        record_skip "upgrade" "den" "index not available"
        record_skip "upgrade" "brew" "skipped (paired with den)"
    else
        echo "--- op: install (pkg=${TEST_PKG}, runs=${INSTALL_RUNS}) ---"
        echo "  WARNING: install/upgrade mutate the shared Cellar (cleaned up on exit)."

        # Ensure a clean starting point.
        "${DEN_BIN}" uninstall "${TEST_PKG}" >/dev/null 2>&1 || true
        brew uninstall --force "${TEST_PKG}" >/dev/null 2>&1 || true

        bench_install_one "den" \
            "${DEN_BIN} install ${TEST_PKG}" \
            "${DEN_BIN} uninstall ${TEST_PKG}"

        bench_install_one "brew" \
            "brew install ${TEST_PKG}" \
            "brew uninstall --force ${TEST_PKG}"

        # -------------------------------------------------------------------
        # OP: upgrade
        # A true upgrade requires an older version installed and a newer one
        # available. den/brew almost always serve the latest version of a
        # leaf package, so a real version bump isn't deterministically
        # available. We benchmark the *no-op upgrade* path instead — the
        # common case a user hits when nothing is stale — which exercises the
        # same manifest/index/version-resolution machinery without a download.
        # -------------------------------------------------------------------
        echo "--- op: upgrade (no-op path, pkg=${TEST_PKG}) ---"

        # den: install once, then time 'upgrade' (already current → no work).
        if "${DEN_BIN}" install "${TEST_PKG}" >/dev/null 2>&1; then
            tmpfile="$(mktemp /tmp/den-bench-XXXXXX.json)"
            if hyperfine \
                --runs "${RUNS}" \
                --warmup 1 \
                --export-json "${tmpfile}" \
                --style basic \
                --time-unit millisecond \
                --command-name "den-upgrade" \
                "${DEN_BIN} upgrade ${TEST_PKG} >/dev/null 2>&1"; then
                parse_hf_json "${tmpfile}" "upgrade" || true
            else
                record_skip "upgrade" "den" "hyperfine run failed"
            fi
            rm -f "${tmpfile}"
            "${DEN_BIN}" uninstall "${TEST_PKG}" >/dev/null 2>&1 || true
        else
            record_skip "upgrade" "den" "install for upgrade baseline failed"
        fi

        # brew: install once, then time 'upgrade' (already current → no work).
        if brew install "${TEST_PKG}" >/dev/null 2>&1; then
            tmpfile="$(mktemp /tmp/den-bench-XXXXXX.json)"
            if hyperfine \
                --runs "${RUNS}" \
                --warmup 1 \
                --export-json "${tmpfile}" \
                --style basic \
                --time-unit millisecond \
                --command-name "brew-upgrade" \
                "brew upgrade ${TEST_PKG} >/dev/null 2>&1"; then
                parse_hf_json "${tmpfile}" "upgrade" || true
            else
                record_skip "upgrade" "brew" "hyperfine run failed"
            fi
            rm -f "${tmpfile}"
            brew uninstall --force "${TEST_PKG}" >/dev/null 2>&1 || true
        else
            record_skip "upgrade" "brew" "install for upgrade baseline failed"
        fi
    fi
else
    echo "--- op: install ---"
    record_skip "install" "den" "disabled (pass --with-install to enable)"
    record_skip "install" "brew" "disabled (pass --with-install to enable)"
    echo "--- op: upgrade ---"
    record_skip "upgrade" "den" "disabled (pass --with-install to enable)"
    record_skip "upgrade" "brew" "disabled (pass --with-install to enable)"
fi

# ---------------------------------------------------------------------------
# Write results
# ---------------------------------------------------------------------------
echo ""
echo "Writing results..."

{
    echo "["
    first=true
    for entry in "${JSON_ENTRIES[@]}"; do
        if $first; then
            echo "  ${entry}"
            first=false
        else
            echo "  ,${entry}"
        fi
    done
    echo "]"
} > "${RESULT_JSON}"

printf '%s\n' "${CSV_ROWS[@]}" > "${RESULT_CSV}"

echo "Results written:"
echo "  JSON: ${RESULT_JSON}"
echo "  CSV:  ${RESULT_CSV}"
echo ""

# ---------------------------------------------------------------------------
# Summary table
# ---------------------------------------------------------------------------
if [[ -s "${RESULT_JSON}" ]]; then
    echo "=== Summary (mean latency, milliseconds) ==="
    printf "%-14s %-10s %s\n" "tool" "op" "mean_ms"
    printf "%-14s %-10s %s\n" "----" "--" "-------"
    jq -r '.[] | [.tool, .op, (if .mean_s == null then "skipped" else (.mean_s * 1000 | floor | tostring) end)] | @tsv' \
        "${RESULT_JSON}" 2>/dev/null \
        | while IFS=$'\t' read -r tool op mean; do
            printf "%-14s %-10s %s\n" "${tool}" "${op}" "${mean}"
        done
fi

echo ""
echo "Done. Compare against baselines with: scripts/bench/compare-results.sh"

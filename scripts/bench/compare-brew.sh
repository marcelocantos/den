#!/usr/bin/env bash
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# compare-brew.sh — benchmark den vs brew on common operations.
#
# Requires: hyperfine (brew install hyperfine)
# Usage:    scripts/bench/compare-brew.sh [--output-dir DIR] [--runs N] [--json]
#
# Measures: list, info, search, install, upgrade
# Skipped ops are reported but do not cause a non-zero exit.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
OUTPUT_DIR="${REPO_ROOT}/bench/results"
RUNS=10
JSON_OUTPUT=false
TIMESTAMP="$(date -u +%Y%m%dT%H%M%SZ)"
TEST_PKG="jq"          # tiny, no deps — representative for install/upgrade
SEARCH_QUERY="json"    # common search term

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --runs)       RUNS="$2";       shift 2 ;;
        --json)       JSON_OUTPUT=true; shift ;;
        *)            echo "Unknown option: $1" >&2; exit 1 ;;
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

DEN_VERSION="$("${DEN_BIN}" --version 2>&1 || echo unknown)"
BREW_VERSION="$(brew --version 2>&1 | head -1 || echo unknown)"
HOST_OS="$(uname -s)-$(uname -m)"

echo "=== den vs brew benchmark ==="
echo "den:  ${DEN_VERSION}"
echo "brew: ${BREW_VERSION}"
echo "host: ${HOST_OS}"
echo "runs: ${RUNS}"
echo "pkg:  ${TEST_PKG}"
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

# ---------------------------------------------------------------------------
# Helper: run hyperfine for one op, extract timings, append to results.
# ---------------------------------------------------------------------------
# run_op OP DEN_CMD BREW_CMD
# If DEN_CMD or BREW_CMD is empty (""), the tool is skipped for that op.
run_op() {
    local op="$1"
    local den_cmd="$2"
    local brew_cmd="$3"

    echo "--- op: ${op} ---"

    local tmpfile
    tmpfile="$(mktemp /tmp/den-bench-XXXXXX.json)"
    trap "rm -f ${tmpfile}" RETURN

    local hf_args=(
        --runs "${RUNS}"
        --export-json "${tmpfile}"
        --style basic
        --time-unit millisecond
    )

    local labels=()
    local commands=()

    if [[ -n "${den_cmd}" ]]; then
        labels+=(--command-name "den-${op}")
        commands+=("${den_cmd}")
    else
        echo "  SKIPPED: den ${op} (not yet implemented)"
    fi

    if [[ -n "${brew_cmd}" ]]; then
        labels+=(--command-name "brew-${op}")
        commands+=("${brew_cmd}")
    else
        echo "  SKIPPED: brew ${op}"
    fi

    if [[ ${#commands[@]} -eq 0 ]]; then
        return
    fi

    # Build the interleaved label+command args.
    local hf_cmd_args=()
    local i=0
    while [[ $i -lt ${#labels[@]} ]]; do
        hf_cmd_args+=("${labels[$i]}" "${commands[$i]}")
        ((i++)) || true
    done

    hyperfine "${hf_args[@]}" "${hf_cmd_args[@]}" || {
        echo "  WARNING: hyperfine failed for op '${op}'" >&2
        return
    }

    # Parse the JSON output and append to our result arrays.
    if [[ -f "${tmpfile}" ]] && command -v jq &>/dev/null; then
        local n_results
        n_results="$(jq '.results | length' "${tmpfile}")"
        for ((j=0; j<n_results; j++)); do
            local name mean stddev min max status
            name="$(jq -r ".results[${j}].command" "${tmpfile}")"
            mean="$(jq -r ".results[${j}].mean" "${tmpfile}")"
            stddev="$(jq -r ".results[${j}].stddev" "${tmpfile}")"
            min="$(jq -r ".results[${j}].min" "${tmpfile}")"
            max="$(jq -r ".results[${j}].max" "${tmpfile}")"
            status="ok"

            # Convert ms (hyperfine exports seconds when time-unit is ms but
            # the JSON always uses seconds for mean/min/max).
            JSON_ENTRIES+=("{\"timestamp\":\"${TIMESTAMP}\",\"op\":\"${op}\",\"tool\":\"${name}\",\"mean_s\":${mean},\"stddev_s\":${stddev},\"min_s\":${min},\"max_s\":${max},\"status\":\"${status}\"}")
            CSV_ROWS+=("${TIMESTAMP},${op},${name},${mean},${stddev},${min},${max},${status}")
        done
    else
        # No jq — record a partial row.
        local warn="jq not available; timings not parsed"
        JSON_ENTRIES+=("{\"timestamp\":\"${TIMESTAMP}\",\"op\":\"${op}\",\"status\":\"${warn}\"}")
        CSV_ROWS+=("${TIMESTAMP},${op},,,,,, ${warn}")
    fi
}

# ---------------------------------------------------------------------------
# Prepare a fresh isolated DEN_HOME so we don't touch the user's environment.
# ---------------------------------------------------------------------------
DEN_HOME_TMP="$(mktemp -d /tmp/den-bench-home-XXXXXX)"
trap "rm -rf ${DEN_HOME_TMP}" EXIT

export DEN_HOME="${DEN_HOME_TMP}"

# Seed the den index so info/search/list work without a network call.
# If the user's real cache already has an index, copy it; otherwise fetch it.
REAL_DEN_HOME="${HOME}/.den"
REAL_INDEX="${REAL_DEN_HOME}/cache/index.json"
BENCH_CACHE="${DEN_HOME_TMP}/cache"
mkdir -p "${BENCH_CACHE}"

if [[ -f "${REAL_INDEX}" ]]; then
    echo "Using existing den index from ${REAL_INDEX}"
    cp "${REAL_INDEX}" "${BENCH_CACHE}/index.json"
else
    echo "Fetching den index (one-time setup)..."
    "${DEN_BIN}" update || {
        echo "WARNING: 'den update' failed — info/search may be skipped." >&2
    }
fi

INDEX_READY=false
[[ -f "${BENCH_CACHE}/index.json" ]] && INDEX_READY=true

# ---------------------------------------------------------------------------
# OP: list
# ---------------------------------------------------------------------------
# Both 'den list' and 'brew list' are always available.
run_op "list" \
    "${DEN_BIN} list" \
    "brew list"

# ---------------------------------------------------------------------------
# OP: info
# ---------------------------------------------------------------------------
if $INDEX_READY; then
    run_op "info" \
        "${DEN_BIN} info ${TEST_PKG}" \
        "brew info ${TEST_PKG}"
else
    echo "--- op: info ---"
    echo "  SKIPPED: den info (index not available — run 'den update' first)"
    echo "  SKIPPED: brew info (skipped to keep comparison fair)"
fi

# ---------------------------------------------------------------------------
# OP: search
# ---------------------------------------------------------------------------
if $INDEX_READY; then
    run_op "search" \
        "${DEN_BIN} search ${SEARCH_QUERY}" \
        "brew search ${SEARCH_QUERY}"
else
    echo "--- op: search ---"
    echo "  SKIPPED: den search (index not available)"
    echo "  SKIPPED: brew search (skipped to keep comparison fair)"
fi

# ---------------------------------------------------------------------------
# OP: install
# Requires network. den install is implemented but depends on the index and
# a working bottle download. Skip if index isn't ready.
# ---------------------------------------------------------------------------
echo "--- op: install ---"
echo "  SKIPPED: install (network-dependent; excluded from automated bench run)"
echo "  NOTE: run manually: hyperfine --runs 3 'den install ${TEST_PKG}' 'brew install ${TEST_PKG}'"

# ---------------------------------------------------------------------------
# OP: upgrade
# Requires an already-installed package with an available newer version.
# Skipped for the same reasons as install.
# ---------------------------------------------------------------------------
echo "--- op: upgrade ---"
echo "  SKIPPED: upgrade (network-dependent; excluded from automated bench run)"
echo "  NOTE: run manually after installing an outdated package."

# ---------------------------------------------------------------------------
# Write results
# ---------------------------------------------------------------------------
echo ""
echo "Writing results..."

# JSON
{
    echo "["
    local_IFS="${IFS}"
    IFS=$'\n'
    first=true
    for entry in "${JSON_ENTRIES[@]}"; do
        if $first; then
            echo "  ${entry}"
            first=false
        else
            echo "  ,${entry}"
        fi
    done
    IFS="${local_IFS}"
    echo "]"
} > "${RESULT_JSON}"

# CSV
printf '%s\n' "${CSV_ROWS[@]}" > "${RESULT_CSV}"

echo "Results written:"
echo "  JSON: ${RESULT_JSON}"
echo "  CSV:  ${RESULT_CSV}"
echo ""

# ---------------------------------------------------------------------------
# Summary table
# ---------------------------------------------------------------------------
if command -v jq &>/dev/null && [[ -s "${RESULT_JSON}" ]]; then
    echo "=== Summary (mean latency, seconds) ==="
    printf "%-20s %-10s %s\n" "tool" "op" "mean_s"
    printf "%-20s %-10s %s\n" "----" "--" "------"
    jq -r '.[] | [.tool, .op, (.mean_s | tostring)] | @tsv' "${RESULT_JSON}" 2>/dev/null \
        | while IFS=$'\t' read -r tool op mean; do
            printf "%-20s %-10s %s\n" "${tool}" "${op}" "${mean}"
        done
fi

echo ""
echo "Done. To view historical trends: cat ${OUTPUT_DIR}/bench-*.json | jq ."

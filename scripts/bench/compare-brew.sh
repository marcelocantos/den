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
#   --runs N            hyperfine repetitions per round for read-only ops
#                       and upgrade (default: 10)
#   --install-runs N    repetitions per round for install (default: 3)
#   --rounds N          independent hyperfine rounds per op (default: 3, or
#                       $DEN_BENCH_ROUNDS). Rounds alternate den/brew order
#                       and their per-run times are pooled, so each tool gets
#                       RUNS x ROUNDS samples per op; compare-results.sh
#                       gates on the median of the pool.
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
# * Each op is measured in ROUNDS separate hyperfine invocations, with den
#   first on odd rounds and brew first on even rounds. A single hyperfine
#   call runs all of den's iterations and then all of brew's, so a runner
#   slowdown partway through lands on one tool only; interleaving rounds
#   spreads it over both. Per-run times from every round are pooled by
#   merge-hyperfine.sh into mean/median/stddev/min/max plus the raw times_s.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
OUTPUT_DIR="${REPO_ROOT}/bench/results"
RUNS=10
INSTALL_RUNS=3
ROUNDS="${DEN_BENCH_ROUNDS:-3}"
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
        --rounds)       ROUNDS="$2";       shift 2 ;;
        --with-install) WITH_INSTALL=true; shift ;;
        --pkg)          TEST_PKG="$2";     shift 2 ;;
        --json)         JSON_OUTPUT=true;  shift ;;
        *)              echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if ! [[ "${ROUNDS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "ERROR: --rounds must be a positive integer (got '${ROUNDS}')" >&2
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"

# ---------------------------------------------------------------------------
# Tool checks
# ---------------------------------------------------------------------------
if ! command -v hyperfine &>/dev/null; then
    echo "ERROR: hyperfine not found. Install with: brew install hyperfine" >&2
    exit 1
fi

# hyperfine 2.0 dropped --time-unit (units moved to --metrics METRIC:UNIT)
# and changed its JSON export schema; merge-hyperfine.sh reads both schemas.
# Only pass a display-unit flag the installed hyperfine understands.
HF_UNIT_ARGS=()
HF_HELP="$(hyperfine --help 2>&1 || true)"
if grep -q -- '--time-unit' <<<"${HF_HELP}"; then
    HF_UNIT_ARGS=(--time-unit millisecond)
elif grep -q -- '--metrics' <<<"${HF_HELP}"; then
    HF_UNIT_ARGS=(--metrics time_wall_clock:ms)
fi
HF_VERSION="$(hyperfine --version 2>/dev/null | head -1 || echo unknown)"

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

# Stable host class for regression comparisons. GitHub Actions macos-14
# runners and a local M4 Max are both Darwin/arm64, but their absolute
# times are not comparable — so CI must not treat a local snapshot as
# its baseline (that is what made weekly bench.yml fail after T68).
if [[ "${GITHUB_ACTIONS:-}" == "true" ]]; then
    HOST_ID="gha-${ImageOS:-${RUNNER_OS:-unknown}}-${RUNNER_ARCH:-unknown}"
else
    HOST_ID="${HOST_OS}-local"
fi
HOST_ID="$(printf '%s' "${HOST_ID}" | tr -c 'A-Za-z0-9._-' '_')"

echo "=== den vs brew benchmark ==="
echo "den:          ${DEN_VERSION}"
echo "brew:         ${BREW_VERSION}"
echo "hyperfine:    ${HF_VERSION}"
echo "host:         ${HOST_OS}"
echo "host-id:      ${HOST_ID}"
echo "runs:         ${RUNS}"
echo "install-runs: ${INSTALL_RUNS}"
echo "rounds:       ${ROUNDS}  (samples per op: ${RUNS}x${ROUNDS} read-only/upgrade, ${INSTALL_RUNS}x${ROUNDS} install)"
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
CSV_HEADER="timestamp,host,op,tool,mean_s,stddev_s,min_s,max_s,status,median_s,runs"
CSV_ROWS+=("${CSV_HEADER}")

# record_skip OP TOOL REASON — note an op we could not measure.
record_skip() {
    local op="$1" tool="$2" reason="$3"
    echo "  SKIPPED: ${tool} ${op} (${reason})"
    JSON_ENTRIES+=("{\"timestamp\":\"${TIMESTAMP}\",\"host\":\"${HOST_ID}\",\"op\":\"${op}\",\"tool\":\"${tool}\",\"mean_s\":null,\"stddev_s\":null,\"min_s\":null,\"max_s\":null,\"median_s\":null,\"runs\":0,\"status\":\"skipped: ${reason}\"}")
    CSV_ROWS+=("${TIMESTAMP},${HOST_ID},${op},${tool},,,,,skipped: ${reason},,0")
}

# ---------------------------------------------------------------------------
# Per-round hyperfine exports land here and are pooled per op by
# record_rounds. Kept outside DEN_HOME_TMP so the install cleanup trap
# cannot remove them early.
# ---------------------------------------------------------------------------
HF_DIR="$(mktemp -d /tmp/den-bench-hf-XXXXXX)"
MERGE="${REPO_ROOT}/scripts/bench/merge-hyperfine.sh"

# hf_out OP TAG ROUND — path for one round's hyperfine export.
hf_out() {
    printf '%s/%s-%s-r%02d.json' "${HF_DIR}" "$1" "$2" "$3"
}

# record_rounds OP — pool every round exported for OP and append one row per
# tool (den-OP, brew-OP). Returns 1 if no round produced output.
record_rounds() {
    local op="$1"
    local files=()
    local f
    for f in "${HF_DIR}/${op}"-*.json; do
        [[ -s "${f}" ]] && files+=("${f}")
    done
    if [[ ${#files[@]} -eq 0 ]]; then
        echo "  WARNING: no hyperfine output for op '${op}'" >&2
        return 1
    fi
    local row
    while IFS= read -r row; do
        [[ -z "${row}" ]] && continue
        JSON_ENTRIES+=("${row}")
        CSV_ROWS+=("$(jq -r '[.timestamp, .host, .op, .tool, .mean_s, .stddev_s, .min_s, .max_s, .status, .median_s, .runs] | map(tostring) | join(",")' <<<"${row}")")
        jq -r '"  pooled \(.tool): median \(.median_s*1000*10|round/10) ms, mean \(.mean_s*1000*10|round/10) ms over \(.runs) runs (\(.rounds) rounds)"' <<<"${row}"
    done < <("${MERGE}" "${op}" "${HOST_ID}" "${TIMESTAMP}" "${files[@]}")
}

# tool_has_rows OP TOOL — true once a pooled row for TOOL-OP exists.
tool_has_rows() {
    local op="$1" tool="$2" f
    for f in "${HF_DIR}/${op}-${tool}"-*.json "${HF_DIR}/${op}-both"-*.json; do
        [[ -s "${f}" ]] && jq -e --arg c "${tool}-${op}" \
            'any(.results[]; (.name // .command) == $c)' "${f}" >/dev/null 2>&1 && return 0
    done
    return 1
}

# ---------------------------------------------------------------------------
# run_op OP DEN_CMD BREW_CMD — benchmark a read-only op for both tools,
# ROUNDS times, alternating which tool hyperfine runs first.
# An empty command string skips that tool.
# ---------------------------------------------------------------------------
run_op() {
    local op="$1" den_cmd="$2" brew_cmd="$3"
    echo "--- op: ${op} (${RUNS} runs x ${ROUNDS} rounds) ---"

    local round
    for ((round = 1; round <= ROUNDS; round++)); do
        local hf_args=(
            --runs "${RUNS}"
            --warmup 1
            --export-json "$(hf_out "${op}" both "${round}")"
            --style basic
            "${HF_UNIT_ARGS[@]+"${HF_UNIT_ARGS[@]}"}"
        )
        local den_args=() brew_args=()
        [[ -n "${den_cmd}" ]] && den_args=(--command-name "den-${op}" "${den_cmd}")
        [[ -n "${brew_cmd}" ]] && brew_args=(--command-name "brew-${op}" "${brew_cmd}")
        if (( round % 2 == 1 )); then
            hf_args+=("${den_args[@]+"${den_args[@]}"}" "${brew_args[@]+"${brew_args[@]}"}")
        else
            hf_args+=("${brew_args[@]+"${brew_args[@]}"}" "${den_args[@]+"${den_args[@]}"}")
        fi
        echo "  round ${round}/${ROUNDS}"
        if ! hyperfine "${hf_args[@]}"; then
            echo "  WARNING: hyperfine failed for op '${op}' round ${round}" >&2
            rm -f "$(hf_out "${op}" both "${round}")"
        fi
    done
    record_rounds "${op}" || true
}

# ---------------------------------------------------------------------------
# warm_install TOOL INSTALL_CMD UNINSTALL_CMD — one real install (the only
# network-touching step) to populate the download cache, then uninstall so the
# timed runs start clean. Records a skip and returns 1 on failure.
# ---------------------------------------------------------------------------
warm_install() {
    local tool="$1" install_cmd="$2" uninstall_cmd="$3"
    if ! eval "${install_cmd}" >/dev/null 2>&1; then
        record_skip "install" "${tool}" "warm-up install failed (offline or bottle unavailable)"
        return 1
    fi
    eval "${uninstall_cmd}" >/dev/null 2>&1 || true
}

# ---------------------------------------------------------------------------
# install_round TOOL INSTALL_CMD UNINSTALL_CMD ROUND — one hyperfine round of
# TOOL's install of TEST_PKG, uninstalling before each run (hyperfine
# --prepare) so every timed run is a clean, cache-warm install.
# ---------------------------------------------------------------------------
install_round() {
    local tool="$1" install_cmd="$2" uninstall_cmd="$3" round="$4"
    local out
    out="$(hf_out install "${tool}" "${round}")"
    if ! hyperfine \
        --runs "${INSTALL_RUNS}" \
        --prepare "${uninstall_cmd} >/dev/null 2>&1 || true" \
        --export-json "${out}" \
        --style basic \
        "${HF_UNIT_ARGS[@]+"${HF_UNIT_ARGS[@]}"}" \
        --command-name "${tool}-install" \
        "${install_cmd} >/dev/null 2>&1"; then
        echo "  WARNING: hyperfine failed for ${tool} install round ${round}" >&2
        rm -f "${out}"
    fi
    # Leave the package uninstalled for the next round / upgrade / cleanup.
    eval "${uninstall_cmd}" >/dev/null 2>&1 || true
}

# ---------------------------------------------------------------------------
# upgrade_round TOOL INSTALL_CMD UPGRADE_CMD UNINSTALL_CMD ROUND — install
# TEST_PKG once, time the no-op upgrade for one round, then uninstall.
# Returns 1 if the baseline install failed.
# ---------------------------------------------------------------------------
upgrade_round() {
    local tool="$1" install_cmd="$2" upgrade_cmd="$3" uninstall_cmd="$4" round="$5"
    if ! eval "${install_cmd}" >/dev/null 2>&1; then
        return 1
    fi
    local out
    out="$(hf_out upgrade "${tool}" "${round}")"
    if ! hyperfine \
        --runs "${RUNS}" \
        --warmup 1 \
        --export-json "${out}" \
        --style basic \
        "${HF_UNIT_ARGS[@]+"${HF_UNIT_ARGS[@]}"}" \
        --command-name "${tool}-upgrade" \
        "${upgrade_cmd} >/dev/null 2>&1"; then
        echo "  WARNING: hyperfine failed for ${tool} upgrade round ${round}" >&2
        rm -f "${out}"
    fi
    eval "${uninstall_cmd}" >/dev/null 2>&1 || true
}

# ---------------------------------------------------------------------------
# Prepare a fresh isolated DEN_HOME for the read-only ops.
# ---------------------------------------------------------------------------
DEN_HOME_TMP="$(mktemp -d /tmp/den-bench-home-XXXXXX)"

cleanup() {
    rm -rf "${DEN_HOME_TMP}" "${HF_DIR}"
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
        rm -rf "${DEN_HOME_TMP}" "${HF_DIR}"
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
        echo "--- op: install (pkg=${TEST_PKG}, runs=${INSTALL_RUNS} x ${ROUNDS} rounds) ---"
        echo "  WARNING: install/upgrade mutate the shared Cellar (cleaned up on exit)."

        DEN_INSTALL="${DEN_BIN} install ${TEST_PKG}"
        DEN_UNINSTALL="${DEN_BIN} uninstall ${TEST_PKG}"
        BREW_INSTALL="brew install ${TEST_PKG}"
        BREW_UNINSTALL="brew uninstall --force ${TEST_PKG}"

        # Ensure a clean starting point.
        eval "${DEN_UNINSTALL}" >/dev/null 2>&1 || true
        eval "${BREW_UNINSTALL}" >/dev/null 2>&1 || true

        DEN_OK=false; BREW_OK=false
        warm_install "den" "${DEN_INSTALL}" "${DEN_UNINSTALL}" && DEN_OK=true
        warm_install "brew" "${BREW_INSTALL}" "${BREW_UNINSTALL}" && BREW_OK=true

        for ((round = 1; round <= ROUNDS; round++)); do
            echo "  round ${round}/${ROUNDS}"
            if (( round % 2 == 1 )); then
                $DEN_OK && install_round "den" "${DEN_INSTALL}" "${DEN_UNINSTALL}" "${round}"
                $BREW_OK && install_round "brew" "${BREW_INSTALL}" "${BREW_UNINSTALL}" "${round}"
            else
                $BREW_OK && install_round "brew" "${BREW_INSTALL}" "${BREW_UNINSTALL}" "${round}"
                $DEN_OK && install_round "den" "${DEN_INSTALL}" "${DEN_UNINSTALL}" "${round}"
            fi
        done
        record_rounds "install" || true
        $DEN_OK && ! tool_has_rows install den && record_skip "install" "den" "hyperfine run failed"
        $BREW_OK && ! tool_has_rows install brew && record_skip "install" "brew" "hyperfine run failed"

        # -------------------------------------------------------------------
        # OP: upgrade
        # A true upgrade requires an older version installed and a newer one
        # available. den/brew almost always serve the latest version of a
        # leaf package, so a real version bump isn't deterministically
        # available. We benchmark the *no-op upgrade* path instead — the
        # common case a user hits when nothing is stale — which exercises the
        # same manifest/index/version-resolution machinery without a download.
        # Each round installs the package with one tool, times its no-op
        # upgrade, and uninstalls it again, alternating which tool goes first.
        # -------------------------------------------------------------------
        echo "--- op: upgrade (no-op path, pkg=${TEST_PKG}, runs=${RUNS} x ${ROUNDS} rounds) ---"

        DEN_UP_FAIL=0; BREW_UP_FAIL=0
        for ((round = 1; round <= ROUNDS; round++)); do
            echo "  round ${round}/${ROUNDS}"
            if (( round % 2 == 1 )); then order="den brew"; else order="brew den"; fi
            for tool in ${order}; do
                if [[ "${tool}" == "den" ]]; then
                    upgrade_round den "${DEN_INSTALL}" "${DEN_BIN} upgrade ${TEST_PKG}" \
                        "${DEN_UNINSTALL}" "${round}" || DEN_UP_FAIL=$((DEN_UP_FAIL + 1))
                else
                    upgrade_round brew "${BREW_INSTALL}" "brew upgrade ${TEST_PKG}" \
                        "${BREW_UNINSTALL}" "${round}" || BREW_UP_FAIL=$((BREW_UP_FAIL + 1))
                fi
            done
        done
        record_rounds "upgrade" || true
        if ! tool_has_rows upgrade den; then
            if [[ "${DEN_UP_FAIL}" -gt 0 ]]; then
                record_skip "upgrade" "den" "install for upgrade baseline failed"
            else
                record_skip "upgrade" "den" "hyperfine run failed"
            fi
        fi
        if ! tool_has_rows upgrade brew; then
            if [[ "${BREW_UP_FAIL}" -gt 0 ]]; then
                record_skip "upgrade" "brew" "install for upgrade baseline failed"
            else
                record_skip "upgrade" "brew" "hyperfine run failed"
            fi
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
    echo "=== Summary (latency, milliseconds) ==="
    printf "%-14s %-10s %10s %10s %6s\n" "tool" "op" "median_ms" "mean_ms" "runs"
    printf "%-14s %-10s %10s %10s %6s\n" "----" "--" "---------" "-------" "----"
    jq -r '.[] | [.tool, .op,
                  (if .median_s == null then "skipped" else (.median_s * 1000 | floor | tostring) end),
                  (if .mean_s == null then "skipped" else (.mean_s * 1000 | floor | tostring) end),
                  ((.runs // 0) | tostring)] | @tsv' \
        "${RESULT_JSON}" 2>/dev/null \
        | while IFS=$'\t' read -r tool op median mean runs; do
            printf "%-14s %-10s %10s %10s %6s\n" "${tool}" "${op}" "${median}" "${mean}" "${runs}"
        done
fi

echo ""
echo "Done. Compare against baselines with: scripts/bench/compare-results.sh"

#!/usr/bin/env bash
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# compare-results.sh — evaluate a den-vs-brew benchmark snapshot against the
# T68 acceptance contract, and (optionally) detect regressions vs a prior run.
#
# Requires: jq
#
# Usage:
#   scripts/bench/compare-results.sh [SNAPSHOT.json] [--baseline PRIOR.json]
#                                    [--max-regression PCT]
#
#   SNAPSHOT.json        The run to evaluate. Default: newest bench-*.json in
#                        bench/results/.
#   --baseline PRIOR     A previous snapshot to compare against for regression
#                        detection. Default: second-newest in bench/results/,
#                        if one exists.
#   --max-regression P   Fail if any den op is more than P% slower than the
#                        baseline (default: 25).
#
# Contract checked (🎯T68):
#   1. den is at least as fast as brew on every measured op.
#   2. At least one op is "meaningfully" faster (>= MEANINGFUL_FACTOR x).
#   3. No measured den op regressed by more than --max-regression vs baseline.
#
# Exit status: 0 if the contract holds, 1 otherwise.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RESULTS_DIR="${REPO_ROOT}/bench/results"

# A speedup is "meaningful" when den is at least this many times faster.
MEANINGFUL_FACTOR="1.5"
MAX_REGRESSION_PCT="25"

SNAPSHOT=""
BASELINE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --baseline)       BASELINE="$2";          shift 2 ;;
        --max-regression) MAX_REGRESSION_PCT="$2"; shift 2 ;;
        -*)               echo "Unknown option: $1" >&2; exit 1 ;;
        *)                SNAPSHOT="$1";           shift ;;
    esac
done

if ! command -v jq &>/dev/null; then
    echo "ERROR: jq not found. Install with: brew install jq" >&2
    exit 1
fi

# Pick newest snapshot if none given.
if [[ -z "${SNAPSHOT}" ]]; then
    SNAPSHOT="$(ls -1 "${RESULTS_DIR}"/bench-*.json 2>/dev/null | sort | tail -1 || true)"
fi
if [[ -z "${SNAPSHOT}" || ! -f "${SNAPSHOT}" ]]; then
    echo "ERROR: no snapshot found. Run scripts/bench/compare-brew.sh first." >&2
    exit 1
fi

# Pick second-newest as baseline if none given (best effort).
if [[ -z "${BASELINE}" ]]; then
    BASELINE="$(ls -1 "${RESULTS_DIR}"/bench-*.json 2>/dev/null \
        | grep -v -F "$(basename "${SNAPSHOT}")" | sort | tail -1 || true)"
fi

echo "=== T68 benchmark contract check ==="
echo "snapshot: ${SNAPSHOT}"
[[ -n "${BASELINE}" && -f "${BASELINE}" ]] && echo "baseline: ${BASELINE}" \
                                          || echo "baseline: (none — regression check skipped)"
echo ""

# ---------------------------------------------------------------------------
# Build a map of op -> {den_mean, brew_mean} for measured (non-null) rows.
# ---------------------------------------------------------------------------
# Emits TSV lines: op<TAB>den_mean<TAB>brew_mean   (only ops with both means)
pair_means() {
    local file="$1"
    jq -r '
        [ .[] | select(.mean_s != null) ] as $rows
        | ($rows | map(.op) | unique) as $ops
        | $ops[]
        | . as $op
        | ($rows | map(select(.op == $op and (.tool | startswith("den")))) | .[0].mean_s) as $den
        | ($rows | map(select(.op == $op and (.tool | startswith("brew")))) | .[0].mean_s) as $brew
        | select($den != null and $brew != null)
        | [$op, ($den|tostring), ($brew|tostring)] | @tsv
    ' "${file}"
}

PASS=true
MEANINGFUL_COUNT=0
MEASURED_COUNT=0

echo "--- den vs brew (per op) ---"
printf "%-10s %12s %12s %10s   %s\n" "op" "den_ms" "brew_ms" "speedup" "verdict"
printf "%-10s %12s %12s %10s   %s\n" "--" "------" "-------" "-------" "-------"

while IFS=$'\t' read -r op den brew; do
    [[ -z "${op}" ]] && continue
    MEASURED_COUNT=$((MEASURED_COUNT + 1))
    den_ms="$(awk -v v="${den}" 'BEGIN{printf "%.1f", v*1000}')"
    brew_ms="$(awk -v v="${brew}" 'BEGIN{printf "%.1f", v*1000}')"
    speedup="$(awk -v b="${brew}" -v d="${den}" 'BEGIN{ if (d>0) printf "%.2f", b/d; else print "inf" }')"

    verdict="ok"
    # den must be at least as fast as brew (allow a tiny 2% slack for noise).
    if awk -v b="${brew}" -v d="${den}" 'BEGIN{ exit !(d > b*1.02) }'; then
        verdict="SLOWER-THAN-BREW"
        PASS=false
    fi
    if awk -v s="${speedup}" -v f="${MEANINGFUL_FACTOR}" 'BEGIN{ exit !(s+0 >= f+0) }'; then
        MEANINGFUL_COUNT=$((MEANINGFUL_COUNT + 1))
    fi
    printf "%-10s %12s %12s %9sx   %s\n" "${op}" "${den_ms}" "${brew_ms}" "${speedup}" "${verdict}"
done < <(pair_means "${SNAPSHOT}")

echo ""

if [[ "${MEASURED_COUNT}" -eq 0 ]]; then
    echo "FAIL: no op had both den and brew measurements." >&2
    exit 1
fi

# Contract 1: den >= brew on every measured op.
if $PASS; then
    echo "PASS: den is at least as fast as brew on all ${MEASURED_COUNT} measured op(s)."
else
    echo "FAIL: den is slower than brew on at least one op."
fi

# Contract 2: at least one meaningfully-faster op.
if [[ "${MEANINGFUL_COUNT}" -ge 1 ]]; then
    echo "PASS: ${MEANINGFUL_COUNT} op(s) are meaningfully faster (>= ${MEANINGFUL_FACTOR}x)."
else
    echo "FAIL: no op is meaningfully faster (>= ${MEANINGFUL_FACTOR}x)."
    PASS=false
fi

# ---------------------------------------------------------------------------
# Contract 3: regression vs baseline (if available).
# ---------------------------------------------------------------------------
if [[ -n "${BASELINE}" && -f "${BASELINE}" ]]; then
    echo ""
    echo "--- regression vs baseline (den only) ---"
    printf "%-10s %12s %12s %10s   %s\n" "op" "base_ms" "now_ms" "delta" "verdict"
    printf "%-10s %12s %12s %10s   %s\n" "--" "-------" "------" "-----" "-------"

    # Map op -> den_mean for both files.
    den_mean_for() {
        jq -r --arg op "$2" '
            [ .[] | select(.op == $op and (.tool|startswith("den")) and .mean_s != null) ]
            | .[0].mean_s // empty
        ' "$1"
    }

    while IFS=$'\t' read -r op _den _brew; do
        [[ -z "${op}" ]] && continue
        base="$(den_mean_for "${BASELINE}" "${op}")"
        now="$(den_mean_for "${SNAPSHOT}" "${op}")"
        [[ -z "${base}" || -z "${now}" ]] && continue
        base_ms="$(awk -v v="${base}" 'BEGIN{printf "%.1f", v*1000}')"
        now_ms="$(awk -v v="${now}" 'BEGIN{printf "%.1f", v*1000}')"
        delta="$(awk -v b="${base}" -v n="${now}" 'BEGIN{ if (b>0) printf "%+.1f%%", (n-b)/b*100; else print "n/a" }')"
        verdict="ok"
        if awk -v b="${base}" -v n="${now}" -v p="${MAX_REGRESSION_PCT}" \
            'BEGIN{ exit !(b>0 && (n-b)/b*100 > p) }'; then
            verdict="REGRESSED"
            PASS=false
        fi
        printf "%-10s %12s %12s %10s   %s\n" "${op}" "${base_ms}" "${now_ms}" "${delta}" "${verdict}"
    done < <(pair_means "${SNAPSHOT}")
fi

echo ""
if $PASS; then
    echo "RESULT: PASS — T68 contract holds."
    exit 0
else
    echo "RESULT: FAIL — T68 contract violated."
    exit 1
fi

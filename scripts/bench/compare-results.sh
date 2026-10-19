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
#                                    [--max-aggregate-regression PCT]
#
#   SNAPSHOT.json        The run to evaluate. Default: newest bench-*.json in
#                        bench/results/.
#   --baseline PRIOR     A previous snapshot to compare against for regression
#                        detection. Default: newest same-host snapshot in
#                        bench/results/, if one exists.
#   --max-regression P   Fail if any single op's den/brew time ratio grew by
#                        more than P% vs the baseline (default: 75).
#   --max-aggregate-regression P
#                        Fail if the geometric mean of the per-op ratio
#                        changes grew by more than P% (default: 30).
#
# Contract checked (🎯T68):
#   1. den is at least as fast as brew on every measured op.
#   2. At least one op is "meaningfully" faster (>= MEANINGFUL_FACTOR x).
#   3. den did not lose ground to brew vs a same-host baseline. Per op the
#      gate compares ratio = den_ms / brew_ms now against the baseline's
#      ratio, so a slower (or faster) GitHub runner, which moves den and
#      brew together, cancels out. An op regresses when its ratio grew by
#      more than --max-regression *and* den's own time grew by more than
#      MIN_ABS_REGRESSION_S. Separately, the geometric mean of the ratio
#      changes across all checked ops must not grow by more than
#      --max-aggregate-regression; a uniform den-only slowdown (e.g. a new
#      fixed startup cost) shows up there well before any single op trips.
#      Tiny ops (den under TINY_OP_S in both runs, i.e. `list`) are reported
#      but excluded from the ratio gate. Cross-host comparisons are skipped.
#
# Tolerances (derived from the 13 weekly gha-macos14 runs, 27 Jul–19 Oct
# 2026, run ids 30253711792..39244425261, ignoring the 7 Sep step where
# brew itself got ~2x faster):
#   - Per-op ratio: consecutive-week changes have sd 14–20% and reach +35%;
#     because failing runs are not committed the baseline can be weeks old,
#     and across every pair since 7 Sep the worst noise is +70% (install,
#     whose brew side has 14% in-run stddev). 75% clears that while still
#     failing any op that roughly doubles relative to brew.
#   - Aggregate (geomean of info/search/install/upgrade ratio changes):
#     sd 10.3%, worst +18.7% over all pairs — about half the noise of raw
#     den ms (sd 18.9%, worst +44%, the 19 Oct false failure). 30% is ~3 sd.
#   - TINY_OP_S = 20 ms: `list` den means span 0.2–15.7 ms (in-run stddev
#     23% den / 34% brew; ratio swings -94%..+3060% week to week) while
#     every other op is >= 180 ms. hyperfine cannot calibrate shell startup
#     below ~5 ms, so those ratios are noise.
#
# Exit status: 0 if the contract holds, 1 otherwise.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RESULTS_DIR="${REPO_ROOT}/bench/results"

# A speedup is "meaningful" when den is at least this many times faster.
MEANINGFUL_FACTOR="1.5"
# Per-op den/brew ratio growth (%) that counts as a regression.
MAX_REGRESSION_PCT="75"
# Geomean (across checked ops) den/brew ratio growth (%) that counts as a
# regression.
MAX_AGG_REGRESSION_PCT="30"
# An op's ratio regression must also be backed by den itself getting at
# least this much slower in absolute terms (seconds).
MIN_ABS_REGRESSION_S="0.025"
# Ops where den's mean is below this in both runs are too small to compare
# by ratio (seconds).
TINY_OP_S="0.020"

SNAPSHOT=""
BASELINE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --baseline)       BASELINE="$2";          shift 2 ;;
        --max-regression) MAX_REGRESSION_PCT="$2"; shift 2 ;;
        --max-aggregate-regression) MAX_AGG_REGRESSION_PCT="$2"; shift 2 ;;
        -*)               echo "Unknown option: $1" >&2; exit 1 ;;
        *)                SNAPSHOT="$1";           shift ;;
    esac
done

if ! command -v jq &>/dev/null; then
    echo "ERROR: jq not found. Install with: brew install jq" >&2
    exit 1
fi

# Host id recorded on each result row by compare-brew.sh. Older snapshots
# without a host field compare as "unknown"; "unknown" only matches other
# "unknown" files, so a new host-tagged CI run will not treat an untagged
# local baseline as comparable.
snapshot_host() {
    local file="$1"
    jq -r '
        if type == "array" then
            ([.[] | .host // empty] | first) // "unknown"
        else
            .host // "unknown"
        end
    ' "${file}"
}

# Pick newest snapshot if none given.
if [[ -z "${SNAPSHOT}" ]]; then
    SNAPSHOT="$(ls -1 "${RESULTS_DIR}"/bench-*.json 2>/dev/null | sort | tail -1 || true)"
fi
if [[ -z "${SNAPSHOT}" || ! -f "${SNAPSHOT}" ]]; then
    echo "ERROR: no snapshot found. Run scripts/bench/compare-brew.sh first." >&2
    exit 1
fi

SNAP_HOST="$(snapshot_host "${SNAPSHOT}")"

# Pick newest same-host snapshot as baseline if none given. Comparing
# across host classes (local M4 Max vs gha-macos14) is what made the
# weekly bench.yml job fail every week after the first local baseline
# was committed: GHA runners are slower in absolute ms, even when den
# still beats brew.
if [[ -z "${BASELINE}" ]]; then
    while IFS= read -r candidate; do
        [[ -z "${candidate}" ]] && continue
        if [[ "$(snapshot_host "${candidate}")" == "${SNAP_HOST}" ]]; then
            BASELINE="${candidate}"
            break
        fi
    done < <(ls -1 "${RESULTS_DIR}"/bench-*.json 2>/dev/null \
        | grep -v -F "$(basename "${SNAPSHOT}")" | sort -r || true)
fi

# An explicitly-passed baseline from another host is not a regression
# signal — drop it rather than fail the T68 contract on hardware noise.
if [[ -n "${BASELINE}" && -f "${BASELINE}" ]]; then
    BASE_HOST="$(snapshot_host "${BASELINE}")"
    if [[ "${BASE_HOST}" != "${SNAP_HOST}" ]]; then
        echo "WARNING: baseline host '${BASE_HOST}' != snapshot host '${SNAP_HOST}'" >&2
        echo "         skipping regression check (cross-host times are not comparable)" >&2
        BASELINE=""
    fi
fi

echo "=== T68 benchmark contract check ==="
echo "snapshot: ${SNAPSHOT}  (host: ${SNAP_HOST})"
if [[ -n "${BASELINE}" && -f "${BASELINE}" ]]; then
    echo "baseline: ${BASELINE}  (host: $(snapshot_host "${BASELINE}"))"
else
    echo "baseline: (none — no same-host snapshot; regression check skipped)"
fi
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
# Contract 3: den/brew ratio regression vs baseline (if available).
# ---------------------------------------------------------------------------
if [[ -n "${BASELINE}" && -f "${BASELINE}" ]]; then
    echo ""
    echo "--- regression vs baseline (den/brew ratio; per-op limit +${MAX_REGRESSION_PCT}%, aggregate limit +${MAX_AGG_REGRESSION_PCT}%) ---"
    printf "%-10s %18s %18s %10s %10s   %s\n" "op" "base den/brew" "now den/brew" "ratio_chg" "den_dms" "verdict"
    printf "%-10s %18s %18s %10s %10s   %s\n" "--" "-------------" "------------" "---------" "-------" "-------"

    # Baseline op<TAB>den<TAB>brew rows (looked up per op with awk, not an
    # associative array: macOS /bin/bash is 3.2).
    BASE_PAIRS="$(pair_means "${BASELINE}")"

    LOG_SUM="0"
    CHECKED=0
    while IFS=$'\t' read -r op now_den now_brew; do
        [[ -z "${op}" ]] && continue
        base_row="$(awk -F'\t' -v op="${op}" '$1 == op { print $2 "\t" $3; exit }' <<<"${BASE_PAIRS}")"
        [[ -z "${base_row}" ]] && continue
        IFS=$'\t' read -r base_den base_brew <<<"${base_row}"

        # One awk call computes the row: ratios, deltas, and the verdict.
        IFS=$'\t' read -r base_r now_r ratio_d den_d verdict logr <<<"$(awk \
            -v bd="${base_den}" -v bb="${base_brew}" \
            -v nd="${now_den}" -v nb="${now_brew}" \
            -v p="${MAX_REGRESSION_PCT}" -v a="${MIN_ABS_REGRESSION_S}" \
            -v tiny="${TINY_OP_S}" '
            BEGIN {
                if (bd <= 0 || bb <= 0 || nd <= 0 || nb <= 0) {
                    printf "n/a\tn/a\tn/a\tn/a\tskip (zero time)\t-\n"; exit
                }
                br = bd / bb; nr = nd / nb
                ch = (nr / br - 1) * 100
                dd = (nd - bd) * 1000
                v = "ok"; lr = sprintf("%.12f", log(nr / br))
                if (bd < tiny + 0 && nd < tiny + 0) {
                    v = sprintf("skip (tiny, den < %d ms)", tiny * 1000); lr = "-"
                } else if (ch > p + 0 && (nd - bd) > a + 0) {
                    v = "REGRESSED"
                }
                printf "%.3f (%.0f/%.0f)\t%.3f (%.0f/%.0f)\t%+.1f%%\t%+.1f\t%s\t%s\n",
                    br, bd*1000, bb*1000, nr, nd*1000, nb*1000, ch, dd, v, lr
            }')"

        if [[ "${verdict}" == "REGRESSED" ]]; then
            PASS=false
        fi
        if [[ "${logr}" != "-" ]]; then
            LOG_SUM="$(awk -v s="${LOG_SUM}" -v l="${logr}" 'BEGIN{printf "%.12f", s + l}')"
            CHECKED=$((CHECKED + 1))
        fi
        printf "%-10s %18s %18s %10s %10s   %s\n" "${op}" "${base_r}" "${now_r}" "${ratio_d}" "${den_d}" "${verdict}"
    done < <(pair_means "${SNAPSHOT}")

    echo ""
    if [[ "${CHECKED}" -gt 0 ]]; then
        agg="$(awk -v s="${LOG_SUM}" -v n="${CHECKED}" 'BEGIN{printf "%+.1f", (exp(s/n)-1)*100}')"
        if awk -v g="${agg}" -v p="${MAX_AGG_REGRESSION_PCT}" 'BEGIN{ exit !(g+0 > p+0) }'; then
            echo "aggregate: geomean den/brew ratio change over ${CHECKED} op(s) = ${agg}%  REGRESSED (limit +${MAX_AGG_REGRESSION_PCT}%)"
            PASS=false
        else
            echo "aggregate: geomean den/brew ratio change over ${CHECKED} op(s) = ${agg}%  ok (limit +${MAX_AGG_REGRESSION_PCT}%)"
        fi
    else
        echo "aggregate: no op large enough for a ratio comparison; skipped"
    fi
fi

echo ""
if $PASS; then
    echo "RESULT: PASS — T68 contract holds."
    exit 0
else
    echo "RESULT: FAIL — T68 contract violated."
    exit 1
fi

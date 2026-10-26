#!/usr/bin/env bash
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# merge-hyperfine.sh — pool the per-run times from one or more hyperfine
# --export-json files (one per round) into den bench snapshot rows.
#
# Requires: jq
#
# Usage:
#   scripts/bench/merge-hyperfine.sh OP HOST TIMESTAMP ROUND.json [ROUND.json ...]
#
# Prints one compact JSON object per command (hyperfine --command-name, e.g.
# "den-info"), pooling every round's per-run wall-clock times. Reads both
# hyperfine 1.x exports (.results[].times) and 2.x schema_version 2 exports
# (.results[].measurements[].time_wall_clock.value):
#
#   {"timestamp","host","op","tool","mean_s","stddev_s","median_s","min_s",
#    "max_s","runs","rounds","times_s":[...],"status":"ok"}
#
# median_s is what compare-results.sh gates on. A single slow run (GitHub
# runner stall, page-cache miss) shifts the mean of 10 runs by 10% of its
# excess; it does not move the median unless half the runs are slow.
# times_s keeps the raw samples so a snapshot can be re-analysed later.

set -euo pipefail

if [[ $# -lt 4 ]]; then
    echo "usage: $0 OP HOST TIMESTAMP ROUND.json [ROUND.json ...]" >&2
    exit 2
fi

OP="$1"; HOST="$2"; TS="$3"; shift 3

jq -s -c --arg op "${OP}" --arg host "${HOST}" --arg ts "${TS}" '
    # hyperfine 1.x: .command is the --command-name, samples in .times.
    # hyperfine 2.x (schema_version 2): .name is the --command-name,
    # .command the shell string, samples in .measurements[].time_wall_clock.
    [ .[] | .results[]
      | { tool: (.name // .command),
          times: (.times // [ .measurements[]? | .time_wall_clock.value? // empty ]) } ]
    | group_by(.tool)
    | sort_by(if (.[0].tool | startswith("den")) then 0 else 1 end)
    | .[]
    | (map(.times) | add) as $t
    | ($t | length) as $n
    | select($n > 0)
    | ($t | sort) as $s
    | ($t | add / $n) as $mean
    | (if $n % 2 == 1 then $s[($n - 1) / 2 | floor]
       else ($s[$n / 2 - 1 | floor] + $s[$n / 2 | floor]) / 2 end) as $median
    | (if $n > 1 then ($t | map((. - $mean) * (. - $mean)) | add / ($n - 1) | sqrt)
       else 0 end) as $sd
    | {timestamp: $ts, host: $host, op: $op, tool: .[0].tool,
       mean_s: $mean, stddev_s: $sd, median_s: $median,
       min_s: $s[0], max_s: $s[-1], runs: $n, rounds: length,
       times_s: $t, status: "ok"}
' "$@"

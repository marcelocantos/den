# den vs brew benchmark suite

Measures wall-clock performance of `den` against `brew` on the five operations
that matter most to a user: `list`, `info`, `search`, `install`, and `upgrade`.

## Prerequisites

| Tool       | Install                  | Purpose                     |
|------------|--------------------------|-----------------------------|
| `hyperfine`| `brew install hyperfine` | Statistical micro-benchmarking |
| `jq`       | `brew install jq`        | Parsing hyperfine JSON output |
| `brew`     | https://brew.sh          | Reference baseline           |

`den` itself must be built first:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Running

```bash
# Quick run with defaults (10 repetitions per op):
scripts/bench/compare-brew.sh

# Customise output directory and repetition count:
scripts/bench/compare-brew.sh --output-dir /tmp/bench-out --runs 5

# All options:
scripts/bench/compare-brew.sh \
  --output-dir bench/results   \   # where to write JSON/CSV (default: bench/results/)
  --runs 10                    \   # hyperfine repetitions (default: 10)
  --json                           # (reserved; output is always JSON+CSV)
```

## What is measured

| Op       | den command           | brew command     | Automated? |
|----------|-----------------------|------------------|------------|
| `list`   | `den list`            | `brew list`      | Yes        |
| `info`   | `den info jq`         | `brew info jq`   | Yes*       |
| `search` | `den search json`     | `brew search json` | Yes*     |
| `install`| `den install jq`      | `brew install jq`| No (network) |
| `upgrade`| `den upgrade jq`      | `brew upgrade jq`| No (network) |

\* Requires `den update` to have been run at least once so the local index
exists. The script copies your existing index into the isolated benchmark
`DEN_HOME` automatically.

`install` and `upgrade` are excluded from the automated run because they
require network access, modify system state, and take variable time depending
on mirror speed. To benchmark them manually:

```bash
# install
hyperfine --runs 3 \
  'den install jq' \
  'brew install jq'

# upgrade (install an older version first)
hyperfine --runs 3 \
  'den upgrade jq' \
  'brew upgrade jq'
```

## Output

Results land in `bench/results/` as two files per run:

```
bench/results/bench-20260517T120000Z.json
bench/results/bench-20260517T120000Z.csv
```

The JSON schema per entry:

```json
{
  "timestamp": "20260517T120000Z",
  "op":        "list",
  "tool":      "den-list",
  "mean_s":    0.042,
  "stddev_s":  0.003,
  "min_s":     0.039,
  "max_s":     0.051,
  "status":    "ok"
}
```

The CSV mirrors the same fields for easy import into spreadsheets / pandas.

## Tracking regressions over time

Result files are committed to `bench/results/` as snapshots on each release.
To compare two snapshots:

```bash
jq -r '.[] | [.tool, .op, .mean_s] | @tsv' bench/results/bench-20260510T*.json \
  bench/results/bench-20260517T*.json \
  | sort | column -t
```

Or load both CSVs into pandas / Excel and pivot on `timestamp`.

## Current status (T68)

- `list`, `info`, `search`: benchmarked end-to-end on every run.
- `install`, `upgrade`: skipped in automated mode (network-dependent).
  Upstream gap: `den install` and `den upgrade` both require `den update`
  (index fetch) and a working bottle download pipeline (🎯T19).

## CI integration

A scheduled GitHub Actions workflow runs the automated ops weekly:
`.github/workflows/bench.yml`

Results are uploaded as workflow artifacts and preserved for 90 days.

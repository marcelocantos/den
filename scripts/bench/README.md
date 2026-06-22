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
# Read-only ops only (list/info/search) — never touches system state:
scripts/bench/compare-brew.sh

# All five ops, including install/upgrade (mutates the shared Cellar, but
# uninstalls the test package on exit):
scripts/bench/compare-brew.sh --with-install

# All options:
scripts/bench/compare-brew.sh \
  --output-dir bench/results   \   # where to write JSON/CSV (default: bench/results/)
  --runs 10                    \   # hyperfine reps for read-only ops (default: 10)
  --install-runs 5             \   # hyperfine reps for install/upgrade (default: 3)
  --with-install               \   # enable install + upgrade benchmarks
  --pkg hello                  \   # package used for install/upgrade (default: hello)
  --json                           # (reserved; output is always JSON+CSV)
```

## What is measured

| Op       | den command           | brew command       | Default run |
|----------|-----------------------|--------------------|-------------|
| `list`   | `den list`            | `brew list`        | Yes         |
| `info`   | `den info hello`      | `brew info hello`  | Yes*        |
| `search` | `den search json`     | `brew search json` | Yes*        |
| `install`| `den install hello`   | `brew install hello` | `--with-install` |
| `upgrade`| `den upgrade hello`   | `brew upgrade hello` | `--with-install` |

\* Requires `den update` to have been run at least once so the local index
exists. The script copies your existing index into the isolated benchmark
`DEN_HOME` automatically.

### install / upgrade methodology

`install` and `upgrade` touch the network and mutate the shared Cellar, so
they are gated behind `--with-install` and made deterministic as follows:

- The download cache is warmed **once** up front (the only network step). If
  warm-up fails (offline, bottle unavailable), the op degrades to `SKIPPED`
  and the run still succeeds.
- Each timed `install` run is preceded by an uninstall (hyperfine
  `--prepare`), so it measures a clean, cache-warm install rather than a
  no-op re-install.
- `upgrade` measures the common **no-op upgrade path** (package already
  current): the same manifest/index/version-resolution machinery a user hits
  when nothing is stale, without a non-deterministic version bump.
- The test package (`hello` by default — tiny, no deps) is uninstalled from
  both tools on exit, restoring the Cellar.

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

Result files are committed to `bench/results/` as snapshots. The CI workflow
appends a new snapshot on every run, so a trend accrues across releases.

`compare-results.sh` evaluates a snapshot against the 🎯T68 contract and, when
a prior snapshot exists, flags regressions:

```bash
# Evaluate the newest snapshot against the previous one (auto-discovered):
scripts/bench/compare-results.sh

# Explicit snapshot + baseline + tighter regression threshold:
scripts/bench/compare-results.sh bench/results/bench-NEW.json \
  --baseline bench/results/bench-OLD.json \
  --max-regression 15
```

It exits non-zero (failing CI) if den is no longer at least as fast as brew on
every op, if no op is meaningfully faster, or if a den op regressed beyond the
threshold.

## Current status (🎯T68 — satisfied)

- All five ops (`list`, `info`, `search`, `install`, `upgrade`) are measured
  against a fresh Homebrew baseline on the same hardware.
- A first baseline is committed under `bench/results/`; CI appends a new
  snapshot each run so the "recorded over time" criterion accrues.
- On the reference machine (M4 Max, macOS 26) den is faster than brew on
  **every** op, with `install` the standout (~20-30x faster, cache-warm).

## CI integration

`.github/workflows/bench.yml` runs weekly (and on demand). It builds den,
runs all five ops with `--with-install`, checks the T68 contract, commits the
snapshot back to `bench/results/` on `master`, and uploads the results as an
artifact (90-day retention).

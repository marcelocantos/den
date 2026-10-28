# den vs brew benchmark suite

Measures wall-clock performance of `den` against `brew` on the five operations
that matter most to a user: `list`, `info`, `search`, `install`, and `upgrade`.

## Prerequisites

| Tool       | Install                  | Purpose                     |
|------------|--------------------------|-----------------------------|
| `hyperfine`| `brew install hyperfine` | Statistical micro-benchmarking (1.x or 2.x) |
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
  --runs 10                    \   # hyperfine reps per round, read-only ops (default: 10)
  --install-runs 5             \   # hyperfine reps per round, install/upgrade (default: 3)
  --rounds 3                   \   # rounds per op, pooled (default: 3, or $DEN_BENCH_ROUNDS)
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

### Rounds and medians

Each op is timed in `--rounds` separate hyperfine invocations (default 3), so
the CI workflow's `--runs 10 --install-runs 5` gives 30 runs per tool per
op (15 for install). Rounds alternate which tool goes first (den first on
odd rounds, brew first on even ones), so a slow patch on the runner hits both
tools rather than whichever happened to run during it. Install and upgrade
rounds each reset the package state themselves.

`merge-hyperfine.sh` pools every round's per-run times into one row per tool
and records the **median** alongside the mean. The regression gate compares
medians: a few runner stalls (the 26 Oct run had den `info` at 338 ± 65 ms,
range 272–450 ms) drag a mean of 10 runs a long way, but do not move a median
of 30 unless nearly half the runs stall. A real regression moves every run,
so it moves the median too.

hyperfine 2.0 removed `--time-unit` and changed its JSON export
(`.results[].name`, `.measurements[].time_wall_clock.value`). The script
detects the installed version and reads both formats.

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
  "host":      "Darwin-arm64-local",
  "op":        "list",
  "tool":      "den-list",
  "mean_s":    0.042,
  "stddev_s":  0.003,
  "min_s":     0.039,
  "max_s":     0.051,
  "median_s":  0.041,
  "runs":      30,
  "rounds":    3,
  "times_s":   [0.041, 0.040, "..."],
  "status":    "ok"
}
```

`median_s`, `runs`, `rounds` and `times_s` (the raw per-run times, so a
snapshot can be re-analysed later) are absent from snapshots taken before
the median gate.

`host` is a stable machine class (`Darwin-arm64-local` on a developer
machine, `gha-macos15-ARM64` on GitHub Actions macos-15). Snapshots up to
Oct 2026 are `gha-macos14-ARM64`, from before the macOS 14 runner image
was retired. Regression checks only compare snapshots from the same host
class — a local M4 Max baseline must not fail CI on a slower shared
runner, and the first macos-15 run has no baseline yet.

The CSV mirrors the same fields for easy import into spreadsheets / pandas.

## Tracking regressions over time

Result files are committed to `bench/results/` as snapshots. The CI workflow
appends a new snapshot on every run, so a trend accrues across releases.

`compare-results.sh` evaluates a snapshot against the 🎯T68 contract and, when
a prior same-host snapshot exists, flags regressions:

```bash
# Evaluate the newest snapshot against the previous same-host one:
scripts/bench/compare-results.sh

# Explicit snapshot + baseline + custom tolerances:
scripts/bench/compare-results.sh bench/results/bench-NEW.json \
  --baseline bench/results/bench-OLD.json \
  --max-regression 50 --max-aggregate-regression 20
```

It exits non-zero (failing CI) if den is no longer at least as fast as brew on
every op, if no op is meaningfully faster (>= 1.5x), or if den lost ground to
brew against a **same-host** baseline. If no prior snapshot shares the current
host class, the regression clause is skipped (den-vs-brew still gates) so a
first-on-this-runner run can commit a CI baseline.

The regression clause compares **den/brew time ratios**, not raw den ms. GitHub
runners vary from week to week by much more than any change we would want to
catch: run 39244425261 measured every den op 38–51% slower than the 12 Oct
snapshot while brew slowed by the same amount, and den's ratio to brew stayed
flat. A ratio cancels that out. Per op, `ratio = den_ms / brew_ms`:

- **Per op:** an op regresses when its ratio grew by more than 75%
  (`--max-regression`) *and* den itself got more than 25 ms slower.
- **Aggregate:** the geometric mean of the per-op ratio changes must not grow
  by more than 30% (`--max-aggregate-regression`). A uniform den-only slowdown,
  such as a new fixed startup cost, shows up here before any single op trips.
- **Tiny ops:** an op whose den mean is under 20 ms in both runs (in practice
  `list`) is shown but left out of both checks. hyperfine cannot calibrate
  shell startup below ~5 ms, and `list` ratios have swung from -94% to +3060%
  between weekly runs.

The tolerances come from the 13 weekly gha-macos14 runs between 27 Jul and
19 Oct 2026, leaving out the 7 Sep step where brew itself got about 2x faster:

- Per-op ratio changes between consecutive weeks have an sd of 14–20% and
  reach +35%. Failed runs are not committed, so the baseline can be several
  weeks old; across every pair of runs since 7 Sep the worst change is +70%
  (install, where brew's own run-to-run stddev is 14%).
- The aggregate has an sd of 10.3% and a worst case of +18.7%, about half the
  noise of raw den ms (sd 18.9%, worst +44%).
- Replaying all 36 same-era run pairs, the old raw-ms gate fails 12 and the
  ratio gate fails none.

**Statistic:** ratios use per-op medians when both the snapshot and the
baseline have them. If either lacks them (any snapshot before the median
gate, including the 12 Oct baseline), both sides are compared on means, and
the output says so: a median against an old mean would favour the new run.
The first median snapshot committed to `bench/results/` becomes the first
median baseline.

Known limit: because the gate is relative to brew, a big brew speedup (like
7 Sep) shrinks den's lead and can trip it even when den did not change. That
is still a real change in the T68 comparison; refresh the baseline once it
has been reviewed.

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

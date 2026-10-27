#!/usr/bin/env python3
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Regenerates the noisy-*.json bench fixtures (median gate, see
# compare-results.sh). Each tool gets 3 rounds x 10 runs (install: 3 x 5)
# of synthetic per-run times, written as hyperfine 1.x round exports and
# pooled through scripts/bench/merge-hyperfine.sh, exactly as
# compare-brew.sh does on CI.
#
# Centres and spreads are the 12 Oct gha-macos14 baseline means/stddevs
# (run 38257565888). Deterministic: seeded RNG, no network.
#
#   python3 tests/fixtures/bench/gen-noisy-fixtures.py
import json, os, random, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
MERGE = os.path.join(ROOT, "scripts", "bench", "merge-hyperfine.sh")
HOST = "gha-macos14-ARM64"

# (op, mean_s, stddev_s) for den and brew from the 12 Oct baseline.
BASE = {
    "list":    ((0.0069, 0.0015), (0.0216, 0.0040)),
    "info":    ((0.2382, 0.0243), (0.5074, 0.0448)),
    "search":  ((0.2367, 0.0226), (0.4984, 0.0339)),
    "install": ((0.2483, 0.0213), (0.9569, 0.0979)),
    "upgrade": ((0.2352, 0.0209), (0.4395, 0.0463)),
}
OPS = ["list", "info", "search", "install", "upgrade"]


def samples(rng, mean, sd, n):
    return [max(0.001, rng.gauss(mean, sd)) for _ in range(n)]


def build(name, stamp, tweak):
    rng = random.Random(68)
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for op in OPS:
            per_round = 5 if op == "install" else 10
            n = 3 * per_round
            times = {}
            for tool, (m, s) in zip(("den", "brew"), BASE[op]):
                times[tool] = samples(rng, m, s, n)
            tweak(op, times)
            files = []
            for r in range(3):
                res = [{"command": f"{t}-{op}", "times": times[t][r * per_round:(r + 1) * per_round]}
                       for t in ("den", "brew")]
                f = os.path.join(tmp, f"{op}-{r}.json")
                with open(f, "w") as fh:
                    json.dump({"results": res}, fh)
                files.append(f)
            out = subprocess.run([MERGE, op, HOST, stamp] + files, check=True,
                                 capture_output=True, text=True).stdout
            rows += [l for l in out.splitlines() if l.strip()]
    with open(os.path.join(HERE, name), "w") as fh:
        fh.write("[\n  " + "\n  ,".join(rows) + "\n]\n")
    print("wrote", name)


def none(op, t):
    pass


def stall(op, t):
    # GitHub runner stalls: 9 of 30 den info runs (3 per round) take
    # ~3.5x as long, as in the 26 Oct run's 272–450 ms info range but
    # worse. den's code is unchanged, so its median is unchanged.
    if op == "info":
        for i in (2, 5, 9, 12, 15, 19, 22, 25, 29):
            t["den"][i] = 0.80 + 0.01 * (i % 5)


def info_2x(op, t):
    # A real regression: every den info run takes twice as long, plus the
    # same stalls hitting brew (inflating brew's mean, not its median).
    if op == "info":
        t["den"][:] = [x * 2 for x in t["den"]]
        for i in (3, 13, 23):
            t["brew"][i] = 1.6


build("noisy-baseline-median.json", "20261027T000000Z", none)
build("noisy-runner-stall.json", "20261027T010000Z", stall)
build("noisy-den-info-2x.json", "20261027T020000Z", info_2x)

# den performance baseline (🎯T81)

Nine of den's CLI callbacks open with
`load_index(cfg.cache / "index.json")`. On a developer machine that
file is around 21 MB, so `den install`, `den search`, `den info` and
`den list` all parse it before doing anything the user asked for.
`bench/bench.cpp` measures that.

## Running them

```bash
make bench             # run them and print the results
make bench-lock        # make this run the new baseline
make bench-gate        # compare against docs/perf/baseline.txt
make bench-gate-loose  # compare only the counted metrics
```

The gate fails in **both** directions. A regression needs a fix; an
improvement needs `make bench-lock` in the same commit as the change
that earned it, because a baseline nobody re-locks stops describing the
code and the next regression only has to stay under the stale number.

## The corpus is generated

7,000 packages with the fields `package_from_json` reads — Homebrew's
order of magnitude — written to a temporary file by the benchmark
itself. Never the developer's own `~/.den/cache/index.json`: a
benchmark that reads real local state is neither reproducible nor safe
to commit numbers from.

The generated index is 5.9 MB against the real one's 21 MB, so every
timing here is roughly a quarter of what the machine actually pays.

## Timings are floors

Each benchmark runs many rounds and reports the fastest. Load on the
machine can only ever make a round slower, so the floor is the closest
thing to an uncontended measurement. Floors are still only comparable
on the machine that recorded them, so `make bench-gate-loose` skips
them and compares only the counted metrics — packages loaded, index
bytes — which are identical everywhere.

## Reference machine

| | |
|---|---|
| Machine | Apple M4 Max, macOS 26.6.2, arm64 |
| Build | Release, C++23 |
| Recorded | 2026-09-07, under a large agent fan-out |

## Locked numbers

| Benchmark | ns/op floor | counted |
|---|---|---|
| `LoadIndex` | 29.8 ms | 7,000 packages, 5,918,724 index bytes |
| `Attr_ParseFromStream` | 26.6 ms | |
| `Attr_ParseFromString` | 25.6 ms | |
| `Attr_ReadFileOnly` | 0.78 ms | |

The last three are not benchmarks of the product but a decomposition of
the first: reading the file, and building the DOM two ways.

## What moved, and by how little

`load_index` now reads the file whole and parses from contiguous
memory, where it used to hand nlohmann the `ifstream`. nlohmann's
stream overload pulls input through a `std::istreambuf_iterator` one
character at a time; a string gets its contiguous fast path.

**It is worth 4-5% of the parse, and no more.** 26.4 ms against 25.3
ms, measured back to back in one process across four runs, with the
file read costing under a millisecond. The change is free and never
slower, so it ships — but the honest number is small.

An earlier reading of the same pair said 50 ms against 26 ms, which
would have been a factor of two. That was contention: the
character-at-a-time path is far more sensitive to a loaded machine than
the contiguous one, and the two attribution benchmarks exist precisely
so that claim could be checked rather than believed. The first version
of this harness also made parsing from a string look *slower* than from
a stream, because its "read the file" step used the
`istreambuf_iterator` idiom and cost 15 ms on its own. Both errors were
in the measurement, not the code.

## Where the cost actually is

After the change, `LoadIndex` is 29.8 ms and the DOM parse is 25.6 ms
of it — about 85%. The remaining structure is the point: the code
builds a `std::map` of all 7,000 packages to answer a question about
one. That is 🎯T82, and it is a format decision rather than a patch, so
it was not attempted in the burst that measured it.

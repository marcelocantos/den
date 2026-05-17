# Telemetry Decision Contract

This document defines the questions that den's opt-in telemetry (🎯T70 / 🎯T32)
must be able to answer.  These are the acceptance criteria for the "actionable
reports" half of T70 — the data design is only good if it drives real product
decisions.

---

## Decision-grade questions

### 1. What fraction of installs need Ruby?

**Question:** Of all `den install` invocations, what percentage trigger a Ruby
evaluation rather than a pure bottle pour?

**Why it matters:** Ruby is the largest complexity and performance risk in den.
If Ruby is triggered in fewer than 1% of installs, lazy-vendoring (download Ruby
only on demand) is sufficient and the binary stays lean.  If it is 20%+, we
should investigate eliminating each trigger category.

**Data required:** `ruby_triggers` category counts (e.g.
`source_build_no_bottle`, `tap_formula_parse`) plus total install count.

**Threshold:** If `sum(ruby_triggers) / total_installs > 0.05` (5%), open a
target to systematically reduce the dominant category.

---

### 2. Which Ruby trigger category dominates?

**Question:** Among installs that do trigger Ruby, which category is most
common — and is it reducible?

**Why it matters:** Tells us where to invest engineering effort.
`tap_formula_parse` → invest in a better native parser.
`post_install_hook` → try to eliminate hook dependencies upstream.
`source_build_no_bottle` → expand bottle matrix.

**Data required:** `ruby_triggers` broken down by category.

**Threshold:** If any single category exceeds 60% of all Ruby triggers, it
becomes the primary engineering focus for T32 follow-up work.

---

## Privacy contract

Every payload must satisfy all of the following before transmission:

| Invariant | Check |
|---|---|
| No formula names | `ruby_triggers` values are counts, not names |
| No user paths | No absolute path strings (values starting with `/`) |
| No email addresses | No `@`-containing string values |
| No usernames | No value matching the system username |
| Opt-in gate | Payload is only collected if `telemetry.enabled = true` in `~/.den/config.json` |

These invariants are enforced mechanically in `tests/test_telemetry_categories.cpp`
(🎯T70 verification harness).

---

## Payload schema reference (T32)

```jsonl
{"v":1,"ts":"2026-03-23T12:00:00Z","period":"7d",
 "ruby_triggers":{"tap_formula_parse":4,"post_install_hook":1},
 "installs":{"bottle":12,"cask":2},
 "envs":3,
 "search":{"keyword":8,"embedding":3},
 "solver_conflicts":0}
```

- `v`: schema version (bump on breaking changes).
- `ts`: ISO-8601 UTC timestamp of the report window end.
- `period`: reporting window duration.
- `ruby_triggers`: map of category → count. Keys are the fixed set documented
  in T32; no formula names appear here.
- `installs`: breakdown by artifact type (bottle / cask / source).
- `envs`: number of named environments in the active DEN_HOME.
- `search`: counts by search provider type.
- `solver_conflicts`: number of SAT-solver conflicts during dependency resolution.

---

## Transparency guarantee

`den telemetry send` prints the exact JSONL payload to stdout **before**
transmitting, with no hidden fields or post-processing.  What the user sees
is exactly what gets sent.  This is verified by `tests/test_telemetry_payload_visible.cpp`.

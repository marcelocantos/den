# Audit Log

## 2026-03-27 — /audit (round 3)

- **Commit**: `18f56c8`
- **Outcome**: 25 findings (4 high, 10 medium, 10 low, 1 deferred). Report: `docs/audit-2026-03-27.md`. 6 agents (4 opus, 2 sonnet) with adversarial review gate. 6 findings filtered/downgraded. Post-cli-split, post-round-2-fixes.
- **Key findings**: Shell injection via eval'd output (high), env path allows unsafe chars (high), manifest-sourced names bypass validation (high), cask module ungated for macOS (high), XML injection in plist (medium), non-atomic writes in 5 locations (medium).
- **Deferred**: F25: CI/CD pipeline (carried forward from round 2).

## 2026-03-25 — /audit (round 2) + fixes

- **Commit**: `b1de362`
- **Outcome**: 37 findings (3 critical, 8 high, 14 medium, 12 low). Report: `docs/audit-2026-03-25.md`. 6 parallel opus agents (security, correctness, testing, architecture/perf, legal/docs, build/CI/portability) with adversarial review gate. 1 false positive filtered (decode_component roundtrip). 1 additional false positive (F37 --help-agent already wired).
- **Fixed**: 35 of 37 findings addressed across 3 commits (fbbeca7, 0c27920, 45d5e1b).
- **Deferred**:
  - F3: CI/CD pipeline (requires GitHub repo to exist first)
  - F14: cli/mod.rs split — resolved in `18f56c8`

## 2026-03-24 — /audit (round 1)

- **Commit**: `3f65a82`
- **Outcome**: 78 raw findings → most addressed in subsequent fix commits (a9e7328, d36d0e4, 60ce805, b1de362). Remaining issues carried forward to round 2.

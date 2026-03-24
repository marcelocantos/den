# Audit Log

## 2026-03-25 — /audit (round 2)

- **Commit**: `b1de362`
- **Outcome**: 37 findings (3 critical, 8 high, 14 medium, 12 low). Report: `docs/audit-2026-03-25.md`. 6 parallel opus agents (security, correctness, testing, architecture/perf, legal/docs, build/CI/portability) with adversarial review gate. 1 false positive filtered (decode_component roundtrip).
- **Deferred**: All 37 findings pending user triage.

## 2026-03-24 — /audit (round 1)

- **Commit**: `3f65a82`
- **Outcome**: 78 raw findings → most addressed in subsequent fix commits (a9e7328, d36d0e4, 60ce805, b1de362). Remaining issues carried forward to round 2.

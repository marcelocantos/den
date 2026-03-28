# Audit Log

## 2026-03-29 — /release v0.1.0

- **Commit**: `b9c1764`
- **Outcome**: Released v0.1.0 (darwin-aarch64, linux-x86_64, linux-aarch64). First public release. Streaming downloads, doctor command, daemon auto_download setting, STABILITY.md, release workflow fixes. Install via `curl -fsSL .../install.sh | sh`.

## 2026-03-28 — /audit (round 5)

- **Commit**: `ef7eaf9` + `ff1cb42`
- **Outcome**: 2 opus agents (adversarial security + correctness/edge cases). 13 raw findings total; after dedup: 4 fixed, 5 deferred as architectural/design items, 4 filtered (trust-model observations not actionable as code changes).
- **Fixed**:
  - `decode_component` chained-replace corruption for `%2D` in path components (high) — `2f8d4a2`
  - `validate_formula_name` allows `.`/`..` enabling cellar path traversal (medium) — `2f8d4a2`
  - `settings::set_key` cannot reset numeric fields to null (medium) — `2f8d4a2`
  - Plist write in daemon_cmd.rs uses non-atomic `fs::write` (medium) — `ff1cb42`
- **Deferred / documented** (architectural — need targets):
  - Manifest read-modify-write has no file locking — concurrent `den install` can lose changes (high)
  - Concurrent `pour_bottle` of same formula can interleave files (medium)
  - `parent_path("")` returns `Some("/")` — double root in `ancestor_chain` (medium)
  - Formula index has no integrity verification — supply-chain trust model (high, design)
  - Download streaming to disk instead of full in-memory buffering (low, performance)
  - CI/CD pipeline (carried forward from round 2)
- **Filtered** (trust-model observations, same as Homebrew):
  - GHCR token sent to URL from formula index (index IS the trust root)
  - HOMEBREW_PREFIX/CELLAR env vars accepted without validation (user env is trusted)
  - Service plist loaded from kegs without content validation (same trust model as Homebrew)

## 2026-03-28 — /audit (round 4)

- **Commit**: `b5838f9`
- **Outcome**: 14 raw findings from 3 agents (2 opus, 1 sonnet). After dedup against rounds 2-3: 7 new actionable findings (0 high, 5 medium, 2 low). Build/clippy/fmt/test all clean. Only file >500 lines: daemon/mod.rs (525).
- **Fixed**: All 7 in commit `4f5c06d`. Shell `!` escaping, env path slash collapsing + `.` rejection, child env protection on remove, manifest_file defense-in-depth assert, install.sh version validation, signal handler error propagation, dep tree label fix.
- **Deferred**: CI/CD pipeline (carried forward).

## 2026-03-27 — /audit (round 3)

- **Commit**: `18f56c8`
- **Outcome**: 25 findings (4 high, 10 medium, 10 low, 1 deferred). Report: `docs/audit-2026-03-27.md`. 6 agents (4 opus, 2 sonnet) with adversarial review gate. 6 findings filtered/downgraded. Post-cli-split, post-round-2-fixes.
- **Key findings**: Shell injection via eval'd output (high), env path allows unsafe chars (high), manifest-sourced names bypass validation (high), cask module ungated for macOS (high), XML injection in plist (medium), non-atomic writes in 5 locations (medium).
- **Fixed**: 24 of 25 findings addressed in commit `b457a15`. Only F25 (CI/CD) remains deferred.

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

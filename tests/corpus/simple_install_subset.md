# SIMPLE Formula Install Subset (T59)

This directory contains the supporting files for the T59 end-to-end install
verification suite.

## Files

| File | Purpose |
|---|---|
| `simple_install_subset.txt` | ~10 fast-building formulae chosen for CI |
| `.github/workflows/simple-install-cron.yml` | Nightly cron workflow that runs `den install --build-from-source` per entry |

## What this verifies

T59 proves that the native formula parser's output is *sufficient* to drive
a real install — not just equivalent to Ruby's output on paper. The workflow
runs `den install --build-from-source <formula>` for each entry in the
subset. A failed install fails the CI run.

Failure modes captured:

- **Parser missing a needed field** — build exits non-zero because a required
  flag or path was absent from `build_commands`.
- **Parser misclassifying a formula as SIMPLE** — build runs the wrong steps
  and produces a broken or empty install.
- **Build environment gap** — a missing header, linker flag, or tool only
  surfaces at actual compile time, not in parser unit tests.

## Inclusion criteria

A formula belongs in `simple_install_subset.txt` when:

1. It appears in `tests/corpus/simple_formulae.txt` (native parser
   classifies it as SIMPLE; url/sha256/build_commands match the Ruby golden).
2. Its build completes in under ~3 minutes on a standard CI runner (no
   heavy compile-time dependencies, no large generated sources).
3. It has no mandatory external library dependencies outside the subset
   itself (self-contained, or depends only on other subset members).
4. It builds cleanly on macOS arm64 and Linux x86_64.

## How to expand

1. Add the candidate to `tests/corpus/simple_formulae.txt`.
2. Run `ruby scripts/update-oracle-golden.rb` to generate/refresh the golden
   file under `tests/corpus/golden/`.
3. Run `ctest --test-dir build -R formula_parser_oracle` and confirm the new
   entry passes.
4. Add the candidate to `simple_install_subset.txt` **and** to the
   `matrix.formula` list in the workflow YAML.
5. Trigger a manual `workflow_dispatch` run to confirm CI passes before the
   next nightly cron.

## Expected runtime

Approximately 15–20 minutes total per (formula, OS) pair on a cold runner,
dominated by source download and compile time. The full matrix (10 formulae ×
2 OS) runs in parallel; wall-clock time is bounded by the slowest single job.

## Upstream dependency: T58

T58 adds dependency extraction to the native parser. Until T58 is merged:

- The workflow has `continue-on-error` commented into its steps (not yet
  active because only confirmed-SIMPLE formulae are in the active matrix).
- The cron schedule trigger is commented out in the workflow — activate it
  by un-commenting the `cron:` line after T58 lands.
- Proposed candidates in `simple_install_subset.txt` are commented out; add
  them to the active matrix list once their parser classification is verified.

**To flip the schedule on (post-T58):**

1. Un-comment `- cron: '30 0 * * *'` in the workflow's `on:` block.
2. Remove any remaining `continue-on-error: true` from install steps.
3. Un-comment proposed candidates in the matrix formula list and in
   `simple_install_subset.txt` (after verifying each via oracle tests).

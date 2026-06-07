# den harness

End-to-end release-candidate validation for den. This harness is distinct from
`tests/test_*.cpp`, which are in-process unit tests run by `ctest`. The harness
exercises a real built binary against a real (sandboxed) filesystem, proving that
install, environment management, and uninstall behave correctly end-to-end.

## Purpose

Catch integration regressions before a release ships to users. The harness runs
the same smoke set (`smoke.sh`) on every supported platform:

- **Linux** — executed inside a Docker container (see `tests/harness/linux/`).
- **macOS** — executed over SSH on a real macOS host (see `tests/harness/macos/`, T73.2 — not yet implemented).

## File layout

```
tests/harness/
├── README.md          — this file
├── smoke.sh           — shared POSIX smoke set (Linux + macOS)
├── linux/             — Docker harness (Makefile rule: make harness-linux)
└── macos/             — SSH harness (future — T73.2)
```

## Running locally (Linux)

Build den for your current platform, then:

```sh
make harness-linux
```

This rule (defined in the root Makefile) builds a Docker image with the den
binary baked in, mounts a temporary `DEN_HOME`, and runs `smoke.sh` inside the
container. The exit code mirrors the smoke-set result.

## Smoke-set contract

`smoke.sh` is driven by three environment variables:

| Variable   | Required | Default | Description |
|------------|----------|---------|-------------|
| `DEN_BIN`  | yes      | —       | Path to the den binary, or bare command name if it is on PATH. |
| `DEN_HOME` | yes      | —       | Writable sandbox directory. Must not be `$HOME/.den` or `/opt/homebrew`; the script refuses to run if either guard fires. |
| `TEST_PKG` | no       | `jq`    | Package to install and exercise. Must be a small, fast Homebrew formula. |

### Output format

- `PASS: <name>` — step succeeded.
- `FAIL: <name>: <reason>` — step failed.
- `SKIP: <name>: <reason>` — step was not exercised (feature not yet implemented).

Verbose detail (raw command output) goes to **stderr**. The structured
`PASS`/`FAIL`/`SKIP` lines go to **stdout**, making them easy to capture in CI
logs.

**Exit semantics**: exit 0 if there are no `FAIL` lines (SKIPs are allowed);
non-zero if any step fails.

### Idempotency

The script is safe to run twice in sequence without resetting `DEN_HOME`. Each
step cleans up its own preconditions before acting (e.g., `env_create` removes
`harness-test` if it already exists).

## The smoke set is canonical

The 13-step smoke set in `smoke.sh` is the **authoritative list** of what the
harness expects den to support:

1. `version` — `den --version` reports a non-empty version string.
2. `help` — `den --help` mentions `install` and `doctor`.
3. `doctor` — `den doctor` runs and produces output (non-zero exit allowed while doctor is partial).
4. `info` — `den info $TEST_PKG` returns package metadata.
5. `install` — `den install $TEST_PKG` succeeds.
6. `installed_pkg_runs` — the installed binary (e.g. `jq --version`) runs correctly.
7. `env_create` — `den env create harness-test` creates a new environment.
8. `env_list` — `den env list` includes `harness-test`.
9. `env_use` — `den env use harness-test` switches the active environment.
10. `env_destroy` — `den env remove harness-test` tears the environment down.
11. `uninstall` — `den uninstall $TEST_PKG` removes the package.
12. `state_clean_post_uninstall` — no binary remains under `$DEN_HOME/envs/*/bin/` after uninstall.
13. `selfupdate_check` — `den self-update --check` probes for an update without applying it (currently SKIP — no dry-run mode exists).

**Gaps are SKIPs, not omissions.** When a den capability is not yet implemented,
the corresponding step emits `SKIP:` with a clear reason. A `SKIP` is a tracked
gap — it is not invisible. When the capability lands, change the step from a
`SKIP` to a real assertion. Adding a new den capability means adding the step as
a `SKIP` first.

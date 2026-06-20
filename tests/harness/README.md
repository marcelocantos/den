# den harness

End-to-end release-candidate validation for den. This harness is distinct from
`tests/test_*.cpp`, which are in-process unit tests run by `ctest`. The harness
exercises a real built binary against a real (sandboxed) filesystem, proving that
install, environment management, and uninstall behave correctly end-to-end.

## Purpose

Catch integration regressions before a release ships to users. The harness runs
the same smoke set (`smoke.sh`) on every supported platform:

- **Linux** — executed inside a Docker container (see `tests/harness/linux/`).
- **macOS** — executed over SSH on a real macOS host (see `tests/harness/macos/`).

## File layout

```
tests/harness/
├── README.md          — this file
├── smoke.sh           — shared POSIX smoke set (Linux + macOS)
├── linux/             — Docker harness (Makefile rule: make harness-linux)
└── macos/             — SSH harness (Makefile rule: make harness-macos)
```

## Running locally (Linux)

Build den for your current platform, then:

```sh
make harness-linux
```

This rule (defined in the root Makefile) builds a Docker image with the den
binary baked in, mounts a temporary `DEN_HOME`, and runs `smoke.sh` inside the
container. The exit code mirrors the smoke-set result.

## Running locally (macOS)

One-time setup is required (SSH alias, Remote Login, key-based auth, clamshell-sleep fix).
See `tests/harness/macos/README.md` for step-by-step instructions.

Once set up, build den for your current platform and then:

```sh
make harness-macos
```

This rule (defined in the root Makefile) ssh's into the configured test host, uploads the
binary and `smoke.sh`, runs the smoke set inside a temporary `DEN_HOME` sandbox under
`/tmp/den-harness/`, and reports pass/fail/skip counts.

**This harness is not in CI.** The test host is a dev-controlled physical machine, not a
CI runner. Runs are triggered manually via `make harness-macos` (or by calling
`tests/harness/macos/run.sh` directly).

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

The 14-step smoke set in `smoke.sh` is the **authoritative list** of what the
harness expects den to support:

1. `version` — `den --version` reports a non-empty version string.
2. `help` — `den --help` exits 0 and produces non-empty output.
3. `doctor` — `den doctor` runs and produces output (non-zero exit allowed while doctor is partial).
4. `update` — `den update` populates the package index (required before info/install on a fresh sandbox).
5. `info` — `den info $TEST_PKG` returns package metadata.
6. `install` — `den install $TEST_PKG` succeeds **and** the package binary appears under `$DEN_HOME`.
7. `installed_pkg_runs` — the installed binary (e.g. `jq --version`) runs correctly.
8. `uninstall` — `den uninstall $TEST_PKG` removes the package.
9. `state_clean_post_uninstall` — no binary remains under `$DEN_HOME/envs/*/bin/` after uninstall.
10. `env_create` — `den env create harness-test` creates a new environment.
11. `env_list` — `den env list` includes `harness-test`.
12. `env_use` — `den env use harness-test` switches the active environment.
13. `env_destroy` — `den env remove harness-test` tears the environment down.
14. `selfupdate_check` — `den self-update --check` probes for an update without applying it (must not download or replace the binary).

Install/uninstall lifecycle (steps 6–9) runs before env creation/teardown
(steps 10–13) so the package's symlinks are checked against the env they
were installed into. Reordering creates a known false negative when the
active env changes mid-lifecycle.

**Gaps are SKIPs, not omissions.** When a den capability is not yet implemented,
the corresponding step emits `SKIP:` with a clear reason. A `SKIP` is a tracked
gap — it is not invisible. When the capability lands, change the step from a
`SKIP` to a real assertion. Adding a new den capability means adding the step as
a `SKIP` first.

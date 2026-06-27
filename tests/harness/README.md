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

`smoke.sh` is driven by two required environment variables and a set of
optional knobs that nominate packages for the expanded feature steps:

| Variable   | Required | Default | Description |
|------------|----------|---------|-------------|
| `DEN_BIN`  | yes      | —       | Path to the den binary, or bare command name if it is on PATH. |
| `DEN_HOME` | yes      | —       | Writable sandbox directory. Must not be `$HOME/.den` or `/opt/homebrew`; the script refuses to run if either guard fires. |
| `TEST_PKG` | no       | `jq`    | Homebrew formula to install and exercise in the bottle-install lifecycle. Must be small and fast. |
| `MULTI_PKG`| no       | (unset) | Versioned Homebrew formula for the SAT multi-version coinstall step. Unset → that step SKIPs. |
| `TAP_NAME` | no       | (unset) | Third-party tap (`user/repo`) for the source-build-via-tap step. Unset → SKIP. |
| `TAP_URL`  | no       | (unset) | Optional clone source for `TAP_NAME` (git URL or local path). |
| `TAP_PKG`  | no       | (unset) | Formula in `TAP_NAME` to build from source. Unset → SKIP. |
| `PY_PKG`   | no       | `cowsay` | Package installed via the `pip` provider. |
| `NODE_PKG` | no       | `is-thirteen` | Package installed via the `npm` provider. |
| `GO_PKG`   | no       | `github.com/rakyll/hey@latest` | Module installed via the `go` provider. |
| `CARGO_PKG`| no       | `ripgrep` | Crate installed via the `cargo` provider. |
| `MIGRATE_SMOKE` | no  | auto-detected | Path to `scripts/migrate-smoke.sh`. Auto-resolved from a checkout. |
| `RUN_DEEP_MIGRATE` | no | `0` | Set to `1` to additionally run the full `migrate-smoke.sh` (real migration + Cellar byte-identity + idempotency). Off by default because it snapshots the whole Cellar twice and is slow on large installs. |
| `MIGRATE_TIMEOUT` | no | `180` | Seconds before the deep migrate check is killed and recorded as SKIP (needs `timeout`/`gtimeout`). |

All package knobs are overridable so a platform-specific driver can pick
versions known to have bottles / registry entries for the runner.

### Output format

- `PASS: <name>` — step succeeded.
- `FAIL: <name>: <reason>` — step failed.
- `SKIP: <name>: <reason>` — step was not exercised (precondition absent — a
  missing toolchain, no network, no Homebrew, or no nominated package).

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

The smoke set in `smoke.sh` is the **authoritative list** of what the harness
expects den to support. It is split into a network-free **baseline** (always
runs) and an **expanded v1 feature set** (🎯T74 / 🎯T75) that covers every row
of `docs/den-vs-brew.md` and degrades to `SKIP` when a precondition is absent.

### Baseline lifecycle

1. `version` — `den --version` reports a non-empty version string.
2. `help` — `den --help` exits 0 and produces non-empty output.
3. `doctor` — `den doctor` runs and produces output (non-zero exit allowed while doctor is partial).
4. `update` — `den update` populates the package index. **Network-gated**: SKIPs offline (no index → downstream Homebrew steps SKIP too).
5. `info` — `den info $TEST_PKG` returns package metadata. SKIPs if the index is empty.
6. `install` — `den install $TEST_PKG` succeeds **and** the package binary appears under `$DEN_HOME`. SKIPs if the index is empty.
7. `installed_pkg_runs` — the installed binary (e.g. `jq --version`) runs correctly. SKIPs if install was skipped.
8. `uninstall` — `den uninstall $TEST_PKG` removes the package. SKIPs if nothing was installed.
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

### Expanded v1 feature set

These steps map one-to-one onto the den-vs-brew capability matrix. Each is
**self-gating**: it SKIPs with a clear reason when its toolchain, the network,
or a nominated package is missing, so the harness stays deterministic on any
CI box.

| Step | Capability (🎯) | Runs when… | Otherwise |
|------|-----------------|-----------|-----------|
| `config` | host introspection (🎯T66) | always | — |
| `list_cellar` | Cellar introspection (🎯T66) | always | — |
| `deps_explain` | SAT solver reasoning (🎯T63) | always (built-in demo universe) | — |
| `defer_while_in_use` | deferred-upgrade surfacing (🎯T72) | always (`outdated` + `daemon status`) | — |
| `multi_version_coinstall` | multi-version coinstall (🎯T63) | index ready + `MULTI_PKG` set | SKIP |
| `source_build_via_tap` | tap + source build (🎯T67 / 🎯T65) | network + compiler + `TAP_NAME`/`TAP_PKG` | SKIP |
| `python_provider` | pip provider (🎯T60) | `pip3` on PATH + network | SKIP |
| `node_provider` | npm provider (🎯T60) | `npm` on PATH + network | SKIP |
| `go_provider` | go provider (🎯T60) | `go` on PATH + network | SKIP |
| `cargo_provider` | cargo provider (🎯T60) | `cargo` on PATH + network | SKIP |
| `migrate_from_brew` | brew migration (🎯T71) | `brew` on PATH | SKIP |

The `config`, `list_cellar`, `deps_explain`, and `defer_while_in_use` steps are
network-free and assert real behaviour on every run — including offline CI.

`migrate_from_brew` runs `den migrate --dry-run` (non-destructive) when `brew`
is present — that is the guaranteed assertion. With `RUN_DEEP_MIGRATE=1` it
additionally runs the deeper `scripts/migrate-smoke.sh` (real migration +
Cellar byte-identity + idempotency) in an isolated `DEN_HOME`, bounded by
`MIGRATE_TIMEOUT` so it can never hang the harness on a large Cellar (a
timeout is recorded as SKIP, the dry-run having already passed).

### Graceful degradation contract

Every expanded step **must SKIP, never FAIL**, when its precondition is absent.
A `FAIL` is reserved for a real regression (den crashed, exited non-zero on a
path that was supposed to work, or produced wrong output). This keeps the
harness green-or-red on *den's* behaviour, not on the runner's toolchain
inventory. The network is probed once (fast, cached) via the Homebrew formula
API; if `curl`/`wget` are absent or the probe fails, network-dependent steps
SKIP.

**Gaps are SKIPs, not omissions.** When a den capability is not yet implemented,
the corresponding step emits `SKIP:` with a clear reason. A `SKIP` is a tracked
gap — it is not invisible. When the capability lands, change the step from a
`SKIP` to a real assertion. Adding a new den capability means adding the step as
a `SKIP` first.

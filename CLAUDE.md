# den

A universal development environment manager. Think virtualenv for
everything — not just Python, but every package on your system.

## Overview

den is a package and environment manager that:

- **Consumes the Homebrew ecosystem** — compatible with existing
  formulae, casks, taps, and bottles.
- **Supports multi-version coinstallation** — install python@3.11 and
  python@3.12 side by side without conflict. Switch with `den use`.
- **Provides named environments** — like virtualenv but for all
  packages. Create isolated, composable, reproducible environments
  that layer via PATH.
- **Upgrades in the background** — a daemon downloads and installs
  new versions alongside existing ones. Nothing changes until you
  switch. Rollback is instant.

## Delivery

delivery: merged to master, CI green

## Gates

profile: cli

## Release

homebrew_tap: disabled

den replaces Homebrew, so distributing den via a Homebrew tap is
circular. Binaries ship via GitHub Releases and the `install.sh`
installer. The `/release` skill honours this directive and skips all
tap-related phases.

## Architecture

The convergence roadmap lives in `bullseye.yaml` at the repo root.
Use the `bullseye` MCP server (or `/cv`) to query it; there is no
rendered Markdown copy.

### Key Design Decisions

- **Shared Cellar at /opt/homebrew**: Den uses the same Cellar as
  Homebrew (`/opt/homebrew/Cellar/`). Bottles pour at their expected
  prefix with zero relocation — 100% of bottles work immediately.
  Den and Homebrew coexist on the same Cellar. Den tracks what it
  manages via its own manifest in `~/.den/`.
- **Environments are symlink sets**: Each environment is a directory
  in `~/.den/envs/` containing symlinks into the Cellar. Environments
  compose via PATH precedence — a project env overrides the default env.
- **Multi-version by default**: Installing a new version never removes
  the old one. `den use` atomically switches which version is linked
  in the active environment.
- **Background daemon**: Downloads and stages upgrades without user
  intervention. The user's active environment is never modified
  without explicit action.

### Source Layout

C++23, built with CMake + Ninja. `src/` is split into per-concern
modules; everything compiles into a single `den_lib` static library
linked by the `den` binary and the `den_tests` test runner.

```
src/
├── main.cpp        # CLI entry point
├── activity/       # User-facing progress / activity reporting
├── build/          # Source-build pipeline (incl. native formula parser)
├── cli/            # Command parsing and dispatch
├── core/           # Shared utilities and primitives
├── daemon/         # Background upgrade / supervisor daemon
├── doctor/         # `den doctor` health checks
├── download/       # Bottle / source download + cache
├── env/            # Environment management (create, switch, layer)
├── index/          # Formula / cask metadata index
├── migrate/        # Homebrew → den migration
├── platform/       # OS/arch detection, Mach-O/ELF tools
├── provider/       # PackageProvider interface (multi-provider seam)
├── ruby/           # Bundled / lazy-vendored Portable Ruby integration
├── selfupdate/     # `den selfupdate`
├── settings/       # Unified settings (config.json)
├── smoke/          # In-process smoke checks
└── store/          # Cellar / keg / manifest layer
```

Tests live in `tests/test_*.cpp` (one binary, `den_tests`, run via
`ctest`). Header-only deps are vendored under `vendor/`; system deps
are `libcurl` and `libarchive`.

## Build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

`make bullseye` runs the standing-invariants check used by `/cv`
(configure-if-needed, build, test, format, clean tree).

## TODO Location

docs/TODO.md

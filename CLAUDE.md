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
- **Upgrades in the background** — a daemon refreshes the index,
  pre-downloads new versions and (when `daemon.auto_upgrade` is on)
  installs them alongside existing ones. Rollback is instant.

## Delivery

delivery: merged to master, CI green

## Gates

profile: cli

## Release

homebrew_tap: disabled
<!-- den is not distributed through Homebrew: no tap formula, no bottles, no tapper. -->

The generic `/release` skill honours this directive and skips all of its
own tap-related phases. den's release pipeline is its own: RCs are cut by
`release-candidate.yml`, promoted to GA by `promote-rc.yml`, and binaries
ship via GitHub Releases and the `install.sh` installer.

den is not distributed through Homebrew: there is no tap formula and
no den bottles, and nothing in the release path runs tapper.
`scripts/check-no-brew-release.sh` (ctest `script_test_no_brew_release`)
fails if a workflow or the Makefile brings that path back.

## Architecture

The convergence roadmap lives in `bullseye.yaml` at the repo root.
Use the `bullseye` MCP server (or `/cv`) to query it; there is no
rendered Markdown copy.

### Key Design Decisions

- **Shared Cellar at /opt/homebrew**: Den uses the same Cellar as
  Homebrew (`/opt/homebrew/Cellar/`). Bottles keep `@@HOMEBREW_*@@`
  placeholders, which den rewrites on pour. On macOS it rewrites Mach-O
  ids, load commands and rpaths via `otool`/`install_name_tool`
  (falling back to `/Library/Developer/CommandLineTools/usr/bin` when
  the xcode-select shim is broken), binds `opt/<dep>` references through
  `<prefix>/opt/<dep>` when den owns that link and pins them to the exact
  dependency keg otherwise (always in a prefix shared with Homebrew), and fails and rolls back the pour if any
  placeholder survives. Den never creates or repoints Homebrew's shared
  `<prefix>/opt` links on macOS (#64). On Linux, den rewrites ELF
  interpreter/RPATH entries with patchelf and creates a prefix `opt`
  link only if it is missing or dangling.
  Den and Homebrew coexist on the same Cellar. Den tracks what it
  manages via its own manifest in `~/.den/`.
- **Environments are symlink sets**: Each environment is a directory
  in `~/.den/envs/` containing symlinks into the Cellar. Environments
  compose via PATH precedence — a project env overrides the default env.
- **Multi-version by default**: Installing a new version never removes
  the old one. `den use` atomically switches which version is linked
  in the active environment.
- **Background daemon**: Each cycle (`run_maintenance_cycle` in
  `src/daemon/daemon.cpp`) refreshes the index, computes pending
  upgrades for den-managed formulae, pre-downloads bottles, and — only
  with `daemon.auto_upgrade` inside `daemon.upgrade_window` — applies
  them via the in-use-aware upgrade pass. Casks are never auto-upgraded. The user's active environment is never modified
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
├── cask/           # macOS cask install/upgrade/uninstall (receipts, stage-then-swap)
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
`ctest`). Header-only deps are vendored under `vendor/`; libarchive
(+xz/zstd/lz4) is built from pinned sources and linked statically
(`cmake/VendorLibarchive.cmake`, `DEN_VENDOR_LIBARCHIVE=ON`). The only
system dep is `libcurl`; `scripts/check-links.sh` enforces that release
binaries link nothing outside the base OS.

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

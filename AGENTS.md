# den — agent instructions

Canonical agent-facing instructions for this repo. `CLAUDE.md` imports
this file and adds the Claude-only directives.

## What this is

den is a universal development environment manager: one C++ binary
that installs packages from the Homebrew ecosystem (and, via
`--provider`, from pip, npm, go and cargo) into named, composable
environments, keeps every installed version side by side, and stages
upgrades in the background without touching the active environment.
User-facing overview: [README.md](README.md). Product-facing agent
guide: [agents-guide.md](agents-guide.md). Public-surface catalogue:
[STABILITY.md](STABILITY.md).

## Key design decisions

- **Shared Cellar.** den installs Homebrew packages into
  `$HOMEBREW_CELLAR` (default `/opt/homebrew/Cellar` on Apple Silicon,
  `/usr/local/Cellar` on Intel, `/home/linuxbrew/.linuxbrew/Cellar` on
  Linux — `src/core/config.cpp`), so bottles pour at their expected
  prefix with zero relocation and den and `brew` coexist. den tracks
  what it manages in `~/.den/manifests/`.
- **Environments are symlink sets.** Each environment is a directory
  under `~/.den/envs/` whose `bin/`, `lib/`, `include/`, `opt/` symlink
  into the Cellar (or a provider's own root). `den init` emits shell
  code that puts the active environment's `bin/` on `PATH` and sets
  `LIBRARY_PATH`, `CPATH`, `PKG_CONFIG_PATH` and friends.
- **Manifests are per provider.** `manifest.json` is
  `{"packages": {"homebrew": {name: version}, "pip": {…}},
  "auto_deps": {provider: [name]}}`. A pre-v0.11 flat `packages`
  block is promoted under `homebrew` on read.
- **Multi-version by default.** Installing never removes the old
  version; `den use <pkg> <version>` relinks atomically.
- **Providers behind one seam.** `make_default_registry`
  (`src/provider/registry.cpp`) registers `homebrew` first as the
  fallback, then `pip`, `npm`, `go`, `cargo` and `stub`; the auxiliary
  providers never auto-claim a bare name and are selected by
  `--provider` or a manifest hint.
- **Dependency resolution is a SAT solver** (`src/index/sat_solver.*`);
  `den deps --explain` prints the clauses behind an assignment.
- **Two-source trust.** Every bottle install cross-checks the Homebrew
  API hash against `data/known_hashes.json`, an independent replica
  refreshed by `replica-verify.yml`. Rules and outcomes:
  [docs/trust-model.md](docs/trust-model.md).
- **Built-in supervisor.** `den services` manages launchd-free services
  (`src/supervisor/`); the daemon (`src/daemon/`) polls for upgrades,
  defers packages whose files are in use, and applies inside an
  optional `daemon.upgrade_window`.
- **Source builds.** `den install -s` evaluates formulae with a bundled
  Portable Ruby on macOS (lazily downloaded on Linux) or the native
  formula parser for simple ones (`src/build/`, `src/ruby/`).

## Source layout

C++23, CMake + Ninja. Everything under `src/` compiles into the
`den_lib` static library linked by the `den` binary and the `den_tests`
doctest runner.

```
src/
├── main.cpp        # entry point → cli::run
├── activity/       # activity.json upgrade log
├── build/          # source-build pipeline, native formula parser, relocation
├── cli/            # CLI11 command tree and dispatch; install, outdated, shell init
├── core/           # Config (paths, HOMEBREW_* overrides), error types
├── daemon/         # background upgrade daemon, in-use detection, upgrade pass
├── doctor/         # `den doctor` checks, including the trust block
├── download/       # HTTP fetch, archive extraction, SHA-256, cache
├── env/            # environment manifests and materialisation
├── index/          # package index, dependency resolution, SAT solver
├── migrate/        # Homebrew → den migration
├── platform/       # OS/arch detection
├── provider/       # PackageProvider seam: homebrew, pip, npm, go, cargo, stub
├── ruby/           # Portable Ruby bundle and formula extraction helpers
├── selfupdate/     # `den self-update`
├── settings/       # ~/.den/config.json (daemon, search, taps)
├── smoke/          # `den smoke` runner and multi-provider probes
├── store/          # Cellar inspection and linking
├── supervisor/     # built-in service supervisor and plist import
├── tap/            # third-party tap registry under ~/.den/taps/
└── trust/          # two-source hash cross-check
```

Tests are `tests/test_*.cpp` (top level only; `tests/corpus/` holds the
oracle corpus and the optional `homebrew-core` submodule). Header-only
dependencies are vendored under `vendor/include/`; `spdlog` is a
submodule; system dependencies are `libcurl` and `libarchive`.

## Build and gates

```bash
git submodule update --init vendor/github.com/gabime/spdlog   # homebrew-core is optional
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On macOS with a keg-only libarchive add
`-DCMAKE_PREFIX_PATH=$(brew --prefix libarchive)` to the configure
step, as CI does.

`make bullseye` is the standing-invariants gate (`configure`, `build`,
`test`, `format`, `clean-tree`); `make format-fix` applies
clang-format. The unit run takes about three minutes.

Out-of-band gates:

| Target / workflow | What it proves |
|-------------------|----------------|
| `make harness-linux` | Docker smoke of install / env / uninstall with the built binary ([tests/harness/README.md](tests/harness/README.md)) |
| `make harness-macos`, `make soak-macos`, `make remote-check` | The same over SSH on the lab Mac, plus a real `~/.den` soak; dev-triggered only |
| `ci.yml` | Build + ctest on macos-14 and ubuntu on push/PR; format check is warning-only |
| `harness-linux.yml` | Linux harness on push/PR |
| `replica-verify.yml` | Daily re-verification of `data/known_hashes.json` against GHCR |
| `source-build-smoke.yml`, `simple-install-cron.yml`, `bench.yml` | Scheduled source-build, SIMPLE-formula install, and den-vs-brew benchmark runs |

## Release

Releases are minor versions cut as release candidates and promoted:
[docs/release-candidate.md](docs/release-candidate.md).
`release-candidate.yml` builds darwin-aarch64, linux-x86_64 and
linux-aarch64; `promote-rc.yml` flips an RC to GA; `release.yml` runs on
a published release. Binaries ship through GitHub Releases and
`install.sh`; there is no Homebrew tap (den replaces Homebrew). The
version lives in `CMakeLists.txt` (`project(den VERSION …)`).

## Conventions

- Apache-2.0; new sources carry `SPDX-License-Identifier: Apache-2.0`.
- Read `~/.claude/cpp.md` before substantial C++ and `~/.claude/bash.md`
  before touching `install.sh` or the harness scripts.
- No new TOML; settings are JSON.
- Followable work lives in `bullseye.yaml` (use the bullseye MCP tools).
  There is no TODO file.
- Historical audits live under `docs/audits/` and `docs/audit/`; they
  describe the tree on their date, not the current one.

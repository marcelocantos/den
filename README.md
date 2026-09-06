# den

A universal development environment manager. Think virtualenv for
everything — not just Python, but every package on your system.

## What den does

- **Shared Cellar** — den uses `/opt/homebrew/Cellar` directly, so
  bottles pour at their expected prefix with zero relocation. Den and
  Homebrew coexist on the same Cellar.
- **Unified package model** — no formula/cask distinction. Just
  `den install firefox` or `den install ffmpeg`.
- **Multi-version coinstallation** — install `python@3.11` and
  `python@3.12` side by side. Switch instantly with `den use`.
- **Named environments** — create isolated, composable environments
  that inherit from each other. Like virtualenv, but for all packages.
- **Background upgrades** — a daemon downloads new versions alongside
  existing ones. Nothing changes until you switch. Rollback is instant.
  Packages whose files are in use are deferred, not clobbered.
- **Source-first architecture** — embedded Ruby VM for evaluating
  Homebrew formula build recipes. Pre-built archives as an optimisation,
  not a requirement.
- **Multi-provider** — Homebrew is the default, but `den install`
  routes through a pluggable `PackageProvider` interface. Pass
  `--provider pip|npm|go|cargo` to reach those ecosystems without
  changes to the CLI, manifest, or environment layers.
- **Two-source trust** — every bottle hash is cross-checked against an
  independent replica before download. See [docs/trust-model.md](docs/trust-model.md).

How den compares with Homebrew, row by row:
[docs/den-vs-brew.md](docs/den-vs-brew.md).

## Install

```bash
curl -fsSL https://raw.githubusercontent.com/marcelocantos/den/master/install.sh | sh
```

`DEN_VERSION` pins a release, `DEN_INSTALL_DIR` overrides `~/.den`, and
`DEN_NO_MODIFY_PATH=1` skips the shell-profile edit. Releases are built
for macOS arm64, Linux x86_64 and Linux arm64.

Or build from source:

```bash
git clone https://github.com/marcelocantos/den.git
cd den
git submodule update --init vendor/github.com/gabime/spdlog
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cp build/den ~/.den/bin/den
```

Requires cmake, ninja, libcurl and libarchive (all available via
Homebrew; on macOS add `-DCMAKE_PREFIX_PATH=$(brew --prefix libarchive)`
to the configure step if libarchive is keg-only). The
`tests/corpus/homebrew-core` submodule is only needed by the formula
oracle tests, which skip when it is absent.

## Quick start

```bash
# Add to your shell (~/.zshrc or ~/.bashrc; fish is supported too):
eval "$("$HOME/.den/bin/den" init)"

# Fetch the package index:
den update

# Install packages:
den install tree
den install ffmpeg          # resolves and installs all dependencies
den install firefox         # GUI apps — no --cask flag needed
den install -s jq           # build from source instead of pouring a bottle
den install --provider pip ruff

# Manage versions:
den install python@3.11     # installs alongside existing python@3.12
den use python@3.11 3.11.9  # switch to a specific version

# Environments:
den env create /ml          # child of root, inherits all packages
den env use /ml             # switch to it
den install numpy           # only in /ml, root unchanged
den env use /               # back to root

# Background maintenance:
den daemon install          # auto-start at login
den set daemon.auto_download false  # disable background downloads
den set daemon.auto_upgrade true
den set daemon.upgrade_window "3:00-5:00"
den daemon status           # check for pending or deferred upgrades

# Query:
den search sqlite           # search the local index
den info ffmpeg             # show package details
den deps ffmpeg --tree      # dependency tree
den deps ffmpeg --explain   # why the solver chose what it chose
den outdated                # what needs upgrading
den upgrade                 # upgrade everything
```

## How it works

Den manages package providers behind a uniform interface. Homebrew is
the default — den consumes the same archives and formulae API — but
the install / uninstall / upgrade / list / use flows all dispatch
through a pluggable `PackageProvider`. The pip, npm, go and cargo
providers never claim a bare name; they are selected with
`--provider <name>` or by a manifest hint.

- **Shared Cellar** (`/opt/homebrew/Cellar/`) holds Homebrew-installed
  package versions — shared with Homebrew, so bottles work without
  relocation. Other providers keep their own storage areas.
- **Manifests** declare what each environment contains, grouped per
  provider. Child environments inherit from parents and override
  specific packages.
- **Materialisation** asks each owning provider for its package root
  and binary paths, then resolves the manifest hierarchy into a flat
  directory of symlinks.
- **Shell integration** sets PATH and build environment variables
  (LIBRARY_PATH, CPATH, PKG_CONFIG_PATH, etc.) to point at the
  active environment.

```
/opt/homebrew/Cellar/         # shared package store
├── tree/2.3.2/              # each version in its own directory
├── ffmpeg/7.1.1/
└── python@3.13/3.13.2/

~/.den/
├── bin/den                  # the binary
├── config.json              # settings
├── manifests/               # environment definitions
│   ├── ROOT/
│   │   └── manifest.json    # / (root)
│   └── ml/
│       └── manifest.json    # /ml (inherits from /)
├── envs/                    # materialised environments
│   ├── ROOT/
│   │   ├── bin/             # symlinks into Cellar
│   │   ├── lib/
│   │   ├── include/
│   │   └── opt/
│   └── ml/
│       └── ...
├── cache/                   # index.json + content-addressed archive cache
├── taps/<user>/<repo>/      # third-party taps (den tap add)
├── activity.json            # upgrade activity log (den log)
├── daemon.pid, daemon.log   # background daemon
└── daemon_state.json        # pending / deferred upgrades
```

## Configuration

All settings in `~/.den/config.json`, managed via `den set`:

```bash
den set daemon.auto_download false       # disable background downloads
den set daemon.auto_upgrade true         # auto-apply upgrades
den set daemon.upgrade_window "3:00-5:00"  # when to apply
den set daemon.interval_secs 3600        # polling interval
den settings                             # show all settings
```

`DEN_HOME` moves the whole directory; `HOMEBREW_PREFIX`,
`HOMEBREW_CELLAR` and `HOMEBREW_CASKROOM` override the shared store.

## Commands

Run `den --help` for the full list. `--help-agent` adds a pointer to the
agent guide.

| Command | Description |
|---|---|
| `den install [-s] [--provider <p>] <pkg>...` | Install (with dependency resolution); `-s` builds from source |
| `den uninstall [--provider <p>] <pkg>...` | Remove from the active environment |
| `den upgrade [--provider <p>] [pkg]...` | Upgrade outdated packages |
| `den update` | Refresh the package index |
| `den use <pkg> <version>` | Switch active version |
| `den list [--cellar]` | List packages; `--cellar` shows per-keg disk usage, references and orphans |
| `den run <cmd> [args]...` | Run a binary from the active environment |
| `den search <text>` | Search packages by name or description |
| `den info <pkg>` | Show package details |
| `den deps <pkg> [--tree] [--explain]` | Dependencies, as a tree, or with the solver's reasoning |
| `den outdated` | Packages with updates available, including deferred ones |
| `den env create\|list\|remove\|use\|show\|freeze` | Environments; `freeze` exports a JSON lockfile |
| `den init [--shell bash\|zsh\|fish]` | Print the shell integration script |
| `den status` | Environment and daemon summary |
| `den set <key> <value>` / `den settings` | Configuration |
| `den config` | Detected host configuration (Xcode, SDK, Homebrew paths) |
| `den tap add <user/repo> [source]` / `--list` / `--remove` | Third-party formula taps |
| `den migrate [--dry-run] [--integrate-shell] [--no-health-check]` | Import formulae, casks, taps and services from Homebrew |
| `den daemon run\|stop\|status\|install\|uninstall` | Background upgrade daemon |
| `den services list\|start\|stop\|restart\|status\|logs` | Package services under the built-in supervisor |
| `den log [-n N] [--json]` | Upgrade activity log |
| `den whence <file-or-name>` | Which package owns a file or command |
| `den cleanup` / `den autoremove` | Remove old versions and cache files / unused dependencies |
| `den doctor` | System health, including the trust block |
| `den self-update [--check]` | Update den to the latest release |
| `den smoke [--defs FILE] [-n N]` | Smoke tests against installed packages |

## Agent guide

If you use an agentic coding tool (Claude Code, Cursor, etc.), include
[`agents-guide.md`](agents-guide.md) in your project context for
den-aware assistance.

## Contributing

```bash
git clone https://github.com/marcelocantos/den.git
cd den
git submodule update --init vendor/github.com/gabime/spdlog
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

`make bullseye` runs the same gates CI does (configure, build, test,
clang-format check, clean tree). Repo layout, design decisions and the
other gates are in [AGENTS.md](AGENTS.md).

Further reading:

- [STABILITY.md](STABILITY.md) — the public CLI, config, and file-layout surface
- [docs/release-candidate.md](docs/release-candidate.md) — how releases are cut and promoted
- [docs/trust-model.md](docs/trust-model.md) — bottle integrity cross-check
- [tests/harness/README.md](tests/harness/README.md) — end-to-end smoke harness
- [scripts/bench/README.md](scripts/bench/README.md) — den vs brew benchmarks
- [docs/telemetry-decisions.md](docs/telemetry-decisions.md) — what opt-in telemetry must answer (not yet shipped, 🎯T70)

To report a security vulnerability, see [SECURITY.md](SECURITY.md).

## Licence

Apache 2.0. See [LICENSE](LICENSE).

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
- **Source-first architecture** — embedded Ruby VM for evaluating
  Homebrew formula build recipes. Pre-built archives as an optimisation,
  not a requirement.
- **Multi-provider** — Homebrew is the default, but `den install`
  routes through a pluggable `PackageProvider` interface. Future
  providers (pip, npm, cargo, go) plug in without changes to the CLI,
  manifest, or environment layers. Pass `--provider <name>` to choose
  explicitly.

## Install

```bash
curl -fsSL https://raw.githubusercontent.com/marcelocantos/den/master/install.sh | sh
```

Or build from source:

```bash
git clone --recurse-submodules https://github.com/marcelocantos/den.git
cd den
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cp build/den ~/.den/bin/den
```

## Quick start

```bash
# Add to your shell (~/.zshrc or ~/.bashrc):
eval "$("$HOME/.den/bin/den" init)"

# Fetch the package index:
den update

# Install packages:
den install tree
den install ffmpeg          # resolves and installs all dependencies
den install firefox         # GUI apps — no --cask flag needed

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
den daemon status           # check for pending upgrades

# Query:
den search sqlite           # search 15,000+ packages instantly
den info ffmpeg             # show package details
den deps ffmpeg --tree      # dependency tree
den outdated                # what needs upgrading
den upgrade                 # upgrade everything
```

## How it works

Den manages package providers behind a uniform interface. Homebrew is
the default — den consumes the same archives and formulae API — but
the install / uninstall / upgrade / list / use flows all dispatch
through a pluggable `PackageProvider` so additional providers (pip,
npm, cargo, go) can plug in without changes to the CLI, manifest, or
environment-composition layers.

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
├── cache/                   # content-addressed archive cache
└── daemon.log               # background maintenance log
```

## Configuration

All settings in `~/.den/config.json`, managed via `den set`:

```bash
den set daemon.auto_download false       # disable background downloads
den set daemon.auto_upgrade true         # auto-apply upgrades
den set daemon.upgrade_window "3:00-5:00"  # when to apply
den settings                             # show all settings
```

## Commands

Run `den --help` for the full list. Key commands:

| Command | Description |
|---|---|
| `den install <pkg> [--provider <p>]` | Install a package (with dependency resolution); `--provider` routes to a specific provider |
| `den uninstall <pkg> [--provider <p>]` | Remove from the active environment |
| `den use <pkg> <version>` | Switch active version |
| `den upgrade [pkg]` | Upgrade outdated packages |
| `den list` | List packages in the active environment |
| `den search <text>` | Search packages by name or description |
| `den info <pkg>` | Show package details |
| `den env create <path>` | Create a child environment |
| `den env use <path>` | Switch active environment |
| `den env show [path]` | Show resolved packages |
| `den env freeze` | Export environment as JSON lockfile |
| `den whence <file-or-name>` | Show which package owns a file or command |
| `den self-update` | Update den to the latest release |
| `den cleanup` | Remove old versions and cache files |
| `den autoremove` | Remove unused dependencies |
| `den services list` | Show managed services |
| `den log [-n] [--json]` | Show upgrade activity log |
| `den daemon status` | Daemon and pending upgrade status |
| `den doctor` | Check system health and report issues |
| `den smoke` | Run smoke tests against installed packages |
| `den migrate` | Scan Homebrew Cellar for migration |

## Agent guide

If you use an agentic coding tool (Claude Code, Cursor, etc.), include
[`agents-guide.md`](agents-guide.md) in your project context for
den-aware assistance.

## Contributing

```bash
git clone --recurse-submodules https://github.com/marcelocantos/den.git
cd den
cmake -B build -G Ninja
cmake --build build
cd build && ctest --output-on-failure
```

Requires: cmake, ninja, libcurl, libarchive (all available via Homebrew).

To report a security vulnerability, see [SECURITY.md](SECURITY.md).

## Licence

Apache 2.0. See [LICENSE](LICENSE).

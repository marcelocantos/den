# den

A universal development environment manager. Think virtualenv for
everything — not just Python, but every package on your system.

## What den does

- **Consumes the Homebrew ecosystem** — compatible with existing
  formulae, casks, taps, and bottles. No need to rebuild anything.
- **Multi-version coinstallation** — install `python@3.11` and
  `python@3.12` side by side. Switch instantly with `den use`.
- **Named environments** — create isolated, composable environments
  that inherit from each other. Like virtualenv, but for all packages.
- **Background upgrades** — a daemon downloads new versions alongside
  existing ones. Nothing changes until you switch. Rollback is instant.

## Install

```bash
curl -fsSL https://raw.githubusercontent.com/marcelocantos/den/master/install.sh | sh
```

Or build from source:

```bash
git clone https://github.com/marcelocantos/den.git
cd den
cargo build --release
cp target/release/den ~/.den/bin/den
```

## Quick start

```bash
# First run — den detects Homebrew and offers to import your packages:
den

# Or import manually:
den migrate

# Add to your shell (~/.zshrc or ~/.bashrc):
eval "$("$HOME/.den/bin/den" init)"

# Install packages:
den install tree
den install ffmpeg          # resolves and installs all dependencies
den install --cask firefox  # GUI apps too

# Manage versions:
den install python@3.11     # installs alongside existing python@3.12
den use tree=2.3.1          # switch to a specific version

# Environments:
den env create /ml          # child of root, inherits all packages
den env use /ml             # switch to it
den install numpy           # only in /ml, root unchanged
den env use /               # back to root

# Background maintenance:
den daemon install          # auto-start at login
den set daemon.auto_upgrade true
den set daemon.upgrade_window "3:00-5:00"
den daemon status           # check for pending upgrades

# Query:
den search sqlite           # search 8000+ formulae instantly
den info ffmpeg             # show package details
den deps ffmpeg --tree      # dependency tree
den outdated                # what needs upgrading
den upgrade                 # upgrade everything
```

## How it works

Den reuses Homebrew's package ecosystem — the same bottles, the same
Cellar, the same formulae API. What's different is the layer above:

- **Manifests** declare what each environment contains. Child
  environments inherit from parents and override specific packages.
- **Materialisation** resolves the manifest hierarchy and creates a
  flat directory of symlinks into the Cellar.
- **Shell integration** sets PATH and build environment variables
  (LIBRARY_PATH, CPATH, PKG_CONFIG_PATH, etc.) to point at the
  active environment.

```
~/.den/
├── bin/den                  # the binary
├── config.json              # settings
├── manifests/               # environment definitions
│   ├── manifest.json        # / (root)
│   └── ml/
│       └── manifest.json    # /ml (inherits from /)
├── envs/                    # materialised environments
│   ├── ROOT/
│   │   ├── bin/             # symlinks to Cellar
│   │   ├── lib/
│   │   ├── include/
│   │   └── opt/
│   └── ml/
│       └── ...
├── cache/                   # content-addressed bottle cache
└── daemon.log               # background maintenance log
```

## Configuration

All settings in `~/.den/config.json`, managed via `den set`:

```bash
den set daemon.auto_upgrade true        # auto-apply upgrades
den set daemon.upgrade_window "3:00-5:00"  # when to apply
den set search.provider keyword         # search provider
den settings                            # show all settings
```

## Commands

Run `den --help` for the full list. Key commands:

| Command | Description |
|---|---|
| `den install <pkg>` | Install a formula (with dependency resolution) |
| `den install --cask <pkg>` | Install a GUI application |
| `den uninstall <pkg>` | Remove from the active environment |
| `den use <pkg>=<version>` | Switch active version |
| `den upgrade [pkg]` | Upgrade outdated packages |
| `den list` | List packages in the active environment |
| `den search <text>` | Search formulae by name or description |
| `den info <pkg>` | Show package details |
| `den env create <path>` | Create a child environment |
| `den env use <path>` | Switch active environment |
| `den env show [path]` | Show resolved packages |
| `den services list` | Show managed services |
| `den daemon status` | Daemon and pending upgrade status |
| `den migrate` | Import from Homebrew |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions and PR guidelines.
To report a security vulnerability, see [SECURITY.md](SECURITY.md).

## Licence

Apache 2.0. See [LICENSE](LICENSE).

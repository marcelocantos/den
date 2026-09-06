# den — Agent Guide

den is a universal development environment manager that replaces
Homebrew, pyenv, nvm, virtualenv, and similar tools with a single
binary.

## Key concepts

- **Cellar**: Shared package storage at `/opt/homebrew/Cellar/<name>/<version>/`
  (`$HOMEBREW_CELLAR`; `/usr/local/Cellar` on Intel Macs,
  `/home/linuxbrew/.linuxbrew/Cellar` on Linux). Den uses the same
  Cellar as Homebrew — bottles pour at their expected prefix with zero
  relocation.
- **Environment**: A named set of symlinks into the Cellar.
  Environments use path-based naming (`/` is root, `/ml`,
  `/work/legacy`) with manifest-level inheritance.
- **Manifest**: JSON file declaring which packages an environment
  contains, grouped per provider:
  `{ "packages": { "homebrew": {"ffmpeg": "6.1.1"}, "pip": {...} },
  "auto_deps": { "homebrew": ["x264"] } }`.
  Child environments inherit from parents and can override specific
  versions. A legacy flat `packages: {name: ver}` block (pre-v0.11.0)
  is auto-promoted under the `homebrew` provider on read.
- **Materialisation**: Resolving a manifest hierarchy into a flat
  symlink directory. The env materialiser asks each owning provider
  for its package root and binary paths, then symlinks them into the
  env — Homebrew kegs and packages from other providers flow through
  the same pipeline.
- **Unified package model**: No formula/cask distinction. All
  packages are installed with `den install <name>`.
- **Providers**: Pluggable backends behind a `PackageProvider`
  interface. Registered names are `homebrew` (default), `pip`, `npm`,
  `go`, `cargo` and `stub` (a no-op that proves the seam). The
  auxiliary providers never auto-claim a bare name: pass
  `--provider <name>` to install / uninstall / upgrade, or let a
  manifest hint route it; otherwise Homebrew is the fallback.
- **Trust**: every bottle install cross-checks the Homebrew API hash
  against a replica, `data/known_hashes.json`; disagreement refuses the
  install, an absent replica warns and proceeds. `cmake --install`
  places it at `share/den/known_hashes.json`, but the release tarball
  and `install.sh` do not ship it, so a curl-installed den runs in the
  warn-and-proceed mode unless `~/.den/trust/known_hashes.json` is
  provided (entropy audit ENT-002). `den doctor` shows the `[trust]`
  block. See [docs/trust-model.md](docs/trust-model.md).

## Common operations

```bash
den install <name>           # install with dependency resolution
den install -s <name>        # build from source
den install <name> --provider <p>   # route to a specific provider
den uninstall <name>         # remove from active environment
den use <pkg> <version>      # switch active version
den upgrade                  # upgrade all outdated packages
den run <cmd> [args...]      # run a binary from the active environment
den env create <path>        # create child environment
den env use <path>           # switch environment
den env show [path]          # show resolved packages
den env freeze               # export environment as JSON lockfile
den search <text>            # search the local index
den info <name>              # package details
den deps <name> --tree       # dependency tree
den deps <name> --explain    # solver clauses, preferences, conflicts
den list [--cellar]          # installed packages; --cellar adds disk/refs/orphans
den outdated                 # packages with available (or deferred) upgrades
den update                   # fetch latest package index
den tap add <user/repo>      # register a third-party tap (--list, --remove)
den cleanup                  # remove old versions and cache files
den autoremove               # remove unneeded dependencies
den migrate [--dry-run]      # import formulae, casks, taps, services from Homebrew
den daemon status            # background maintenance status
den daemon install           # start the daemon at login
den services list            # services under the built-in supervisor
den log [--json]             # upgrade activity log
den whence <file-or-cmd>     # owning package
den config                   # detected host configuration
den set <key> <value>        # configure settings
den settings                 # show all settings
den doctor                   # system health checks (incl. trust)
den self-update [--check]    # update den
den smoke                    # run smoke tests
```

## Configuration keys (`~/.den/config.json`, via `den set`)

| Key | Type | Default |
|---|---|---|
| `daemon.auto_download` | bool | `true` |
| `daemon.auto_upgrade` | bool | `false` |
| `daemon.upgrade_window` | `"HH:MM-HH:MM"` | unset |
| `daemon.interval_secs` | integer | unset (built-in interval) |
| `search.provider` | string | unset |
| `taps` | object `{ "user/repo": source }` | managed by `den tap` |

## Environment variables

- `DEN_HOME`: den's home directory (default: `~/.den`)
- `DEN_ENV`: currently active environment path (set by `den init`)
- `DEN_SHELL`: shell name used by the init wrapper when `$SHELL` is unreliable
- `HOMEBREW_PREFIX`, `HOMEBREW_CELLAR`, `HOMEBREW_CASKROOM`: override the
  shared store locations
- `DEN_VERSION`, `DEN_INSTALL_DIR`, `DEN_NO_MODIFY_PATH`: `install.sh` only

## File layout

```
/opt/homebrew/Cellar/    # shared package store
└── <name>/<ver>/        # each version self-contained

~/.den/
├── bin/den              # binary
├── config.json          # settings
├── manifests/           # environment manifests (<slug>/manifest.json)
├── envs/                # materialised environments
├── cache/               # index.json + content-addressed archive cache
├── taps/<user>/<repo>/  # third-party tap clones
├── activity.json        # upgrade activity log
├── daemon.pid           # daemon process ID
├── daemon.log           # daemon log
└── daemon_state.json    # pending / deferred upgrades
```

## Build

```bash
git submodule update --init vendor/github.com/gabime/spdlog
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires: cmake, ninja, libcurl, libarchive.
Optional: Portable Ruby (for formula evaluation / source builds); on
macOS it is embedded, on Linux it is downloaded on first use.
Repo layout and gates: [AGENTS.md](AGENTS.md).

# den — Agent Guide

den is a universal development environment manager that replaces
Homebrew, pyenv, nvm, virtualenv, and similar tools with a single
binary.

## Key concepts

- **Cellar**: Shared package storage at `/opt/homebrew/Cellar/<name>/<version>/`.
  Den uses the same Cellar as Homebrew — bottles pour at their expected
  prefix with zero relocation.
- **Environment**: A named set of symlinks into the Cellar.
  Environments use path-based naming (`/` is root, `/ml`,
  `/work/legacy`) with manifest-level inheritance.
- **Manifest**: JSON file declaring which packages an environment
  contains. Child environments inherit from parents and can override
  specific versions.
- **Materialisation**: Resolving a manifest hierarchy into a flat
  symlink directory.
- **Unified package model**: No formula/cask distinction. All
  packages are installed with `den install <name>`.

## Common operations

```bash
den install <name>           # install with dependency resolution
den uninstall <name>         # remove from active environment
den use <pkg> <version>      # switch active version
den upgrade                  # upgrade all outdated packages
den env create <path>        # create child environment
den env use <path>           # switch environment
den env show [path]          # show resolved packages
den env freeze               # export environment as JSON lockfile
den search <text>            # search 15,000+ packages
den info <name>              # package details
den deps <name> --tree       # dependency tree
den list                     # list installed packages
den outdated                 # list packages with available upgrades
den update                   # fetch latest package index
den cleanup                  # remove old versions and cache files
den autoremove               # remove unneeded dependencies
den migrate                  # scan Homebrew Cellar for migration
den daemon status            # background maintenance status
den config                   # show den configuration
den set <key> <value>        # configure settings
den settings                 # show all settings
den doctor                   # system health checks
den smoke                    # run smoke tests
```

## Environment variables

- `DEN_HOME`: den's home directory (default: `~/.den`)
- `DEN_ENV`: currently active environment path

## File layout

```
/opt/homebrew/Cellar/    # shared package store
└── <name>/<ver>/        # each version self-contained

~/.den/
├── bin/den              # binary
├── config.json          # settings
├── manifests/           # environment manifests
├── envs/                # materialised environments
├── cache/archives/      # content-addressed archive cache
├── activity.json        # upgrade activity log
├── daemon.pid           # daemon process ID
├── daemon.log           # daemon log
└── daemon_state.json    # pending upgrades
```

## Build

```bash
cmake -B build -G Ninja
cmake --build build
cd build && ctest --output-on-failure
```

Requires: cmake, ninja, libcurl, libarchive.
Optional: Portable Ruby (for formula evaluation / source builds).

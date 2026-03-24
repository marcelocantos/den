# den — Agent Guide

den is a universal development environment manager that replaces
Homebrew, pyenv, nvm, virtualenv, and similar tools with a single
binary.

## Key concepts

- **Cellar**: Storage for all installed package versions (shared
  with Homebrew at `/opt/homebrew/Cellar`).
- **Environment**: A named set of symlinks into the Cellar.
  Environments use path-based naming (`/` is root, `/ml`,
  `/work/legacy`) with manifest-level inheritance.
- **Manifest**: JSON file declaring which packages an environment
  contains. Child environments inherit from parents and can override
  specific versions.
- **Materialisation**: Resolving a manifest hierarchy into a flat
  symlink directory.

## Common operations

```bash
den install <formula>        # install with dependency resolution
den install --cask <app>     # install GUI application
den uninstall <formula>      # remove from active environment
den use <pkg>=<version>      # switch active version
den upgrade                  # upgrade all outdated packages
den env create <path>        # create child environment
den env use <path>           # switch environment
den env show [path]          # show resolved packages
den search <text>            # search 8000+ formulae
den info <formula>           # package details
den deps <formula> --tree    # dependency tree
den list                     # list installed packages
den outdated                 # list packages with available upgrades
den update                   # fetch latest formula index
den cleanup [formula...]     # remove old versions and cache files
den autoremove               # remove unneeded dependencies
den migrate                  # import from Homebrew
den daemon status            # background maintenance status
den config                   # show den configuration
den set <key> <value>        # configure settings
den settings                 # show all settings
```

## Environment variables

- `DEN_HOME`: den's home directory (default: `~/.den`)
- `DEN_ENV`: currently active environment path
- `HOMEBREW_PREFIX`: Homebrew installation prefix
- `HOMEBREW_CELLAR`: Cellar location

## File layout

```
~/.den/
├── bin/den              # binary
├── config.json          # settings
├── manifests/           # environment manifests
├── envs/                # materialised environments
├── cache/bottles/       # content-addressed bottle cache
├── daemon.pid           # daemon process ID
├── daemon.log           # daemon log
└── daemon_state.json    # pending upgrades
```

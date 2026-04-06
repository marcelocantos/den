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

## Architecture

See `docs/targets.md` for the convergence roadmap.

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

### Crate Structure

```
src/
├── main.rs           # CLI entry point
├── cli/              # Command definitions and handlers
│   ├── mod.rs        # Cli struct, Command enum, dispatch
│   ├── install.rs    # install, pour, uninstall, upgrade
│   ├── query.rs      # info, search, deps, list, use, cleanup
│   ├── env_cmd.rs    # environment management commands
│   ├── daemon_cmd.rs # daemon lifecycle commands
│   ├── shell.rs      # shell init and env switch output
│   ├── services.rs   # service list/start/stop/restart
│   └── migrate.rs    # Homebrew cellar migration
├── env/              # Environment management (create, switch, layer)
├── formula/          # Formula parsing, metadata, DSL
├── cask/             # Cask parsing, artifacts
├── keg/              # Keg/Cellar management
├── tap/              # Tap (git repo) management (placeholder)
├── bottle/           # Bottle download, pour, relocation
├── deps/             # Dependency resolution, topological sort
├── link/             # Symlink management
├── download/         # Download strategies, caching (placeholder)
├── api/              # formulae.brew.sh API client
├── tab/              # INSTALL_RECEIPT.json handling
├── daemon/           # Background upgrade service
├── config/           # Configuration, environment
├── manifest/         # Manifest read/write, environment hierarchy resolution
├── service/          # Service management (launchd plists)
├── settings/         # Unified settings (config.json)
├── platform/         # OS/arch detection, Mach-O/ELF tools
└── error.rs          # Error types
```

## Build

```bash
cargo build
cargo test
```

## TODO Location

docs/TODO.md

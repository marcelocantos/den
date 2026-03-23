# rubrew

A Rust reimplementation of Homebrew, the macOS package manager.

## Overview

rubrew aims to be a drop-in replacement for Homebrew with full
compatibility with existing formulae, casks, taps, and bottles. Written
in Rust for speed, correctness, and resource efficiency.

## Delivery

delivery: merged to master, CI green

## Gates

profile: cli

## Architecture

See `docs/targets.md` for the convergence roadmap.

### Key Design Decisions

- **Formula compatibility**: Must support existing Homebrew Ruby formula
  DSL — parse `.rb` files to extract metadata, delegate `install` and
  `test` blocks to a sandboxed Ruby evaluator (or transpile to a native
  representation over time).
- **Filesystem layout**: 100% compatible with Homebrew's Cellar/opt/tap
  layout so users can migrate in place.
- **Bottle compatibility**: Pour existing Homebrew bottles unchanged.
- **API compatibility**: Consume `formulae.brew.sh` JSON API.
- **CLI compatibility**: Same commands, same flags, same output format
  where possible.

### Crate Structure

```
src/
├── main.rs           # CLI entry point
├── cli/              # Command definitions (clap)
├── formula/          # Formula parsing, metadata, DSL
├── cask/             # Cask parsing, artifacts
├── keg/              # Keg/Cellar management
├── tap/              # Tap (git repo) management
├── bottle/           # Bottle download, pour, relocation
├── deps/             # Dependency resolution, topological sort
├── link/             # Symlink management
├── download/         # Download strategies, caching
├── api/              # formulae.brew.sh API client
├── tab/              # INSTALL_RECEIPT.json handling
├── config/           # Configuration, environment
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

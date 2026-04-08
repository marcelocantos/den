# Stability

## Commitment

Once den reaches 1.0, backwards compatibility becomes a binding
contract. Breaking changes to the CLI interface, configuration format,
manifest format, or environment layout will not occur in minor or patch
releases. The pre-1.0 period exists to get these surfaces right before
locking them in.

## Interaction surface catalogue

Snapshot as of v0.6.0.

### CLI commands

| Command | Stability | Notes |
|---|---|---|
| `den install [-s] <names...>` | Stable | Unified model — no --cask flag |
| `den uninstall <names...>` | Stable | |
| `den upgrade [names...]` | Stable | |
| `den update` | Stable | Fetches Homebrew formula + cask index |
| `den list` | Stable | |
| `den info <name>` | Stable | |
| `den search <text>` | Stable | |
| `den deps <name> [--tree]` | Stable | |
| `den cleanup` | Stable | Removes old versions and cache |
| `den autoremove` | Stable | Removes unreferenced auto-deps |
| `den doctor` | Stable | |
| `den config` | Stable | |
| `den env create <path>` | Stable | |
| `den env list` | Stable | |
| `den env remove <path>` | Stable | |
| `den env use <path>` | Stable | |
| `den env show [path]` | Stable | |
| `den env freeze` | Needs review | JSON lockfile output format not finalised |
| `den use <pkg> <version>` | Stable | |
| `den init [--shell <shell>]` | Stable | |
| `den status` | Stable | Environment + daemon summary |
| `den set <key> <value>` | Stable | |
| `den settings` | Stable | |
| `den migrate` | Needs review | Will evolve as piecemeal migration develops |
| `den daemon run\|stop\|status\|install\|uninstall` | Stable | |
| `den outdated` | Stable | |
| `den services list\|start\|stop\|restart` | Needs review | Will be replaced by built-in supervisor (🎯T33) |
| `den whence <file-or-name>` | Stable | Resolves file/command to owning package |
| `den self-update` | Stable | Downloads and replaces the den binary |
| `den smoke [--defs] [-n]` | Fluid | Internal testing tool, may change |
| `den --version` | Stable | |
| `den --help` | Stable | |
| `den --help-agent` | Stable | |

### Global flags

| Flag | Type | Stability |
|---|---|---|
| `--help-agent` | bool | Stable |
| `-h, --help` | bool | Stable |
| `-V, --version` | bool | Stable |

### Configuration (`~/.den/config.json`)

| Key | Type | Default | Stability |
|---|---|---|---|
| `daemon.auto_download` | bool | `true` | Stable |
| `daemon.auto_upgrade` | bool | `false` | Stable |
| `daemon.upgrade_window` | string? | `null` | Stable |
| `daemon.interval_secs` | u64? | `null` | Stable |
| `search.provider` | string? | `null` | Stable |

### Environment variables

| Variable | Stability | Notes |
|---|---|---|
| `DEN_HOME` | Stable | Override den home directory |
| `DEN_ENV` | Stable | Currently active environment path |

### File formats

| File | Stability | Notes |
|---|---|---|
| `manifests/<slug>/manifest.json` | Needs review | Schema: `{packages: {name: version}, auto_deps: [name]}` |
| `config.json` | Stable | See Configuration section |
| `daemon_state.json` | Needs review | Internal daemon state |
| `daemon.pid` | Stable | Plain text PID |
| `daemon.log` | Stable | Plain text log |

### Directory layout (`~/.den/`)

| Path | Stability | Notes |
|---|---|---|
| `bin/` | Stable | den binary |
| `store/` | Stable | Installed packages (`store/<name>/<version>/`) |
| `manifests/` | Stable | Environment manifest files |
| `envs/` | Stable | Materialised environment directories |
| `cache/archives/` | Stable | Content-addressed archive cache |
| `config.json` | Stable | Settings |
| `daemon.pid` | Stable | Daemon PID file |
| `daemon.log` | Stable | Daemon log |

## Gaps and prerequisites for 1.0

- **Source builds**: Currently installs pre-built archives only.
  Packages with hardcoded prefixes (12.6% of Homebrew corpus) fail
  at runtime. Ruby VM embedding is prototyped but not yet wired into
  the install flow.
- **Services redesign**: Current launchd-based services (🎯T16) will
  be replaced by built-in supervisor (🎯T33).
- **Manifest schema**: The manifest JSON format should be documented
  and versioned before locking in.
- **Error messages**: Need user-friendly error messages throughout.
- **Shell completions**: No shell completion generation yet.
- **`env freeze` output**: Format not finalised.
- **`owner/repo` install**: GitHub-hosted packages via `den.json`
  not yet implemented.
- **Cross-platform**: macOS is the primary target. Linux support is
  for archive installs only (no daemon, no services, no Ruby embedding).

## Out of scope for 1.0

- Multi-provider package management (🎯T23) — Go, Cargo, pip, npm.
- Semantic search (🎯T24) — keyword search is sufficient.
- Content-addressed store (🎯T26).
- SAT-based dependency solver (🎯T29).
- Binary transparency log (🎯T44.2).
- Reproducible builds (🎯T44.3).
- Opt-in telemetry (🎯T32).
- Shim-free build toolchain (🎯T45).

# Stability

## Commitment

Once den reaches 1.0, backwards compatibility becomes a binding
contract. Breaking changes to the CLI interface, configuration format,
manifest format, or environment layout will not occur in minor or patch
releases. The pre-1.0 period exists to get these surfaces right before
locking them in.

## Interaction surface catalogue

Snapshot as of v0.1.0.

### CLI commands

| Command | Stability | Notes |
|---|---|---|
| `den install [--cask] [-s] <names...>` | Needs review | `--build-from-source` is a stub |
| `den uninstall <names...>` | Stable | |
| `den upgrade [names...]` | Stable | |
| `den update` | Stable | |
| `den list` | Stable | |
| `den info <name>` | Stable | |
| `den search <text>` | Stable | |
| `den deps <name> [--tree]` | Stable | |
| `den tap <url>` | Fluid | Placeholder implementation |
| `den untap <name>` | Fluid | Placeholder implementation |
| `den link <name>` | Fluid | Not yet implemented |
| `den unlink <name>` | Fluid | Not yet implemented |
| `den cleanup [names...]` | Stable | |
| `den autoremove` | Stable | |
| `den doctor` | Stable | |
| `den config` | Stable | |
| `den env create <path>` | Stable | |
| `den env list` | Stable | |
| `den env remove <path>` | Stable | |
| `den env use <path>` | Stable | |
| `den env show [path]` | Stable | |
| `den env freeze [path]` | Needs review | Output format not finalised |
| `den use <pkg>=<version>` | Stable | |
| `den init [--shell <shell>]` | Stable | |
| `den status` | Fluid | Not yet implemented |
| `den set <key> <value>` | Stable | |
| `den settings` | Stable | |
| `den migrate` | Stable | |
| `den daemon run\|stop\|status\|install\|uninstall` | Stable | |
| `den outdated` | Stable | |
| `den services list\|start\|stop\|restart` | Needs review | Will be replaced by built-in supervisor (🎯T33) |
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
| `HOMEBREW_PREFIX` | Stable | Homebrew installation prefix |
| `HOMEBREW_CELLAR` | Stable | Cellar location |

### File formats

| File | Stability | Notes |
|---|---|---|
| `manifests/*/manifest.json` | Needs review | Schema: `{packages: {name: version}, casks: {name: version}, auto: [name]}` |
| `config.json` | Stable | See Configuration section |
| `daemon_state.json` | Needs review | Internal daemon state, not user-facing contract |
| `daemon.pid` | Stable | Plain text PID |
| `daemon.log` | Stable | Plain text log |

### Directory layout (`~/.den/`)

| Path | Stability | Notes |
|---|---|---|
| `bin/` | Stable | den binary |
| `manifests/` | Stable | Environment manifest files |
| `envs/` | Stable | Materialised environment directories |
| `cache/bottles/` | Stable | Content-addressed bottle cache |
| `config.json` | Stable | Settings |
| `daemon.pid` | Stable | Daemon PID file |
| `daemon.log` | Stable | Daemon log |
| `daemon_state.json` | Needs review | Daemon state |
| `trust/` | Needs review | Trust model files |

## Gaps and prerequisites for 1.0

- **Unimplemented commands**: `link`, `unlink`, `status` are declared
  in the CLI but return "not yet implemented". Must be implemented or
  removed before 1.0.
- **Tap management**: `tap`/`untap` are placeholders. Need full
  implementation or removal.
- **Services redesign**: Current launchd-based services (🎯T16) will
  be replaced by built-in supervisor (🎯T33). The `den services`
  subcommand surface will change.
- **Manifest schema**: The manifest JSON format should be documented
  and versioned before locking in.
- **Error messages**: Many error paths use raw `anyhow::bail!` without
  user-friendly messages. Polish before 1.0.
- **Shell completions**: No shell completion generation yet.
- **`env freeze` output**: Format not finalised — should produce a
  lockfile that can reproduce the environment exactly.
- **Cross-platform**: Currently macOS-focused. Linux support works for
  bottle pours but daemon/services are macOS-only (launchd).

## Out of scope for 1.0

- Multi-provider package management (🎯T23) — Go, Cargo, pip, npm
  providers are post-1.0.
- Semantic search (🎯T24) — keyword search is sufficient for 1.0.
- Content-addressed Cellar (🎯T26) — current Cellar layout works.
- SAT-based dependency solver (🎯T29) — greedy DFS is adequate for
  Homebrew's dependency graph.
- Binary transparency log (🎯T44.2).
- Reproducible bottle builds (🎯T44.3).
- Opt-in telemetry (🎯T32).

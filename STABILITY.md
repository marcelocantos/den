# Stability

## Commitment

Once den reaches 1.0, backwards compatibility becomes a binding
contract. Breaking changes to the CLI interface, configuration format,
manifest format, or environment layout will not occur in minor
releases. The pre-1.0 period exists to get these surfaces right before
locking them in. All releases are minor (`X.Y.0`); the RC pipeline
rejects patch versions ([docs/release-candidate.md](docs/release-candidate.md)).

## Interaction surface catalogue

Snapshot as of v0.13.0 (2026-09-06), taken from `src/cli/cli.cpp`,
`src/settings/settings.h` and `src/core/config.cpp`.

### CLI commands

| Command | Stability | Notes |
|---|---|---|
| `den install [-s\|--build-from-source] [--provider <p>] <names...>` | Stable | Unified model — no --cask flag |
| `den uninstall [--provider <p>] <names...>` | Stable | |
| `den upgrade [--provider <p>] [names...]` | Stable | |
| `den update` | Stable | Fetches Homebrew formula + cask index |
| `den list [--cellar]` | Stable | `--cellar` adds per-keg disk usage, references, orphans |
| `den run <cmd> [args...]` | Stable | Runs from the active environment |
| `den info <name>` | Stable | |
| `den search <text>` | Stable | Substring match on name + description |
| `den deps <names...> [--tree] [--explain]` | Stable | `--explain` prints SAT clauses and conflicts |
| `den cleanup` | Stable | Removes old versions and cache |
| `den autoremove` | Stable | Removes unreferenced auto-deps |
| `den doctor` | Stable | Includes the `[trust]` block |
| `den config` | Stable | Host toolchain and Homebrew path facts |
| `den env create <path>` | Stable | |
| `den env list` | Stable | |
| `den env remove <path>` | Stable | |
| `den env use <path>` | Stable | |
| `den env show [path]` | Stable | |
| `den env freeze` | Needs review | JSON lockfile output format not finalised |
| `den use <pkg> <version>` | Stable | |
| `den init [--shell bash\|zsh\|fish]` | Stable | |
| `den shell-env [env]` | Internal | Emitted by `den init`'s wrapper; not for direct use |
| `den status` | Stable | Environment + daemon summary |
| `den set <key> <value>` | Stable | |
| `den settings` | Stable | |
| `den tap add <user/repo> [source]`, `den tap --list`, `den tap --remove <user/repo>` | Needs review | Registered in `config.json` `taps`; clones under `~/.den/taps/` |
| `den migrate [names...] [--dry-run] [--integrate-shell] [--no-health-check]` | Needs review | Non-destructive and idempotent; will evolve with piecemeal migration |
| `den daemon run\|stop\|status\|install\|uninstall` | Stable | `install` registers a login service on macOS |
| `den outdated` | Stable | Lists deferred (in-use) upgrades with the reason |
| `den services list\|start\|stop [--timeout]\|restart\|status\|logs [-f]` | Needs review | Built-in supervisor (🎯T33 landed); hardening under 🎯T61, restart-after-upgrade 🎯T52 |
| `den whence <file-or-name>` | Stable | Resolves file/command to owning package |
| `den self-update [--check\|--dry-run]` | Stable | Downloads and replaces the den binary |
| `den log [-n <count>] [--json]` | Stable | Upgrade activity log |
| `den smoke [--defs FILE] [-n\|--max N]` | Fluid | Internal testing tool, may change |
| `den replica-verify` | Internal | Hidden; used by `replica-verify.yml` |
| `den --version` | Stable | |
| `den --help` | Stable | |
| `den --help-agent` | Stable | `--help` plus a pointer to `agents-guide.md` |

### Global flags

| Flag | Type | Stability |
|---|---|---|
| `--help-agent` | bool | Stable |
| `-h, --help` | bool | Stable |
| `--version` | bool | Stable |

### Configuration (`~/.den/config.json`)

| Key | Type | Default | Stability |
|---|---|---|---|
| `daemon.auto_download` | bool | `true` | Stable |
| `daemon.auto_upgrade` | bool | `false` | Stable |
| `daemon.upgrade_window` | string? | `null` | Stable |
| `daemon.interval_secs` | u64? | `null` | Stable |
| `search.provider` | string? | `null` | Stable |
| `taps` | object `{ "user/repo": source }` | `{}` | Needs review |

### Environment variables

| Variable | Stability | Notes |
|---|---|---|
| `DEN_HOME` | Stable | Override den home directory |
| `DEN_ENV` | Stable | Currently active environment path; set by `den init` |
| `DEN_SHELL` | Needs review | Shell hint for the init wrapper |
| `HOMEBREW_PREFIX`, `HOMEBREW_CELLAR`, `HOMEBREW_CASKROOM` | Stable | Override the shared store, as for Homebrew itself |
| `DEN_VERSION`, `DEN_INSTALL_DIR`, `DEN_NO_MODIFY_PATH` | Stable | `install.sh` only |

### File formats

| File | Stability | Notes |
|---|---|---|
| `manifests/<slug>/manifest.json` | Needs review | `{packages: {provider: {name: version}}, auto_deps: {provider: [name]}}`; legacy flat `packages` promoted under `homebrew` on read |
| `config.json` | Stable | See Configuration section |
| `daemon_state.json` | Needs review | Internal daemon state |
| `daemon.pid` | Stable | Plain text PID |
| `daemon.log` | Stable | Plain text log |
| `activity.json` | Stable | Structured upgrade activity log |
| `cache/index.json` | Internal | Package index fetched by `den update` |

### Directory layout (`~/.den/`)

| Path | Stability | Notes |
|---|---|---|
| `bin/` | Stable | den binary |
| `manifests/` | Stable | Environment manifest files |
| `envs/` | Stable | Materialised environment directories |
| `cache/` | Stable | Index and content-addressed archive cache |
| `taps/<user>/<repo>/` | Needs review | Third-party tap clones |
| `config.json` | Stable | Settings |
| `daemon.pid`, `daemon.log`, `daemon_state.json` | Stable | Daemon files |
| `activity.json` | Stable | Upgrade activity log |

### Package store

| Path | Stability | Notes |
|---|---|---|
| `/opt/homebrew/Cellar/<name>/<version>/` | Stable | Shared Cellar with Homebrew (🎯T49). den and brew coexist; den tracks what it manages via `~/.den/` manifests. |
| `share/den/known_hashes.json` (beside `bin/`), or `~/.den/trust/known_hashes.json` | Needs review | Trust replica snapshot; placed by `cmake --install` but not by the release tarball or `install.sh` (entropy audit ENT-002) |

## Gaps and prerequisites for 1.0

Open targets in `bullseye.yaml`; 🎯T75 (RC end-to-end validation) is
converging and 🎯T76 (stable release) waits on it.

- **Manifest schema**: the format above should be versioned before
  locking in.
- **`env freeze` output**: format not finalised.
- **Go provider**: bare aliases install; full module paths
  (`host/path@version`) do not (🎯T77).
- **Native formula parser**: dependency extraction and no-Ruby SIMPLE
  installs still lag the Ruby path (🎯T58, 🎯T59).
- **Supervisor hardening** and restart-after-upgrade (🎯T61, 🎯T52).
- **Shell completions**: none yet.
- **Cross-platform**: macOS is the primary target. Linux installs
  archives and lazily downloads Portable Ruby; `den daemon install` is
  launchd-based and therefore macOS-only.
- **Trust replica on the shipped path**: the release tarball and
  `install.sh` omit `known_hashes.json`, so curl-installed users get the
  warn-and-proceed mode (entropy audit ENT-002).

## Out of scope for 1.0

- Semantic search (🎯T24, 🎯T62) and the search corpus pipeline (🎯T25).
- Content-addressed Cellar with explicit bindings (🎯T26, 🎯T28, 🎯T69).
- Daemon socket API (🎯T34).
- Advanced trust layers — transparency log, reproducible builds (🎯T44).
- Opt-in telemetry (🎯T32, 🎯T70; decision contract in
  [docs/telemetry-decisions.md](docs/telemetry-decisions.md)).
- Bottle relocation at scale (🎯T48, set aside).

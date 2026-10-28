# Stability

## Commitment

Once den reaches 1.0, backwards compatibility becomes a binding
contract. Breaking changes to the CLI interface, configuration format,
manifest format, or environment layout will not occur in minor or patch
releases. The pre-1.0 period exists to get these surfaces right before
locking them in.

## Interaction surface catalogue

Snapshot as of v0.10.0.

### CLI commands

| Command | Stability | Notes |
|---|---|---|
| `den install [-s] <names...>` | Stable | Unified model — no --cask flag |
| `den install --overwrite <names...>` | Needs review | Replaces prefix paths that conflict with the named packages (brew's `link --overwrite`) |
| `den uninstall <names...> [--ignore-dependencies]` | Stable | Refuses (non-zero exit, dependents listed) while installed Homebrew packages in any environment need the package at runtime, unless their environment still gets it elsewhere; build-only dependencies don't count; `--ignore-dependencies` overrides. Drops from the active environment; keg kept until `den cleanup`. If no environment still uses that version, den hands its own `opt/<name>` link to another installed den-owned, still-used version, or removes it. Never touches an `opt/` link to a keg den did not install. Does not autoremove dependencies |
| `den upgrade [names...]` | Stable | |
| `den update` | Stable | Fetches Homebrew formula + cask index; migrates an old `envs/ROOT` layout into the prefix once |
| `den list` | Stable | |
| `den info <name>` | Stable | |
| `den search <text>` | Stable | |
| `den deps <name> [--tree]` | Stable | |
| `den cleanup` | Stable | Removes den-owned unreferenced versions and the archive cache; never deletes kegs den did not install |
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
| `den get <key>` | Stable | Exit 0 = set, 1 = not set, 2 = unreadable/invalid `config.json` |
| `den settings` | Stable | |
| `den migrate` | Needs review | Records formulae in the root manifest (`manifests/ROOT/manifest.json`) and casks/taps in `config.json` |
| `den daemon run\|stop\|status\|install\|uninstall` | Stable | |
| `den outdated` | Stable | |
| `den services list\|start\|stop\|restart` | Needs review | Will be replaced by built-in supervisor (🎯T33) |
| `den whence <file-or-name>` | Stable | Resolves file/command to owning package |
| `den self-update` | Stable | Downloads and replaces the den binary |
| `den log [-n <count>] [--json]` | Stable | Upgrade activity log |
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
| `store.prefix` | string? | `null` | Needs review |
| `taps` | object (name → source) | `{}` | Needs review |
| `casks` | object (token → version) | `{}` | Needs review |

### Environment variables

| Variable | Stability | Notes |
|---|---|---|
| `DEN_HOME` | Stable | Override den home directory |
| `DEN_PREFIX` | Needs review | Override den's prefix (store = `$DEN_PREFIX/Cellar`); beats `store.prefix` |
| `HOMEBREW_PREFIX` / `HOMEBREW_CELLAR` / `HOMEBREW_CASKROOM` | Stable | Locate Homebrew for `den migrate` and diagnostics only; never select den's store |
| `DEN_ENV` | Stable | Currently active environment path |

### File formats

| File | Stability | Notes |
|---|---|---|
| `manifests/<slug>/manifest.json` | Needs review | Schema: `{packages: {provider: {name: version}}, auto_deps: {provider: [name]}}`. The root environment is `manifests/ROOT/manifest.json`. |
| `config.json` | Stable | See Configuration section |
| `daemon_state.json` | Needs review | Internal daemon state |
| `daemon.pid` | Stable | Plain text PID |
| `daemon.log` | Stable | Plain text log |
| `activity.json` | Stable | Structured upgrade activity log |

### Directory layout (`~/.den/`)

| Path | Stability | Notes |
|---|---|---|
| `bin/` | Stable | den binary |
| `manifests/` | Stable | Environment manifest files |
| `envs/` | Stable | Materialised named environment directories |
| `envs/ROOT/` | Needs review | Compat shim: `bin`, `sbin`, `lib`, … are symlinks to the prefix's own dirs; `.den/` holds the root env's link ledgers |
| `cache/archives/` | Stable | Content-addressed archive cache |
| `config.json` | Stable | Settings |
| `daemon.pid` | Stable | Daemon PID file |
| `daemon.log` | Stable | Daemon log |
| `activity.json` | Stable | Upgrade activity log |

### Package store

| Path | Stability | Notes |
|---|---|---|
| `<prefix>/Cellar/<name>/<version>/` | Stable | den's store. `<prefix>` resolves as `DEN_PREFIX` > `config.json` `store.prefix` > `$DEN_HOME/prefix` (if populated) > legacy shared Homebrew Cellar > `$DEN_HOME/prefix` (default `~/.den/prefix`). `HOMEBREW_*` env vars never select it. |
| `/opt/homebrew/Cellar/<name>/<version>/` | Stable | Shared Cellar with Homebrew (🎯T49) — opt-in: `den migrate` pins `store.prefix` to Homebrew's prefix, after which den and brew coexist and den tracks what it manages via `~/.den/` manifests. Pre-isolation den homes with kegs there keep using it. |

## Behaviour changes

- **Root environment linked into the prefix.** The root environment (`/`)
  is now linked into the prefix itself (`<prefix>/bin`, `sbin`, `lib`,
  `include`, `share`, `etc`, `var`, `Frameworks`, plus `opt`), the way
  `brew link` does, instead of into `~/.den/envs/ROOT/`. Tools that derive
  paths from their own prefix (automake's `aclocal --print-ac-dir`,
  findutils' `locate` database, ...) now find what other packages
  installed. `den shell-env` puts `<prefix>/bin:<prefix>/sbin` first on
  PATH, and `lib`, `include`, `pkgconfig`, `man` and `info` come from the
  prefix. Link conflicts follow brew: a link into a den-managed keg is
  replaced; any other link or real file blocks that package's link with a
  list of the conflicting paths, and only `den install --overwrite <pkg>`
  replaces them. Uninstall removes only links den recorded. Existing
  config files under `etc` and `var` are kept. `envs/ROOT/{bin,lib,...}`
  remain as symlinks to the prefix dirs, so scripts that use them keep
  working. Existing installs are migrated once by `den update`; `den
  doctor` reports a pending migration and a PATH that does not lead with
  the prefix. Named environments are unchanged.

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

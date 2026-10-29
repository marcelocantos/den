# den

A universal development environment manager. Think virtualenv for
everything — not just Python, but every package on your system.

## What den does

- **Isolated store by default** — den pours kegs into its own Cellar at
  `$DEN_HOME/prefix/Cellar` (default `~/.den/prefix/Cellar`) and never
  writes to `/opt/homebrew` unless you point it there (`den migrate`
  pins the shared Homebrew Cellar; see
  [Store location](#store-location-and-env-precedence)). Bottles still carry Homebrew
  placeholders, which den rewrites on pour: on macOS it rewrites Mach-O
  install names with `otool`/`install_name_tool` and pins `opt/<dep>`
  references to the exact dependency keg; on Linux it patches ELF paths
  with patchelf. Den writes `<prefix>/opt/<name>` links only for kegs
  it poured itself, and never repoints one that points at a keg
  Homebrew installed.
- **Unified package model** — no formula/cask distinction. Just
  `den install firefox` or `den install ffmpeg`. Cask apps go to
  `DEN_APPDIR` (default `/Applications`), staged under
  `~/.den/caskroom/`, never Homebrew's Caskroom; den only replaces or
  removes apps that its own receipt names. `.pkg` casks still need admin.
- **Multi-version coinstallation** — install `python@3.11` and
  `python@3.12` side by side. Switch instantly with `den use`.
- **Named environments** — create isolated, composable environments
  that inherit from each other. Like virtualenv, but for all packages.
- **Background upgrades** — new versions install alongside existing
  ones; nothing changes until you switch, and rollback is instant. The
  daemon refreshes the index, detects outdated formulae and pre-downloads
  their bottles; with `daemon.auto_upgrade` on it applies them inside
  `daemon.upgrade_window`, deferring kegs that are in use (an in-use
  dependent that would move in the same batch defers the whole batch, so a
  dependency chain always moves together). Cask upgrades
  stay a manual `den upgrade`.
- **Source-first architecture** — embedded Ruby VM for evaluating
  Homebrew formula build recipes. Pre-built archives as an optimisation,
  not a requirement.
- **Multi-provider** — Homebrew is the default, but `den install`
  routes through a pluggable `PackageProvider` interface. Pip, npm,
  cargo, and go providers are available via `--provider <name>` without
  changes to the CLI, manifest, or environment layers.

## Install

```bash
curl -fsSL https://raw.githubusercontent.com/marcelocantos/den/master/install.sh | sh
```

`install.sh` needs only `curl` and `shasum` or `sha256sum` (no Homebrew). It
verifies the release checksum and installs `den` into `~/.den/bin`
(`DEN_INSTALL_DIR` overrides). Set `DEN_VERSION` to pick a release, release
candidates included (`DEN_VERSION=0.15.0-rc.1`). If the den prefix
(`~/.den/prefix`, or `DEN_PREFIX`) can't be created without `sudo`, it prints
the `sudo mkdir`/`chown` commands and, when run interactively, offers to run
them. A non-default `DEN_PREFIX` is recorded as `store.prefix` in
`$DEN_HOME/config.json` (via `den set`, keeping every other setting), so it
need not stay exported; if `config.json` already names a different
`store.prefix`, the installer leaves it and says so (`DEN_PREFIX_OVERWRITE=1`
replaces it). An installed den moves to another release with
`den self-update --version X.Y.Z[-rc.N]`; asking for the release that is
already installed (identical binary) is a no-op unless you add `--force`.

Or with Homebrew (macOS, Apple Silicon):

```bash
brew install marcelocantos/tap/den
brew trust --formula marcelocantos/tap/den   # Homebrew 7 and later
```

Homebrew 7 only loads formulae from third-party taps you have trusted.
`brew install marcelocantos/tap/den` trusts it as it installs, but if you
installed before Homebrew 7 (or `brew upgrade` skips den), run the
`brew trust` line once. It trusts this one formula, not the whole tap.

Or build from source:

```bash
git clone --recurse-submodules https://github.com/marcelocantos/den.git
cd den
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cp build/den ~/.den/bin/den
```

## Quick start

```bash
# Add to your shell (~/.zshrc or ~/.bashrc):
eval "$("$HOME/.den/bin/den" init)"

# Fetch the package index:
den update

# Install packages:
den install tree
den install ffmpeg          # resolves and installs all dependencies
den install firefox         # GUI apps — no --cask flag needed

# Manage versions:
den install python@3.11     # installs alongside existing python@3.12
den use python@3.11 3.11.9  # switch to a specific version

# Environments:
den env create /ml          # child of root, inherits all packages
den env use /ml             # switch to it
den install numpy           # only in /ml, root unchanged
den env use /               # back to root

# Background maintenance:
den daemon install          # auto-start at login
den set daemon.auto_download false  # disable background downloads
den set daemon.auto_upgrade true
den set daemon.upgrade_window "3:00-5:00"
den daemon status           # check for pending upgrades
den daemon run --once       # run one maintenance cycle now

# Query:
den search sqlite           # search 15,000+ packages instantly
den info ffmpeg             # show package details
den deps ffmpeg --tree      # dependency tree
den outdated                # what needs upgrading
den upgrade                 # upgrade everything
```

## How it works

Den manages package providers behind a uniform interface. Homebrew is
the default — den consumes the same archives and formulae API — but
the install / uninstall / upgrade / list / use flows all dispatch
through a pluggable `PackageProvider`. Pip, npm, cargo, and go providers
are available via `--provider <name>` without changes to the CLI,
manifest, or environment-composition layers.

- **The store** (`<prefix>/Cellar/`, by default `~/.den/prefix/Cellar/`)
  holds den-installed package versions. After `den migrate` the prefix
  is pinned to Homebrew's, so the store *is* the shared Homebrew Cellar
  (🎯T49) and brew/den kegs coexist. Den rewrites bottle
  placeholders on pour (Mach-O deps pinned to exact kegs on macOS,
  patchelf on Linux). It writes `<prefix>/opt/<name>` links only for
  its own kegs and never repoints Homebrew's. `den uninstall` drops a
  package from the environment but keeps its keg until `den cleanup`;
  if no environment still uses that version, its `opt/` link is handed
  to another installed version den manages, or removed. Other providers
  keep their own storage areas.
- **Manifests** declare what each environment contains, grouped per
  provider. Child environments inherit from parents and override
  specific packages.
- **Materialisation** asks each owning provider for its package root
  and binary paths, then resolves the manifest hierarchy into a flat
  directory of symlinks.
- **Shell integration** sets PATH and build environment variables
  (LIBRARY_PATH, CPATH, PKG_CONFIG_PATH, etc.) to point at the
  active environment.

```
~/.den/
├── bin/den                  # the binary
├── prefix/                  # den's prefix (unless pinned elsewhere)
│   ├── bin/ sbin/ lib/ ...  # root env links, brew-style
│   └── Cellar/              # package store
│       ├── tree/2.3.2/      # each version in its own directory
│       └── ffmpeg/7.1.1/
├── config.json              # settings
├── manifests/               # environment definitions
│   ├── ROOT/
│   │   └── manifest.json    # / (root)
│   └── ml/
│       └── manifest.json    # /ml (inherits from /)
├── envs/                    # materialised environments
│   ├── ROOT/                # the root env is linked into prefix/ itself
│   │   ├── bin -> ../../prefix/bin   # compat links to the prefix dirs
│   │   └── .den/            # link ledgers
│   └── ml/
│       ├── bin/             # symlinks into Cellar
│       ├── lib/
│       └── ...
├── cache/                   # content-addressed archive cache
├── caskroom/                # den's cask staging (<token>/<version>/)
└── daemon.log               # background maintenance log
```

## Configuration

All settings in `~/.den/config.json`, managed via `den set`:

```bash
den set daemon.auto_download false       # disable background downloads
den set daemon.auto_upgrade true         # auto-apply upgrades
den set daemon.upgrade_window "3:00-5:00"  # when to apply
den get store.prefix                     # print one setting (exit 1 if unset)
den settings                             # show all settings
```

### Store location and env precedence

den's *prefix* decides where kegs are poured (`<prefix>/Cellar`). It is
resolved in this order — the first match wins:

1. **`DEN_PREFIX`** environment variable.
2. **`store.prefix`** in `$DEN_HOME/config.json` (`den set store.prefix
   /path`; `den migrate` writes this to pin Homebrew's prefix).
3. **`$DEN_HOME/prefix`**, if `$DEN_HOME/prefix/Cellar` already exists
   (skipped while an unfinished `den migrate-prefix` is still pouring
   into it).
4. **Legacy shared Cellar** — a den home created before store isolation
   (no `$DEN_HOME/prefix/Cellar` yet) whose manifests list packages that
   Homebrew's Cellar has keeps using that Cellar (detected read-only;
   nothing is written to decide this). Run `den migrate-prefix` to give
   den its own folder (see below), or `den migrate` /
   `den set store.prefix …` to make the choice explicit.
5. **`$DEN_HOME/prefix`** (default).

### Moving off Homebrew's folder: `den migrate-prefix`

An older den home that installs into Homebrew's prefix can be moved into a
prefix of its own (`--to`, default `$DEN_HOME/prefix`). With no flags the
command is a dry run: it lists every keg it would reinstall (and who owns
the old copy), manifest drift and pins, the disk it would write per keg and
in total next to the free space at the destination, and the den service
specs, daemon plist and shell rc files it would touch. Nothing changes until
`--apply`.

- Homebrew's prefix is never written. Den-poured kegs there stay in place
  for a follow-up `den cleanup --prefix` to reclaim; Homebrew's own kegs and
  `homebrew.mxcl.*` plists stay Homebrew's.
- A keg whose manifest version is missing is re-poured at the index
  version and reported; den never downgrades, and a pin the index no longer
  offers blocks the move.
- A keg whose bottle only pours into Homebrew's Cellar (a hardcoded
  cellar) stays brew-owned: the plan lists it as `brew-owned`, `--apply`
  links `<to>/opt/<name>` to Homebrew's `opt/<name>`, and every den keg that
  will load it from there gets a `brew-link` line. It stays blocked when
  Homebrew is not installed, when Homebrew's Cellar has no such keg, or when
  the keg there was poured by den, so den never depends on brew silently.
  `den doctor` then lists these as "remaining brew dependencies", along with
  any den keg whose Mach-O load commands point into Homebrew's prefix.
- `--apply` refuses when the destination lacks space, or while den services
  or the den daemon run (`--stop-services` stops them, moves, rewrites
  their specs and the daemon's PATH, then restarts them).
- Kegs are poured and linked first and the switch of `store.prefix` comes
  last, so a failure before it leaves den unchanged. Progress is journalled
  in `$DEN_HOME/migrate-prefix.json`: `--resume` continues (re-verifying
  what was poured), `--rollback` restores the old prefix and keeps the new
  one on disk, and `--only a,b` pours a subset first.

`DEN_HOME` (default `~/.den`) holds manifests, envs, cache, caskroom and
config. So setting **`DEN_HOME` alone gives a fully isolated den** — its
own prefix and Cellar — even in a shell where `brew shellenv` has
exported `HOMEBREW_PREFIX`/`HOMEBREW_CELLAR`.

`HOMEBREW_PREFIX`, `HOMEBREW_CELLAR` and `HOMEBREW_CASKROOM` (or the
platform default `/opt/homebrew`, `/usr/local`, `/home/linuxbrew/.linuxbrew`)
only tell den **where Homebrew is**: `den migrate` reads brew's Cellar and
Caskroom from them, and diagnostics report them. They never choose den's
store. `den migrate` is the explicit step that points den at Homebrew's
prefix: it records `store.prefix = <brew prefix>` (or refuses if den
already has private kegs or `DEN_PREFIX`/`store.prefix` names somewhere
else). `den config` prints the resolved `prefix`, `prefix_source`,
whether it `shares_brew_cellar`, and this precedence.

The daemon's launchd plist carries `DEN_HOME` and (when set) `DEN_PREFIX`
so background upgrades use the same store.

## Commands

Run `den --help` for the full list. Key commands:

| Command | Description |
|---|---|
| `den install <pkg> [--provider <p>]` | Install a package (with dependency resolution); `--provider` routes to a specific provider |
| `den uninstall <pkg> [--provider <p>] [--ignore-dependencies]` | Remove from the active environment. Refuses (listing them) while installed packages in any environment need it at runtime; `--ignore-dependencies` overrides. The keg stays until `den cleanup`; if no environment still uses it, den hands its `opt/<name>` link to another installed version den manages, or removes it |
| `den use <pkg> <version>` | Switch active version |
| `den upgrade [pkg]` | Upgrade outdated packages |
| `den list` | List packages in the active environment |
| `den search <text>` | Search packages by name or description |
| `den info <pkg>` | Show package details |
| `den env create <path>` | Create a child environment |
| `den env use <path>` | Switch active environment |
| `den env show [path]` | Show resolved packages |
| `den env freeze` | Export environment as JSON lockfile |
| `den whence <file-or-name>` | Show which package owns a file or command |
| `den self-update` | Update den to the latest release |
| `den cleanup` | Remove unused versions den installed, and clear the archive cache. Kegs den did not install (including Homebrew's) are left untouched. `--dry-run` previews the removal |
| `den migrate-prefix [--to <dir>] [--apply\|--resume\|--rollback] [--only a,b] [--stop-services]` | Move an older den home off Homebrew's prefix into its own. Dry run unless `--apply`; see [Moving off Homebrew's folder](#moving-off-homebrews-folder-den-migrate-prefix) |
| `den autoremove` | Remove unused dependencies |
| `den services list` | Show managed services |
| `den log [-n] [--json]` | Show upgrade activity log |
| `den daemon status` | Daemon and pending upgrade status |
| `den doctor` | Check system health and report issues |
| `den smoke` | Run smoke tests against installed packages |
| `den migrate` | Adopt Homebrew packages into the root environment so `den list` shows them |

## Agent guide

If you use an agentic coding tool (Claude Code, Cursor, etc.), include
[`agents-guide.md`](agents-guide.md) in your project context for
den-aware assistance.

## Contributing

```bash
git clone --recurse-submodules https://github.com/marcelocantos/den.git
cd den
cmake -B build -G Ninja
cmake --build build
cd build && ctest --output-on-failure
```

Requires: cmake, ninja and libcurl (plus zlib and bzip2 headers on Linux). libarchive and its xz/zstd/lz4 codecs are fetched from pinned sources and linked statically (`-DDEN_VENDOR_LIBARCHIVE=OFF` uses a system libarchive instead).

To report a security vulnerability, see [SECURITY.md](SECURITY.md).

## Licence

Apache 2.0. See [LICENSE](LICENSE).

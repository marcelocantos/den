# Convergence Targets

## 🎯T1 — Core infrastructure compiles and runs

rubrew builds, runs `rubrew --version`, and has the module skeleton
in place for all major subsystems.

**Status**: in progress

---

## 🎯T2 — Configuration and environment detection

rubrew detects HOMEBREW_PREFIX, HOMEBREW_CELLAR, HOMEBREW_CACHE,
platform (macOS/Linux), architecture (arm64/x86_64), macOS version,
and Xcode/CLT availability. Reads existing Homebrew configuration.

**Status**: not started

---

## 🎯T3 — Formula metadata parsing

rubrew can parse Homebrew Ruby formula files and extract all metadata
(name, version, url, sha256, dependencies, bottle specs, etc.)
without executing Ruby code. Falls back to a Ruby evaluator for
formulae with dynamic metadata.

**Status**: not started

---

## 🎯T4 — API client

rubrew can fetch and cache formula/cask metadata from
formulae.brew.sh JSON API. Serves as the primary metadata source
(matching Homebrew 4.0+ behavior).

**Status**: not started

---

## 🎯T5 — Cellar and keg management

rubrew can inspect the Cellar, enumerate racks/kegs, read
INSTALL_RECEIPT.json tabs, and report installed packages with
versions. `rubrew list` works.

**Status**: not started

---

## 🎯T6 — Tap management

rubrew can list, add, remove, and update taps (git repos).
`rubrew tap`, `rubrew untap`, `rubrew tap-info` work.

**Status**: not started

---

## 🎯T7 — Dependency resolution

rubrew resolves transitive dependencies, performs topological sort,
detects cycles, and filters by platform/build-type. Produces a
correct install plan.

**Status**: not started

---

## 🎯T8 — Download and caching

rubrew downloads source tarballs and bottles with SHA256 verification,
resume support, and local caching in HOMEBREW_CACHE.

**Status**: not started

---

## 🎯T9 — Bottle pouring

rubrew can pour (install from pre-built bottle) any Homebrew bottle,
including Mach-O relocation via install_name_tool. `rubrew install
<formula>` works for bottled formulae.

**Status**: not started

---

## 🎯T10 — Symlink management

rubrew can link/unlink kegs, manage /opt/homebrew/opt symlinks,
detect conflicts, and handle keg-only formulae. `rubrew link`,
`rubrew unlink` work.

**Status**: not started

---

## 🎯T11 — Source builds

rubrew can build formulae from source by delegating to a sandboxed
Ruby evaluator for the `install` block. Build environment (compiler
flags, paths) is correctly configured.

**Status**: not started

---

## 🎯T12 — Cask support

rubrew can install, uninstall, and manage casks (DMG/ZIP/PKG
artifacts, app bundles, binaries, fonts). `rubrew install --cask`
works.

**Status**: not started

---

## 🎯T13 — Upgrade and update

`rubrew update` fetches latest tap/API data. `rubrew upgrade`
detects outdated packages and upgrades them in dependency order.

**Status**: not started

---

## 🎯T14 — Info, search, and query commands

`rubrew info`, `rubrew search`, `rubrew deps`, `rubrew uses`,
`rubrew leaves`, `rubrew outdated` all work correctly.

**Status**: not started

---

## 🎯T15 — Cleanup and maintenance

`rubrew cleanup`, `rubrew autoremove`, `rubrew doctor` work.
Doctor performs the same diagnostic checks as Homebrew.

**Status**: not started

---

## 🎯T16 — Services

`rubrew services` can list, start, stop, and restart launchd/systemd
services defined by formulae.

**Status**: not started

---

## 🎯T17 — Full CLI parity

All Homebrew commands and flags are implemented. `rubrew` is a
drop-in replacement for `brew`.

**Status**: not started

---

## 🎯T18 — Performance

rubrew is measurably faster than Homebrew for common operations
(install, list, search, info, update, upgrade, cleanup).

**Status**: not started

# Convergence Targets

## 🎯T1 — Core infrastructure compiles and runs

den builds, runs `den --version`, and has the module skeleton
in place for all major subsystems.

**Status**: achieved

---

## 🎯T2 — Configuration and environment detection

den detects HOMEBREW_PREFIX, HOMEBREW_CELLAR, HOMEBREW_CACHE,
platform (macOS/Linux), architecture (arm64/x86_64), macOS version,
and Xcode/CLT availability. Reads existing Homebrew configuration.

**Status**: not started

---

## 🎯T3 — API client

den can fetch and cache formula/cask metadata from
formulae.brew.sh JSON API. This is the primary metadata source
(no Ruby parsing needed for core formulae).

**Status**: not started

---

## 🎯T4 — Cellar and keg management

den can inspect the Cellar, enumerate racks/kegs, read
INSTALL_RECEIPT.json tabs, and report installed packages with
versions. `den list` works. Multiple versions per package coexist.

**Status**: not started

---

## 🎯T5 — Tap management

den can list, add, remove, and update taps (git repos).
`den tap`, `den untap` work.

**Status**: not started

---

## 🎯T6 — Dependency resolution

den resolves transitive dependencies, performs topological sort,
detects cycles, and filters by platform/build-type. Produces a
correct install plan that respects multi-version coinstallation.

**Status**: not started

---

## 🎯T7 — Download and caching

den downloads source tarballs and bottles with SHA256 verification,
resume support, and local caching.

**Status**: not started

---

## 🎯T8 — Bottle pouring

den can pour (install from pre-built bottle) any Homebrew bottle,
including Mach-O relocation via install_name_tool. `den install
<formula>` works for bottled formulae. Installing a new version
preserves existing versions.

**Status**: not started

---

## 🎯T9 — Environment management

den supports named environments — directories of symlinks into the
Cellar. `den env create`, `den env list`, `den env use`,
`den env remove`, `den env freeze` all work. Environments compose
via PATH precedence.

**Status**: not started

---

## 🎯T10 — Version switching

`den use <package>@<version>` atomically switches which version is
linked in the active environment. No unlink-then-link dance — the
swap is atomic. `den status` shows active versions.

**Status**: not started

---

## 🎯T11 — Source builds

den can build formulae from source by delegating to Ruby for the
`install` block. Build environment (compiler flags, paths) is
correctly configured.

**Status**: not started

---

## 🎯T12 — Cask support

den can install, uninstall, and manage casks (DMG/ZIP/PKG
artifacts, app bundles, binaries, fonts). `den install --cask`
works.

**Status**: not started

---

## 🎯T13 — Background daemon

A background service (launchd on macOS) watches for available
upgrades, downloads bottles, and stages new versions in the Cellar.
By default, new versions are staged but the active environment is
not modified until the user acts. With `auto-upgrade` enabled
(`den set auto-upgrade true`), the daemon also switches the active
environment to the new version automatically. `den status` shows
what's installed, what's pending, and the auto-upgrade setting.

**Status**: not started

---

## 🎯T14 — Info, search, and query commands

`den info`, `den search`, `den deps`, `den uses`,
`den leaves`, `den outdated` all work correctly.

**Status**: not started

---

## 🎯T15 — Cleanup and maintenance

`den cleanup`, `den autoremove`, `den doctor` work.

**Status**: not started

---

## 🎯T16 — Services

`den services` can list, start, stop, and restart launchd/systemd
services defined by formulae.

**Status**: not started

---

## 🎯T17 — Formula metadata parsing (third-party taps)

den can parse Homebrew Ruby formula files to extract metadata for
third-party taps not covered by the JSON API. Falls back to Ruby
for the ~5% of formulae with dynamic metadata.

**Status**: not started

---

## 🎯T18 — Testing oracle

A test harness that captures Homebrew's observable state for any
operation (metadata, file tree, symlinks, receipts) and diffs
against den's output. Runs across the full formula corpus for
metadata equivalence, and against a curated set for installation
equivalence.

**Status**: not started

---

## 🎯T19 — Performance

den is measurably faster than Homebrew for common operations
(install, list, search, info, update, upgrade, cleanup).

**Status**: not started

# Convergence Targets

Targets are ordered by what gets den to daily-driver status fastest.
The critical path is: dependency resolution → Cellar migration →
uninstall → cask support. Everything else is either already done or
can wait.

## Achieved

### 🎯T1 — Core infrastructure
den builds, runs, has module skeleton. **Achieved.**

### 🎯T3 — API client
Fetches formula metadata from formulae.brew.sh JSON API. **Achieved.**

### 🎯T8 — Bottle pouring
Downloads and pours Homebrew bottles with SHA256 verification.
Multi-version coinstallation works. **Achieved.**

### 🎯T9 — Environment management
Manifest-based environment hierarchy with path naming (/ is root,
/ml, /work/legacy). Inheritance at the manifest level — child envs
inherit packages and can override versions. Materialisation produces
flat symlink directories. Shell integration via `eval "$(den init)"`.
**Achieved.**

### 🎯T10 — Version switching
`den use pkg=version` updates manifest and re-materialises.
Old and new versions coexist in the Cellar. **Achieved.**

---

## Critical path to daily-driver

### 🎯T6 — Dependency resolution

`den install ffmpeg` automatically installs all transitive
dependencies. Resolution uses the JSON API's `dependencies` and
`build_dependencies` fields. Topological sort determines install
order. Already-installed kegs are skipped.

This is the single biggest blocker to standalone use — without it,
every install with dependencies fails or requires pre-installing
via brew.

**Status**: not started

---

### 🎯T20 — Cellar migration

`den migrate` scans the existing Homebrew Cellar and populates the
root manifest with everything currently installed. This is the
on-ramp — the user runs it once and den takes over management of
their existing packages without reinstalling anything.

**Status**: not started

---

### 🎯T21 — Uninstall

`den uninstall <pkg>` removes a package from the active environment's
manifest, re-materialises, and optionally removes the keg from the
Cellar if no other environment references it. `den autoremove`
removes packages that were installed as dependencies but are no
longer needed.

**Status**: not started

---

### 🎯T12 — Cask support

`den install --cask google-chrome` downloads and installs DMG/ZIP/PKG
casks. Casks appear in the manifest. Uninstall removes the app.
This is needed because many essential tools (browsers, editors,
terminals, Docker) are casks.

**Status**: not started

---

### 🎯T22 — Update and upgrade

`den update` fetches the latest API metadata. `den upgrade` compares
installed versions against available versions and upgrades outdated
packages (respecting dependency order). `den outdated` lists what
needs upgrading.

**Status**: not started

---

### 🎯T16 — Services

`den services list`, `den services start <name>`,
`den services stop <name>` manage launchd plists for formulae that
define services (postgres, redis, nginx, etc.). Many development
workflows depend on this.

**Status**: not started

---

## Important but not blocking daily use

### 🎯T2 — Configuration and environment detection

Xcode/CLT detection, full Homebrew config compatibility. Partially
done (prefix, cellar, arch, macOS version work). Needs Xcode version
detection and CLT path resolution.

**Status**: partially achieved

---

### 🎯T7 — Download caching

Cache downloaded bottles in HOMEBREW_CACHE with resume support.
Currently downloads every time. Not blocking but wastes bandwidth.

**Status**: not started

---

### 🎯T4 — Cellar inspection improvements

`den list` currently shows resolved manifest packages. Should also
support listing all Cellar contents (including packages not in any
manifest), showing which envs reference a keg, and disk usage.

**Status**: partially achieved

---

### 🎯T14 — Info, search, and query commands

`den info`, `den search`, `den deps --tree`. Useful but you can
fall back to `brew info` / `brew search` in the meantime.

**Status**: not started

---

### 🎯T15 — Cleanup and maintenance

`den cleanup` removes old keg versions not referenced by any
manifest. `den doctor` checks system health.

**Status**: not started

---

### 🎯T13 — Background daemon

launchd service that checks for updates, downloads bottles, and
optionally auto-upgrades during a maintenance window. Nice-to-have
for daily use; manual `den upgrade` works in the meantime.

**Status**: not started

---

## Deferred

### 🎯T5 — Tap management
Third-party taps. Most users only use homebrew-core.

### 🎯T11 — Source builds
Delegate to Ruby. Only needed for `--build-from-source`.

### 🎯T17 — Formula metadata parsing (third-party taps)
Ruby DSL parsing for non-API taps.

### 🎯T18 — Testing oracle
Automated Homebrew-equivalence testing.

### 🎯T19 — Performance
Benchmarking against Homebrew.

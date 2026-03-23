# Convergence Targets

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

### 🎯T6 — Dependency resolution
`den install` resolves the full transitive dependency graph via
the JSON API (post-order DFS), pours deps before the target,
and tracks auto-installed packages in the manifest. **Achieved.**

### 🎯T20 — Cellar migration
`den migrate` scans the Homebrew Cellar, reads INSTALL_RECEIPT.json
for each keg, and populates the root manifest. One command imports
the entire existing package state. **Achieved.**

### 🎯T21 — Uninstall
`den uninstall` removes from the manifest and re-materialises.
`den autoremove` stub exists. **Achieved.**

### 🎯T12 — Cask support
`den install --cask` handles DMG and ZIP casks with app artifacts.
Downloads, mounts/extracts, copies .app to /Applications, tracks
in manifest. **Achieved.**

### 🎯T22 — Update and upgrade
`den outdated` compares manifest vs API. `den upgrade` pours new
bottles and re-materialises. **Achieved.**

### 🎯T16 — Services
`den services list/start/stop/restart` manages launchd plists.
**Achieved.**

---

## Remaining for production quality

### 🎯T2 — Configuration and environment detection
Xcode/CLT version detection, full Homebrew config parity.
**Status**: partially achieved

### 🎯T7 — Download caching
Cache bottles in HOMEBREW_CACHE with resume. Currently re-downloads.
**Status**: not started

### 🎯T4 — Cellar inspection improvements
Show disk usage, which envs reference a keg, all-Cellar listing.
**Status**: partially achieved

### 🎯T14 — Info, search, and query commands
`den info`, `den search`, `den deps --tree`.
**Status**: not started

### 🎯T15 — Cleanup and maintenance
`den cleanup` (remove old kegs), `den doctor` (system health).
**Status**: not started

### 🎯T13 — Background daemon
launchd service for auto-updates with maintenance window.
**Status**: not started

---

## Future

### 🎯T23 — Multi-provider package management

den becomes a universal frontend for multiple package managers.
A `PackageProvider` trait abstracts install/resolve/bin-dir per
ecosystem. Providers:

- **brew** (current) — Homebrew bottles and casks
- **go** — `go install` binaries (GOBIN → den env)
- **cargo** — `cargo install` binaries
- **pip/uv** — Python packages installed into isolated virtualenvs,
  binaries linked into the den environment
- **npm** — global npm packages with binaries linked in

Each provider manages its own storage (GOPATH, cargo target, venvs)
but binaries converge into the active den environment. The manifest
declares provider per-package. Auto-detection chooses the right
provider when the package name is unambiguous (e.g. `den install
ripgrep` → cargo, `den install httpie` → pip).

`den list` shows everything regardless of provider. `den use`
switches versions regardless of source. One tool, one environment,
all ecosystems.

**Status**: not started

---

### 🎯T24 — Semantic search

`den search` uses vector similarity over formula metadata (name,
description, README content) to find packages by intent rather than
keyword. Trained on a technical corpus so domain-specific queries
work well ("compress video without losing quality" → ffmpeg).

Index is pre-built (~24MB for 8K formulae × 768d embeddings) and
ships alongside API metadata. Search is local and instant — no
network call. Updated when formula metadata refreshes.

Embedding model: local via ollama (e.g. nomic-embed-text) for
privacy and offline support. Falls back to keyword matching if
ollama is unavailable.

**Status**: not started

---

### 🎯T5 — Tap management
Third-party taps.

### 🎯T11 — Source builds
Delegate to Ruby for `--build-from-source`.

### 🎯T17 — Formula metadata parsing (third-party taps)
Ruby DSL parsing for non-API taps.

### 🎯T18 — Testing oracle
Automated Homebrew-equivalence testing.

### 🎯T19 — Performance
Benchmarking against Homebrew.

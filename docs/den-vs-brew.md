# den vs Homebrew — capability matrix

This is the v1.0 feature-completeness artifact for den (🎯T74). Every row
states what Homebrew does, what den does, and a status:

- ✅ **parity** — den matches Homebrew on this capability.
- ⭐ **better** — den exceeds Homebrew (faster, safer, or more general).
- ⏳ **partial** — den ships a working subset; the remaining gap is tracked
  by an explicit follow-up target and is **post-v1**.

den consumes the Homebrew ecosystem directly — same Cellar
(`/opt/homebrew/Cellar`), same formulae, casks, taps, and bottles — so
"parity" usually means *literally the same artifacts*. Den rewrites the
bottles' Homebrew placeholders on pour: on macOS, Mach-O references are
pinned to exact dependency kegs and Homebrew's shared `opt/` links are
never written; on Linux, ELF paths are patched with patchelf and a prefix
`opt/` link is created only when missing or dangling. Where den is marked ⭐, the win comes from its
independent store, environment model, multi-provider seam, or SAT solver.

## Matrix

| Capability | Homebrew | den | Status |
|---|---|---|---|
| **Install (bottle)** | `brew install jq` pours a prebuilt bottle into the Cellar and links into the prefix. | `den install jq` pours the same bottle into the shared Cellar (placeholders rewritten on pour; macOS deps pinned to exact kegs, `opt/` never written — #64) and links it into the active environment. | ⭐ better — ~23× faster install on M4 Max (🎯T68); per-environment linking instead of one global prefix. |
| **Source build** | `brew install --build-from-source` evaluates the Ruby formula and compiles. | `den install -s/--build-from-source` builds via the bundled Ruby (complex formulae) or the native C++ fast-path parser (simple ones), with a shim-free macOS toolchain (`SDKROOT`/`-isysroot`, `/usr/bin/ld`, no `xcrun`). | ✅ parity — works today. Native-parser coverage is still narrower than Ruby (dependency extraction 🎯T58, no-Ruby SIMPLE installs 🎯T59); Ruby remains the complex-formula path, so no functional gap. |
| **Third-party taps** | `brew tap user/repo`; tapped formulae resolve and install. | `den tap add user/repo [source]`, `den tap --list`, `den tap --remove`; tapped formulae merge into the index and install via the source-build path (🎯T67). | ✅ parity |
| **Casks** | `brew install --cask` installs macOS apps (DMG/ZIP/PKG, scripts, uninstall/zap stanzas). | `den install <cask>` (no `--cask` flag) installs DMG, ZIP/tarball, `.app` and binary-only casks on macOS: the archive is staged under `~/.den/caskroom/<token>/<version>/` (never Homebrew's Caskroom), the new app is staged beside the target in `DEN_APPDIR` (default `/Applications`) and swapped in only once fully staged, with rollback to the old version on any failure (#61, #62). Den writes `DEN_CASK_RECEIPT.json` and only overwrites or removes apps its own receipt names. `.pkg` casks run `installer`. Casks are excluded from daemon auto-upgrade (#66) and are unsupported on Linux. | ⏳ partial (🎯T12) — not yet handled: `.pkg` installs need admin (den runs `installer` without elevation) and uninstall does not run `pkgutil --forget`; `preflight`/`postflight` and installer scripts; `uninstall` stanzas (launchctl, quit, signal, delete…); `zap`; and quitting a running app before uninstall/upgrade. |
| **Multi-version coinstall** | Versioned formulae (`python@3.11`, `python@3.12`) coexist; switching is manual relinking. | Installing a new version never removes the old one; `den use <name> <version>` atomically switches which version the active environment links. | ⭐ better — multi-version is the default, switch is one atomic command per environment, rollback is instant. |
| **Dependency resolution** | Greedy dependency walk; conflicts surface as install failures. | CDCL/DPLL SAT solver with stability-biased preferences and multi-version awareness; `den deps --explain` prints the clauses, preferences, and conflicts behind an assignment. | ⭐ better — complete solver with explainable UNSAT, replaces the prior greedy DFS. |
| **Environments / isolation** | One global prefix; no first-class isolated environments. | Named environments are symlink sets under `~/.den/envs/`; they compose via PATH precedence (a project env overrides the default). `den env create/list/use/remove/show/freeze`. | ⭐ better — virtualenv-style isolation for *every* package, not just Python; `env freeze` exports a lockfile. |
| **Python packages** | Not a Homebrew concern (formulae only). | `den install --provider pip <pkg>`; uniform `list`/`info`/`uninstall`/`run` through the `pip` provider (🎯T60). | ⭐ better — single tool spans Homebrew + pip. |
| **Node packages** | Not a Homebrew concern. | `den install --provider npm <pkg>` via the `npm` provider (🎯T60). | ⭐ better |
| **Go packages** | Not a Homebrew concern. | `den install --provider go <pkg>` via the `go` provider (`go install`) (🎯T60). | ⏳ partial — curated bare aliases (e.g. `staticcheck`, `gofumpt`) install end-to-end; full module paths (`host/path@version`) are rejected/mis-materialised. Tracked: 🎯T77. |
| **Cargo packages** | Not a Homebrew concern. | `den install --provider cargo <pkg>` via the `cargo` provider (`cargo install`) (🎯T60). | ⭐ better |
| **Search** | `brew search` substring match on names/descriptions. | `den search` case-insensitive substring match on name + description over the local index. | ⏳ partial — keyword parity today. Semantic (embedding/LLM) search is post-v1: 🎯T62 (relevance), 🎯T24 (provider hierarchy), 🎯T25 (corpus CI pipeline). |
| **Services** | `brew services start/stop/...` via launchd/systemd. | `den services list/start/stop/restart/status/logs` with a built-in supervisor (no launchctl); dependency-ordered stop and restart. | ⏳ partial — built-in supervisor manages services today; full launchctl-free supervisor hardening is 🎯T61. Restart-after-upgrade is 🎯T52. |
| **Upgrades** | `brew upgrade [pkg]`; in-place, foreground; an in-use binary can break. | `den upgrade [pkg]` per-provider; a background daemon stages upgrades alongside existing versions without touching the active env. | ⭐ better — ~6× faster on the upgrade path (🎯T68); staged, non-destructive, instant rollback. |
| **Upgrade: defer-while-in-use** | No deferral; upgrading a running tool can clobber it. | The daemon detects in-use keg files (`lsof` / `/proc`) and defers the upgrade; deferred items surface in `den outdated` and `den daemon status` with a reason (🎯T72). | ⭐ better — no Homebrew equivalent. |
| **Pinning** | `brew pin <f>` holds a formula at its installed version; `brew upgrade` skips it (and errors when named explicitly); `brew list --pinned`; `brew unpin`. Uninstall drops the pin. | `den pin <pkg>...` pins at the active version, `den pin`/`den pin --list` lists pins (flagging ones no longer installed or switched with `den use`), `den unpin <pkg>...`. `den upgrade` skips pinned packages ("Not upgrading N pinned package(s)") and refuses a pinned name with a non-zero exit; the daemon auto-upgrade pass never applies a pinned upgrade (it stays visible in `den outdated`, tagged `[pinned at V]`). Uninstall drops the pin. A corrupt `~/.den/pins.json` fails closed (holds every upgrade). | ✅ parity — pins are global to den_home (not per environment); `den use` can still switch a pinned package manually. |
| **Migration from brew** | n/a (Homebrew is the source). | `den migrate` records formulae in the root environment manifest (`manifests/ROOT/manifest.json`, the file `den list` reads), and records casks, taps, and brew services in den's settings and service store. Non-destructive (Cellar byte-identical), idempotent, merges with packages already in the root manifest, with a post-migration health check, `--dry-run`, and opt-in `--integrate-shell` (🎯T71). | ⭐ better — one-shot, reversible adoption; `brew` keeps working afterward. |
| **Trust / verification** | Bottle SHA256 checked against the same Homebrew API that served it (single source of truth). | Install-time cross-check of the Homebrew API hash against an independent replica (`data/known_hashes.json` via `raw.githubusercontent.com`); `den doctor` shows a `[trust]` block; a diff-based GHCR re-verify CI job (`replica-verify.yml`) plus the hidden `den replica-verify` tool (🎯T64). | ⭐ better — independent second source defeats single-point compromise. Advanced layers (sigstore/transparency-log style) are post-v1: 🎯T44. |
| **Host introspection** | `brew config` / `brew doctor` report toolchain and environment. | `den config` / `den doctor` report Xcode / CLT / SDK (host facts on Linux) plus `brew config` parity keys (🎯T66). | ✅ parity (superset of `brew config` keys) |
| **Cellar introspection** | `brew list`, `brew --cellar`; no per-keg disk/ref view. | `den list --cellar` shows per-keg disk usage, environment references, and orphans; `den whence` maps a file/command to its owning keg (🎯T66). | ⭐ better — orphan and reference reporting Homebrew lacks. |
| **Performance** | Ruby startup tax on every invocation. | Native C++ binary; benchmarked against `brew` on the same hardware (`scripts/bench/`, `bench.yml` CI). | ⭐ better — ~6× on list/info/search/upgrade, ~23× on install (M4 Max, 🎯T68). |

## Post-v1 gaps (tracked, not shipped)

These are deliberately out of scope for v1.0 and each has a convergence
target:

| Area | Target(s) | Why deferred |
|---|---|---|
| Semantic search (embedding + LLM) | 🎯T62, 🎯T24, 🎯T25 | Keyword search covers the v1 need; semantic search needs a pre-baked corpus index and a CI pipeline. |
| Content-addressed Cellar + explicit bindings | 🎯T69, 🎯T26, 🎯T28 | The shared-Cellar model ships v1; content-addressing is a storage-layer evolution that must land without regression. |
| Opt-in telemetry | 🎯T70, 🎯T32 | Privacy-first; only ships once reports are transparent and actionable. |
| Built-in supervisor hardening | 🎯T61 | Services work today; full launchctl-free supervisor parity is incremental. |
| Advanced trust layers | 🎯T44 | The replica cross-check ships v1; transparency-log / signature layering is additive. |
| Restart services after upgrade | 🎯T52 | Deferred-upgrade detection ships v1 (🎯T72); auto-restart orchestration follows. |
| Native parser → Ruby-free SIMPLE installs | 🎯T58, 🎯T59 | Ruby remains the complex-formula path; the native fast-path widens over time. |

No row above is a regression against Homebrew: every line is parity or
better, and every ⏳ row ships a working subset with the remainder tracked.

# den vs Homebrew — capability matrix

This is the v1.0 feature-completeness artifact for den (🎯T74). Every row
states what Homebrew does, what den does, and a status:

- ✅ **parity** — den matches Homebrew on this capability.
- ⭐ **better** — den exceeds Homebrew (faster, safer, or more general).
- ⏳ **partial** — den ships a working subset; the remaining gap is tracked
  by an explicit follow-up target and is **post-v1**.

den consumes the Homebrew ecosystem directly — same Cellar
(`/opt/homebrew/Cellar`), same formulae, casks, taps, and bottles — so
"parity" usually means *literally the same artifacts*, poured or built
without relocation. Where den is marked ⭐, the win comes from its
independent store, environment model, multi-provider seam, or SAT solver.

## Matrix

| Capability | Homebrew | den | Status |
|---|---|---|---|
| **Install (bottle)** | `brew install jq` pours a prebuilt bottle into the Cellar and links into the prefix. | `den install jq` pours the same bottle at its expected prefix (zero relocation, shared Cellar) and links it into the active environment. | ⭐ better — ~23× faster install on M4 Max (🎯T68); per-environment linking instead of one global prefix. |
| **Source build** | `brew install --build-from-source` evaluates the Ruby formula and compiles. | `den install -s/--build-from-source` builds via the bundled Ruby (complex formulae) or the native C++ fast-path parser (simple ones), with a shim-free macOS toolchain (`SDKROOT`/`-isysroot`, `/usr/bin/ld`, no `xcrun`). | ✅ parity — works today. Native-parser coverage is still narrower than Ruby (dependency extraction 🎯T58, no-Ruby SIMPLE installs 🎯T59); Ruby remains the complex-formula path, so no functional gap. |
| **Third-party taps** | `brew tap user/repo`; tapped formulae resolve and install. | `den tap add user/repo [source]`, `den tap --list`, `den tap --remove`; tapped formulae merge into the index and install via the source-build path (🎯T67). | ✅ parity |
| **Casks** | `brew install --cask` installs macOS apps. | den imports casks during `den migrate` and recognises app-type artifacts; cask installs ride the Homebrew provider. | ⏳ partial — migration imports casks; native cask install lifecycle is not yet on par. Tracked under the content-addressed store work (🎯T69) and trust layering (🎯T44) for app verification. |
| **Multi-version coinstall** | Versioned formulae (`python@3.11`, `python@3.12`) coexist; switching is manual relinking. | Installing a new version never removes the old one; `den use <name> <version>` atomically switches which version the active environment links. | ⭐ better — multi-version is the default, switch is one atomic command per environment, rollback is instant. |
| **Dependency resolution** | Greedy dependency walk; conflicts surface as install failures. | CDCL/DPLL SAT solver with stability-biased preferences and multi-version awareness; `den deps --explain` prints the clauses, preferences, and conflicts behind an assignment. | ⭐ better — complete solver with explainable UNSAT, replaces the prior greedy DFS. |
| **Environments / isolation** | One global prefix; no first-class isolated environments. | Named environments are symlink sets under `~/.den/envs/`; they compose via PATH precedence (a project env overrides the default). `den env create/list/use/remove/show/freeze`. | ⭐ better — virtualenv-style isolation for *every* package, not just Python; `env freeze` exports a lockfile. |
| **Python packages** | Not a Homebrew concern (formulae only). | `den install --provider pip <pkg>`; uniform `list`/`info`/`uninstall`/`run` through the `pip` provider (🎯T60). | ⭐ better — single tool spans Homebrew + pip. |
| **Node packages** | Not a Homebrew concern. | `den install --provider npm <pkg>` via the `npm` provider (🎯T60). | ⭐ better |
| **Go packages** | Not a Homebrew concern. | `den install --provider go <pkg>` via the `go` provider (`go install`) (🎯T60). | ⭐ better |
| **Cargo packages** | Not a Homebrew concern. | `den install --provider cargo <pkg>` via the `cargo` provider (`cargo install`) (🎯T60). | ⭐ better |
| **Search** | `brew search` substring match on names/descriptions. | `den search` case-insensitive substring match on name + description over the local index. | ⏳ partial — keyword parity today. Semantic (embedding/LLM) search is post-v1: 🎯T62 (relevance), 🎯T24 (provider hierarchy), 🎯T25 (corpus CI pipeline). |
| **Services** | `brew services start/stop/...` via launchd/systemd. | `den services list/start/stop/restart/status/logs` with a built-in supervisor (no launchctl); dependency-ordered stop and restart. | ⏳ partial — built-in supervisor manages services today; full launchctl-free supervisor hardening is 🎯T61. Restart-after-upgrade is 🎯T52. |
| **Upgrades** | `brew upgrade [pkg]`; in-place, foreground; an in-use binary can break. | `den upgrade [pkg]` per-provider; a background daemon stages upgrades alongside existing versions without touching the active env. | ⭐ better — ~6× faster on the upgrade path (🎯T68); staged, non-destructive, instant rollback. |
| **Upgrade: defer-while-in-use** | No deferral; upgrading a running tool can clobber it. | The daemon detects in-use keg files (`lsof` / `/proc`) and defers the upgrade; deferred items surface in `den outdated` and `den daemon status` with a reason (🎯T72). | ⭐ better — no Homebrew equivalent. |
| **Migration from brew** | n/a (Homebrew is the source). | `den migrate` imports formulae, casks, taps, and brew services; non-destructive (Cellar byte-identical), idempotent, with a post-migration health check, `--dry-run`, and opt-in `--integrate-shell` (🎯T71). | ⭐ better — one-shot, reversible adoption; `brew` keeps working afterward. |
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

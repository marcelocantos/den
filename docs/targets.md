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

### 🎯T16 — Services (basic)
`den services list/start/stop/restart` manages launchd plists.
**Achieved.** See T33 for the built-in supervisor that replaces this.

### 🎯T7 — Download caching
Content-addressed bottle cache in `~/.den/cache/bottles/` with
SHA256-keyed storage. Bottles are fetched once and reused across
installs. **Achieved.**

### 🎯T14 — Info, search, and query commands
`den info`, `den search`, `den deps --tree` all implemented.
**Achieved.**

### 🎯T13 — Background maintenance daemon
`den daemon run/stop/status/install/uninstall` implemented. Daemon
runs as a long-lived process, refreshes the formula index, downloads
bottles for outdated packages, and supports auto-upgrade with
configurable maintenance windows. State stored in
`~/.den/daemon_state.json`. **Achieved.**

### 🎯T35 — Build environment integration
`den init` exports LIBRARY_PATH, CPATH, PKG_CONFIG_PATH,
CMAKE_PREFIX_PATH, and MANPATH pointing at the active environment.
These swap when `den env use` switches environments. **Achieved.**

---

## Remaining for production quality

### 🎯T33 — Built-in process supervisor

Replace the launchctl-based service management (T16) with a
built-in process supervisor in den itself. No external dependency —
one binary does the lot.

**Phase 1 (minimal viable supervisor):**
- Fork/exec service binary as a child process
- PID tracking in `~/.den/services/<name>/pid`
- stdout/stderr capture to `~/.den/services/<name>/log`
- `den services stop` sends SIGTERM, waits (configurable timeout),
  then SIGKILL
- `den services list` checks live PIDs, shows uptime
- `den services logs <name> [-f]` tails/follows the log
- Environment-aware: services inherit the active den environment

**Phase 2 (robustness):**
- Restart policies: `always`, `on-failure`, `never` (default: never)
- Configurable restart backoff (exponential with jitter)
- Health checks: TCP port open, HTTP endpoint, custom command
- Graceful shutdown ordering (stop dependents before dependencies)
- Log rotation (size-based, configurable retention)

**Phase 3 (advanced):**
- `den services status` with resource usage (RSS, CPU)
- Per-environment services (`/ml` gets its own postgres instance)
- Service groups (`den services start dev-stack`)
- `den daemon` as a long-lived background process that manages both
  services and background upgrades (T13), started on first use or
  at login via a single launchd bootstrap job

The den daemon is the only thing that touches launchd — everything
else is supervised directly by den.

**Status**: not started

---

### 🎯T2 — Configuration and environment detection
Xcode/CLT version detection, full Homebrew config parity.
**Status**: partially achieved

### 🎯T4 — Cellar inspection improvements
Show disk usage, which envs reference a keg, all-Cellar listing.
**Status**: partially achieved

### 🎯T15 — Cleanup and maintenance
`den cleanup` (remove old kegs), `den doctor` (system health).
**Status**: partially achieved (cleanup works, doctor is stub)

### 🎯T34 — Daemon socket API

Extend the daemon (T13) with a Unix socket at `~/.den/den.sock`
for real-time CLI↔daemon communication. Required for T33 (process
supervision) where the CLI needs to send commands to a running
supervisor. Not needed for background maintenance alone.

**Status**: not started (blocked by T33 need)

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

### 🎯T38 — Python provider (subsume virtualenv)

Each den environment gets its own isolated Python with its own
site-packages. No separate virtualenv needed.

**How it works:**
- Den creates a `pyvenv.cfg` in the environment directory, which
  makes Python treat it as a virtual environment natively.
- `den install numpy` (in `/ml`) runs `uv pip install` (preferred)
  or `pip install --target` into the environment's
  `lib/python3.X/site-packages/`.
- Python packages are tracked in the manifest under a `pip` key:
  ```json
  { "pip": { "numpy": "2.2.1", "torch": "2.6.0" } }
  ```
- Environment inheritance works: `/ml/experiment` inherits `/ml`'s
  Python packages and can add or override them.
- `den use python@3.11` switches the Python version and
  re-evaluates package compatibility.

**Provider detection:** `den install numpy` auto-detects pip as the
provider (numpy isn't a Homebrew formula). `den install --pip numpy`
is explicit. Both work.

**Replaces:** pyenv (version management) + virtualenv/venv (package
isolation) + pipx (binary isolation). One tool.

**Status**: not started

---

### 🎯T39 — Node.js/npm provider

Each den environment gets its own isolated Node.js with its own
global packages.

- `den install prettier` (auto-detected as npm) installs to the
  environment's `lib/node_modules/` and links binaries to `bin/`.
- `den use node@20` switches Node version.
- Manifest tracks under `npm` key.
- `node_modules` is per-environment, not global. No `npm -g` chaos.

**Replaces:** nvm/fnm (version management) + npm global installs.

**Status**: not started

---

### 🎯T40 — Go provider

- `den install gopls` runs `go install` into environment-local
  GOBIN.
- `den use go@1.22` switches Go toolchain version.
- Manifest tracks under `go` key with full module paths.
- Go binaries are statically linked so no site-packages equivalent
  is needed — just the binary in `bin/`.

**Replaces:** manual `go install` + PATH management.

**Status**: not started

---

### 🎯T41 — Cargo provider

- `den install bat` runs `cargo install` into environment-local
  CARGO_INSTALL_ROOT.
- `den use rust@1.84` switches Rust toolchain (delegates to
  rustup if available, or manages versions directly).
- Manifest tracks under `cargo` key with crate names.
- Like Go, Rust binaries are typically self-contained.

**Replaces:** manual `cargo install` + scattered `~/.cargo/bin`.

**Status**: not started

---

### 🎯T24 — Semantic search

`den search` finds packages by intent, not keyword. Pluggable
search providers with increasing capability:

**Provider hierarchy:**

1. **keyword** (default) — substring match on name + description.
   Instant, offline, zero cost. Fallback when nothing else is
   available.

2. **embedding** — vector similarity search over a pre-baked
   embedding index. The entire Homebrew corpus (~8K formulae + ~7.5K
   casks) is embedded at build/release time and shipped as a
   ~24MB index file alongside den. At query time, the query is
   embedded locally (via ollama) and matched against the index.
   Instant, offline after first model pull, no per-query cost.
   Updated with each den release or `den update`.

3. **llm** — a reasoning model (Claude Sonnet/Opus) interprets the
   query, searches the corpus, and returns curated results with
   explanations and workflow suggestions. Not just search —
   consultation. Costs tokens, requires network and an API key.

**Pre-baked corpus index:** The embedding index is built in CI from
the full formulae.brew.sh API dump. Each formula's name, description,
dependencies, and caveats are concatenated and embedded. The index
ships as a binary artifact (e.g. `den-corpus-v1.bin`) and is
version-pinned to avoid query/index model mismatch.

**Configuration:**
```
den set search-provider keyword     # fast, offline
den set search-provider embedding   # semantic, offline
den set search-provider claude-sonnet-4-6  # reasoning
den set search-provider claude-opus-4-6    # best quality
```

`den search --smart` overrides to the highest-configured provider
for a single query without changing the default.

**Status**: not started

---

### 🎯T25 — Search corpus CI pipeline

A separate repo (`den-corpus`) with a CI pipeline that:

1. Runs daily (or on Homebrew API change detection)
2. Fetches the full formula.json + cask.json from formulae.brew.sh
3. Diffs against the previous snapshot, re-embeds changed/new entries
4. Builds the ANN embedding index, BM25 keyword index, and packages
   the cross-encoder reranker model
5. Publishes versioned release artifacts to GitHub Releases with
   a manifest (checksums, model version, build timestamp)

`den update` checks for a new corpus version and downloads it.
The index updates independently of den binary releases. Model
version is pinned in the manifest to prevent query/index mismatch.

Artifacts: `embeddings.bin` (~24MB), `bm25.bin` (~5MB),
`reranker.onnx` (~25MB), `metadata.json`, `manifest.json`.

**Status**: not started

---

### 🎯T26 — Content-addressed Cellar

Replace the `Cellar/name/version/` directory convention with a
content-addressed store keyed by the SHA256 of the keg's contents.
Name and version become metadata pointing into the store.

Benefits:
- **Deduplication** — identical kegs (same formula rebuilt with same
  inputs) occupy one slot regardless of how they got there.
- **Integrity verification** — any keg can be verified at rest by
  rehashing. Corruption is detectable without network access.
- **Untrusted mirrors** — bottles can be served from arbitrary mirrors
  since the content hash is the identity. A compromised mirror can
  only serve correct data or be detected.
- **Atomic installs** — pour into a temp name, verify hash, rename.
  No partial installs.

Inspired by 0install's output-hashed storage and Nix's /nix/store,
but hashing outputs (actual files) rather than inputs (build
instructions). This means we can verify a binary without trusting
the build process.

The store is explicitly non-precious — everything can be re-fetched
from bottles. `den cleanup` can aggressively prune without worry.

**Status**: not started

---

### 🎯T27 — No-code-at-install policy

Bottle pours execute zero code — installation is purely unpack +
verify digest. No post-install scripts, no hooks, no arbitrary
execution. This is enforced as an invariant for den-managed
installs.

Source builds (T11) are the only exception and require explicit
opt-in (`--build-from-source`). When source builds eventually
land, they run in a sandboxed environment with declared
capabilities.

This inverts the Homebrew model where `def post_install` runs
arbitrary Ruby as the installing user. In den, you can pour a
bottle with zero trust in the build system — just verify the hash.

**Status**: partially achieved (bottles already don't run code;
needs enforcement and documentation as a guarantee)

---

### 🎯T28 — Explicit bindings

Replace "link everything from the keg's bin/" with declared
bindings that specify exactly how a package integrates into the
environment. Each formula provides a binding spec:

- `path: bin/ffmpeg` — put this on PATH
- `env: OPENSSL_DIR=/path/to/keg` — set an environment variable
- `env-append: PKG_CONFIG_PATH=/path/to/lib/pkgconfig` — append
- `alias: python3 -> python3.12` — create a named alias

Benefits:
- **Fewer conflicts** — only declared binaries appear, not every
  file in bin/ (many kegs have internal tools not meant for users)
- **Environment variables** — keg_only packages like openssl can
  expose themselves via env vars without symlinking into bin/
- **Precision** — `den env show` can report exactly what each
  package contributes to the environment

Binding specs can be derived from Homebrew formula metadata
(keg_only, link_overwrite) and refined over time. Falls back to
current "link all of bin/" when no binding spec exists.

Inspired by 0install's `<environment>` and `<executable-in-path>`
binding elements.

**Status**: not started

---

### 🎯T29 — SAT-based dependency solver

Replace the current greedy DFS resolver with a SAT-based solver
that handles version constraints, conflicts, and multi-version
coinstallation correctly.

Each candidate version becomes a boolean variable. Dependencies
and conflicts become clauses. The solver finds a satisfying
assignment that maximises a preference function (prefer stable,
prefer newer, prefer already-installed).

Conflict-driven clause learning (CDCL) prunes the search space
when a combination fails, preventing re-exploration of dead ends.

This matters when den manages multiple providers (T23) — a Go
binary might need a specific minimum glibc, a Python package might
conflict with another Python package, and the solver needs to
reason about all of it simultaneously.

Inspired by 0install's OPIUM-derived solver and the broader
SAT-for-packages literature (Debian apt, Nix).

**Status**: not started

---

### 🎯T30 — Stability ratings

Each package version carries a stability level: stable, testing,
developer, buggy, insecure. The solver respects these levels:

- **stable** (default preference) — released and considered safe
- **testing** — pre-release or recently released, not yet proven
- **developer** — HEAD builds, nightly
- **buggy** — known issues, demoted from stable
- **insecure** — known vulnerabilities, never auto-selected

`den set stability testing` opts into pre-release versions.
Per-package overrides: `den set stability tree testing`.
Insecure versions are never selected without explicit `=version`.

The Homebrew API's `deprecated` and `disabled` fields map onto
buggy and insecure. The background daemon (T13) can auto-demote
versions when CVEs are published.

Inspired by 0install's stability ratings as a first-class solver
input.

**Status**: not started

---

### 🎯T31 — Lazy-vendored Ruby

Ruby is not shipped with den and not required for bottle pours.
On first use of a feature that needs it (source builds, third-party
tap formula parsing), den downloads Homebrew's Portable Ruby
bottle into `~/.den/vendor/ruby/<version>/` automatically.

Content-addressed, non-precious (re-fetchable), version-pinned.
Extends to other vendored tools if T23 providers need them.

**Status**: not started

---

### 🎯T32 — Opt-in telemetry

Anonymous, opt-in usage telemetry to answer questions like:

- How often do users trigger Ruby, and *why*?
- Which formulae/casks are most installed via den?
- How many environments does a typical user have?
- How often does the SAT solver hit conflicts?
- Which search provider is most used?

**Ruby trigger categories** — every Ruby invocation is tagged with
a reason category (no formula names, no PII):

| Category | Meaning |
|---|---|
| `source_build_no_bottle` | No bottle for this platform |
| `source_build_user_requested` | User passed `--build-from-source` |
| `source_build_bottle_failed` | Bottle pour failed, fell back |
| `tap_formula_parse` | Third-party tap not on JSON API |
| `tap_formula_dynamic` | Core formula with dynamic Ruby metadata |
| `post_install_hook` | Formula defines `def post_install` |
| `cask_preflight` | Cask with `preflight` block |

Reported as `{category: count}` pairs — enough to know where to
invest (e.g. "80% of Ruby triggers are tap parsing → build a
better Rust parser" vs "80% are post_install → find a way to
eliminate those hooks").

Data drives decisions: if Ruby is triggered <1% of the time,
lazy-vendoring is sufficient. If 30% of installs need it, maybe
we should pre-vendor or find ways to eliminate the dependency.

Privacy-first: opt-in only (`den set telemetry on`), no PII, no
package names (just category counts), aggregated before upload.
Consider differential privacy for small user populations.

**Extreme transparency:** Every telemetry upload prints the exact
JSONL payload to stdout before sending. No hidden fields, no
post-processing, no trust required. What you see is what gets sent.

```
$ den telemetry send
Sending telemetry to https://telemetry.den.dev/v1/report:
{"v":1,"ts":"2026-03-23T12:00:00Z","period":"7d","ruby_triggers":{"tap_formula_parse":4,"post_install_hook":1},"installs":{"bottle":12,"cask":2},"envs":3,"search":{"keyword":8,"embedding":3},"solver_conflicts":0}
Sent. (204 No Content)
```

`den telemetry show` displays the pending payload without sending.
`den telemetry history` shows what was previously sent (kept in
`~/.den/telemetry/sent/` as timestamped JSONL).

**Status**: not started

---

### 🎯T36 — Homebrew prefix compatibility layer

For tools that hardcode `/opt/homebrew` paths, den can optionally
replicate its active environment's symlinks into `/opt/homebrew/`
(or a configurable prefix). This is a one-way sync — den owns the
state, the prefix is a mirror.

Modes:
- **off** (default) — den environments are self-contained, Homebrew
  prefix is untouched
- **mirror** — den materialises into the Homebrew prefix alongside
  its own environment, keeping both in sync
- **takeover** — den replaces Homebrew's symlinks entirely, managing
  the prefix as if it were a den environment

This is the escape hatch for the small number of tools that ignore
env vars and hardcode `/opt/homebrew`. Telemetry (T32) can track
how often users need this to inform whether it's worth maintaining.

**Status**: not started

---

### 🎯T37 — Full brew-to-den migration

`den migrate` becomes a complete, automated migration from Homebrew
to den. The user runs one command and den takes over entirely.

**What it imports:**

1. **Formulae** (current T20) — scan Cellar, build root manifest,
   materialise. Distinguish on-request vs dependency installs from
   INSTALL_RECEIPT.json.

2. **Casks** — scan Caskroom, record installed casks and versions
   in the manifest's `casks` map. Verify the apps exist in
   /Applications.

3. **Taps** — read Homebrew's tap list from Library/Taps/, record
   in den's config so den knows about third-party sources.

4. **Services** — detect running `brew services`, migrate their
   plists to den's service management (T33 when available, or
   the current launchctl approach as fallback).

5. **Shell integration** — detect the user's shell, add
   `eval "$(den init)"` to the right profile file if not already
   present. Offer to comment out `eval "$(brew shellenv)"`.

6. **Verification** — after migration, run a health check:
   - All manifest packages resolve to kegs in the Cellar
   - All expected binaries are accessible via the den environment
   - `pkg-config` resolves key libraries
   - Running services are still running
   - Print a summary: N formulae, N casks, N services migrated

7. **Rollback instructions** — print how to undo if something
   breaks (uncomment brew shellenv, `den daemon stop`).

**Safety:**
- Non-destructive — den reads Homebrew's state but never modifies
  it. Both can coexist indefinitely.
- Idempotent — safe to run multiple times (picks up new installs).
- Dry-run mode — `den migrate --dry-run` shows what would happen.

**Status**: not started (T20 covers step 1 only)

---

### 🎯T5 — Tap management
Third-party taps.
**Status**: not started

### 🎯T11 — Source builds
Delegate to Ruby via lazy-vendored Portable Ruby (T31).
**Status**: not started

### 🎯T17 — Formula metadata parsing (third-party taps)
Ruby DSL parsing for non-API taps. Uses lazy-vendored Ruby (T31).
**Status**: not started

### 🎯T42 — Independent hash verification pipeline

den maintains a verified replica of Homebrew bottle SHA256 hashes in
its own GitHub repo, hosted on a different CDN than formulae.brew.sh.
On install, den cross-references both sources — an attacker must
compromise both Homebrew's CDN and den's GitHub repo simultaneously.

The CI replication job does NOT blindly trust the upstream index.
It builds a diff-based trust model:

1. Fetch the current formula index from formulae.brew.sh.
2. Diff against the previous verified snapshot — identify changed hashes.
3. For each changed hash, independently verify by downloading the
   actual bottle from GHCR and computing SHA256 locally. If the
   bottle's computed hash matches the index claim, the entry is genuine.
   If not, reject it and alert (open a GitHub issue, fail the CI run).
4. Only commit verified changes to the replica (`known_hashes.json`).

This means an attacker who modifies the index must also host a
matching bottle at the correct GHCR path — they can't just change
the hash arbitrarily. The CI job verifies actual bottle content.

Optimisation: only download bottles whose hash changed since the last
snapshot. Typical daily churn is a few dozen formulae, making the
verification tractable. For very large bottles, a HEAD request +
Content-Length check can provide a fast first-pass filter (a hash
change with identical Content-Length is suspicious).

The verified hashes file is fetched by den from GitHub raw content
(separate CDN) and checked alongside the formula index hashes. If
either source is unavailable, den falls back to the other with a
warning. If both are available and disagree, den refuses the install.

**Depends on**: CI/CD pipeline, GitHub repo existence.
**Status**: not started (blocked by CI)

### 🎯T43 — CI/CD pipeline

GitHub Actions workflow for the den project:

- **On push/PR**: `cargo build`, `cargo test`, `cargo clippy -- -D warnings`,
  `cargo fmt --check`, on macOS arm64 and Linux x86_64.
- **On release tag**: cross-compile release binaries for macOS arm64,
  Linux x86_64, Linux arm64. Publish as GitHub Release assets with
  checksums. Update install.sh download URLs.
- **Scheduled (daily)**: Run the 🎯T42 hash verification pipeline —
  fetch Homebrew index, diff, verify changed bottles, commit to
  `known_hashes.json`.

**Status**: not started (requires GitHub repo `marcelocantos/den`)

### 🎯T18 — Testing oracle
Automated Homebrew-equivalence testing.
**Status**: not started

### 🎯T19 — Performance
Benchmarking against Homebrew.
**Status**: not started

# Targets

<!-- last-evaluated: a0e9822 -->

## Active

### 🎯T10 Version switching
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den use pkg=version` updates manifest and re-materialises. Old and new versions coexist in the Cellar. **Achieved.**
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T11 ��� Source builds
- **Value**: 3
- **Cost**: 8
- **Acceptance**: TODO
- **Context**: Delegate to Ruby via bundled Portable Ruby (T31).
- **Depends on**: 🎯T31
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T12 Cask support
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den install --cask` handles DMG and ZIP casks with app artifacts. Downloads, mounts/extracts, copies .app to /Applications, tracks in manifest. **Achieved.**
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T13 Background maintenance daemon
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den daemon run/stop/status/install/uninstall` implemented. Daemon runs as a long-lived process, refreshes the formula index, downloads bottles for outdated packages, and supports auto-upgrade with configurable maintenance windows. State stored in `~/.den/daemon_state.json`. Granular user control via `daemon.auto_download` (default: on) and `daemon.auto_upgrade` (default: off). **Achieved.**
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T14 Info, search, and query commands
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den info`, `den search`, `den deps --tree` all implemented. **Achieved.**
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T15 Cleanup and maintenance
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den cleanup` (remove old kegs), `den doctor` (system health). **Achieved.**
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T16 Services (basic)
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den services list/start/stop/restart` manages launchd plists. **Achieved.** See T33 for the built-in supervisor that replaces this.
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T17 Formula metadata parsing (third-party taps)
- **Value**: 3
- **Cost**: 5
- **Acceptance**: TODO
- **Context**: Ruby DSL parsing for non-API taps. Uses lazy-vendored Ruby (T31).
- **Depends on**: 🎯T31
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T18 Testing oracle
- **Value**: 5
- **Cost**: 8
- **Acceptance**: TODO
- **Context**: Automated Homebrew-equivalence testing.
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T19 Performance
- **Value**: 3
- **Cost**: 5
- **Acceptance**: TODO
- **Context**: Benchmarking against Homebrew.
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T2 Configuration and environment detection
- **Value**: 3
- **Cost**: 2
- **Acceptance**: TODO
- **Context**: Xcode/CLT version detection, full Homebrew config parity.
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T20 Cellar migration
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den migrate` scans the Homebrew Cellar, reads INSTALL_RECEIPT.json for each keg, and populates the root manifest. One command imports the entire existing package state. **Achieved.**
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T21 Uninstall
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den uninstall` removes from the manifest and re-materialises. `den autoremove` stub exists. **Achieved.**
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T22 Update and upgrade
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den outdated` compares manifest vs API. `den upgrade` pours new bottles and re-materialises. **Achieved.**
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T23 Multi-provider package management
- **Value**: 13
- **Cost**: 21
- **Acceptance**: TODO
- **Context**: den becomes a universal frontend for multiple package managers. A `PackageProvider` trait abstracts install/resolve/bin-dir per ecosystem. Providers:
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T24 Semantic search
- **Value**: 5
- **Cost**: 13
- **Acceptance**: TODO
- **Context**: `den search` finds packages by intent, not keyword. Pluggable search providers with increasing capability:  **Provider hierarchy:**  1. **keyword** (default) — substring match on name + description. Instant, offline, zero cost. Fallback when nothing else is available.  2. **embedding** — vector similarity search over a pre-baked embedding index. The entire Homebrew corpus (~8K formulae + ~7.5K casks) is embedded at build/release time and shipped as a ~24MB index file alongside den. At query time, the query is embedded locally (via ollama) and matched against the index. Instant, offline after first model pull, no per-query cost. Updated with each den release or `den update`.  3. **llm** — a reasoning model (Claude Sonnet/Opus) interprets the query, searches the corpus, and returns curated results with explanations and workflow suggestions. Not just search — consultation. Costs tokens, requires network and an API key.  **Pre-baked corpus index:** The embedding index is built in CI from the full formulae.brew.sh API dump. Each formula's name, description, dependencies, and caveats are concatenated and embedded. The index ships as a binary artifact (e.g. `den-corpus-v1.bin`) and is version-pinned to avoid query/index model mismatch.  **Configuration:** ``` den set search-provider keyword     # fast, offline den set search-provider embedding   # semantic, offline den set search-provider claude-sonnet-4-6  # reasoning den set search-provider claude-opus-4-6    # best quality ```  `den search --smart` overrides to the highest-configured provider for a single query without changing the default.
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T25 Search corpus CI pipeline
- **Value**: 3
- **Cost**: 8
- **Acceptance**: TODO
- **Context**: A separate repo (`den-corpus`) with a CI pipeline that:  1. Runs daily (or on Homebrew API change detection) 2. Fetches the full formula.json + cask.json from formulae.brew.sh 3. Diffs against the previous snapshot, re-embeds changed/new entries 4. Builds the ANN embedding index, BM25 keyword index, and packages the cross-encoder reranker model 5. Publishes versioned release artifacts to GitHub Releases with a manifest (checksums, model version, build timestamp)  `den update` checks for a new corpus version and downloads it. The index updates independently of den binary releases. Model version is pinned in the manifest to prevent query/index mismatch.  Artifacts: `embeddings.bin` (~24MB), `bm25.bin` (~5MB), `reranker.onnx` (~25MB), `metadata.json`, `manifest.json`.
- **Depends on**: 🎯T24
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T26 Content-addressed Cellar
- **Value**: 8
- **Cost**: 13
- **Acceptance**: TODO
- **Context**: Replace the `Cellar/name/version/` directory convention with a content-addressed store keyed by the SHA256 of the keg's contents. Name and version become metadata pointing into the store.  Benefits:
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T27 No-code-at-install policy
- **Value**: 5
- **Cost**: 2
- **Acceptance**: TODO
- **Context**: Bottle pours execute zero code — installation is purely unpack + verify digest. No post-install scripts, no hooks, no arbitrary execution. This is enforced as an invariant for den-managed installs.  Source builds (T11) are the only exception and require explicit opt-in (`--build-from-source`). When source builds eventually land, they run in a sandboxed environment with declared capabilities.  This inverts the Homebrew model where `def post_install` runs arbitrary Ruby as the installing user. In den, you can pour a bottle with zero trust in the build system — just verify the hash.
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T28 Explicit bindings
- **Value**: 5
- **Cost**: 8
- **Acceptance**: TODO
- **Context**: Replace "link everything from the keg's bin/" with declared bindings that specify exactly how a package integrates into the environment. Each formula provides a binding spec:  - `path: bin/ffmpeg` — put this on PATH - `env: OPENSSL_DIR=/path/to/keg` — set an environment variable - `env-append: PKG_CONFIG_PATH=/path/to/lib/pkgconfig` — append - `alias: python3 -> python3.12` — create a named alias  Benefits:
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T29 SAT-based dependency solver
- **Value**: 8
- **Cost**: 13
- **Acceptance**: TODO
- **Context**: Replace the current greedy DFS resolver with a SAT-based solver that handles version constraints, conflicts, and multi-version coinstallation correctly.  Each candidate version becomes a boolean variable. Dependencies and conflicts become clauses. The solver finds a satisfying assignment that maximises a preference function (prefer stable, prefer newer, prefer already-installed).  Conflict-driven clause learning (CDCL) prunes the search space when a combination fails, preventing re-exploration of dead ends.  This matters when den manages multiple providers (T23) — a Go binary might need a specific minimum glibc, a Python package might conflict with another Python package, and the solver needs to reason about all of it simultaneously.  Inspired by 0install's OPIUM-derived solver and the broader SAT-for-packages literature (Debian apt, Nix).
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T3 API client
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: Fetches formula metadata from formulae.brew.sh JSON API. **Achieved.**
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T30 Stability ratings
- **Value**: 3
- **Cost**: 5
- **Acceptance**: TODO
- **Context**: Each package version carries a stability level: stable, testing, developer, buggy, insecure. The solver respects these levels:
- **Depends on**: 🎯T29
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T31 Bundled Ruby
- **Value**: 3
- **Cost**: 5
- **Acceptance**: TODO
- **Context**: Ruby is bundled in the den binary (Portable Ruby, 6.7MB compressed, unpacked to `~/.den/ruby/` on first use). With 🎯T49 (shared Cellar), the urgency is reduced — bottles pour at the correct prefix without source builds. Ruby remains needed for third-party tap formula parsing (🎯T17) and source builds when opted in (🎯T11).  Content-addressed, non-precious (re-fetchable), version-pinned.
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T32 Opt-in telemetry
- **Value**: 2
- **Cost**: 8
- **Acceptance**: TODO
- **Context**: Anonymous, opt-in usage telemetry to answer questions like:  - How often do users trigger Ruby, and *why*? - Which formulae/casks are most installed via den? - How many environments does a typical user have? - How often does the SAT solver hit conflicts? - Which search provider is most used?  **Ruby trigger categories** — every Ruby invocation is tagged with a reason category (no formula names, no PII):  | Category | Meaning | |---|---| | `source_build_no_bottle` | No bottle for this platform | | `source_build_user_requested` | User passed `--build-from-source` | | `source_build_bottle_failed` | Bottle pour failed, fell back | | `tap_formula_parse` | Third-party tap not on JSON API | | `tap_formula_dynamic` | Core formula with dynamic Ruby metadata | | `post_install_hook` | Formula defines `def post_install` | | `cask_preflight` | Cask with `preflight` block |  Reported as `{category: count}` pairs — enough to know where to invest (e.g. "80% of Ruby triggers are tap parsing → build a better Rust parser" vs "80% are post_install → find a way to eliminate those hooks").  Data drives decisions: if Ruby is triggered <1% of the time, lazy-vendoring is sufficient. If 30% of installs need it, maybe we should pre-vendor or find ways to eliminate the dependency.  Privacy-first: opt-in only (`den set telemetry on`), no PII, no package names (just category counts), aggregated before upload. Consider differential privacy for small user populations.  **Extreme transparency:** Every telemetry upload prints the exact JSONL payload to stdout before sending. No hidden fields, no post-processing, no trust required. What you see is what gets sent.  ``` $ den telemetry send Sending telemetry to https://telemetry.den.dev/v1/report: {"v":1,"ts":"2026-03-23T12:00:00Z","period":"7d","ruby_triggers":{"tap_formula_parse":4,"post_install_hook":1},"installs":{"bottle":12,"cask":2},"envs":3,"search":{"keyword":8,"embedding":3},"solver_conflicts":0} Sent. (204 No Content) ```  `den telemetry show` displays the pending payload without sending. `den telemetry history` shows what was previously sent (kept in `~/.den/telemetry/sent/` as timestamped JSONL).
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T33 Built-in process supervisor
- **Value**: 8
- **Cost**: 13
- **Acceptance**: TODO
- **Context**: Replace the launchctl-based service management (T16) with a built-in process supervisor in den itself. No external dependency — one binary does the lot.  **Phase 1 (minimal viable supervisor):** - Fork/exec service binary as a child process - PID tracking in `~/.den/services/<name>/pid` - stdout/stderr capture to `~/.den/services/<name>/log` - `den services stop` sends SIGTERM, waits (configurable timeout), then SIGKILL - `den services list` checks live PIDs, shows uptime - `den services logs <name> [-f]` tails/follows the log - Environment-aware: services inherit the active den environment  **Phase 2 (robustness):** - Restart policies: `always`, `on-failure`, `never` (default: never) - Configurable restart backoff (exponential with jitter) - Health checks: TCP port open, HTTP endpoint, custom command - Graceful shutdown ordering (stop dependents before dependencies) - Log rotation (size-based, configurable retention)  **Phase 3 (advanced):** - `den services status` with resource usage (RSS, CPU) - Per-environment services (`/ml` gets its own postgres instance) - Service groups (`den services start dev-stack`) - `den daemon` as a long-lived background process that manages both services and background upgrades (T13), started on first use or at login via a single launchd bootstrap job  The den daemon is the only thing that touches launchd — everything else is supervised directly by den.
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T34 Daemon socket API
- **Value**: 5
- **Cost**: 5
- **Acceptance**: TODO
- **Context**: Extend the daemon (T13) with a Unix socket at `~/.den/den.sock` for real-time CLI↔daemon communication. Required for T33 (process supervision) where the CLI needs to send commands to a running supervisor. Not needed for background maintenance alone.
- **Depends on**: 🎯T33
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T35 Build environment integration
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den init` exports LIBRARY_PATH, CPATH, PKG_CONFIG_PATH, CMAKE_PREFIX_PATH, and MANPATH pointing at the active environment. These swap when `den env use` switches environments. **Achieved.**
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T36 Homebrew prefix compatibility layer
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: **Subsumed by 🎯T49.** With the Cellar at `/opt/homebrew`, no compatibility layer is needed — packages are already at their expected prefix.
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T37 Full brew-to-den migration
- **Value**: 8
- **Cost**: 8
- **Acceptance**: TODO
- **Context**: `den migrate` becomes a complete, automated migration from Homebrew to den. The user runs one command and den takes over entirely.  **What it imports:**  1. **Formulae** (current T20) — scan Cellar, build root manifest, materialise. Distinguish on-request vs dependency installs from INSTALL_RECEIPT.json.  2. **Casks** — scan Caskroom, record installed casks and versions in the manifest's `casks` map. Verify the apps exist in /Applications.  3. **Taps** — read Homebrew's tap list from Library/Taps/, record in den's config so den knows about third-party sources.  4. **Services** — detect running `brew services`, migrate their plists to den's service management (T33 when available, or the current launchctl approach as fallback).  5. **Shell integration** — detect the user's shell, add `eval "$(den init)"` to the right profile file if not already present. Offer to comment out `eval "$(brew shellenv)"`.  6. **Verification** — after migration, run a health check: - All manifest packages resolve to kegs in the Cellar - All expected binaries are accessible via the den environment - `pkg-config` resolves key libraries - Running services are still running - Print a summary: N formulae, N casks, N services migrated  7. **Rollback instructions** — print how to undo if something breaks (uncomment brew shellenv, `den daemon stop`).  **Safety:** - Non-destructive — den reads Homebrew's state but never modifies it. Both can coexist indefinitely. - Idempotent — safe to run multiple times (picks up new installs). - Dry-run mode — `den migrate --dry-run` shows what would happen.
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T38 Python provider (subsume virtualenv)
- **Value**: 8
- **Cost**: 8
- **Acceptance**: TODO
- **Context**: Each den environment gets its own isolated Python with its own site-packages. No separate virtualenv needed.  **How it works:** - Den creates a `pyvenv.cfg` in the environment directory, which makes Python treat it as a virtual environment natively. - `den install numpy` (in `/ml`) runs `uv pip install` (preferred) or `pip install --target` into the environment's `lib/python3.X/site-packages/`. - Python packages are tracked in the manifest under a `pip` key: ```json { "pip": { "numpy": "2.2.1", "torch": "2.6.0" } } ``` - Environment inheritance works: `/ml/experiment` inherits `/ml`'s Python packages and can add or override them. - `den use python@3.11` switches the Python version and re-evaluates package compatibility.  **Provider detection:** `den install numpy` auto-detects pip as the provider (numpy isn't a Homebrew formula). `den install --pip numpy` is explicit. Both work.  **Replaces:** pyenv (version management) + virtualenv/venv (package isolation) + pipx (binary isolation). One tool.
- **Depends on**: 🎯T23
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T39 Node.js/npm provider
- **Value**: 5
- **Cost**: 5
- **Acceptance**: TODO
- **Context**: Each den environment gets its own isolated Node.js with its own global packages.  - `den install prettier` (auto-detected as npm) installs to the environment's `lib/node_modules/` and links binaries to `bin/`. - `den use node@20` switches Node version. - Manifest tracks under `npm` key. - `node_modules` is per-environment, not global. No `npm -g` chaos.  **Replaces:** nvm/fnm (version management) + npm global installs.
- **Depends on**: 🎯T23
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T4 Cellar inspection improvements
- **Value**: 3
- **Cost**: 3
- **Acceptance**: TODO
- **Context**: Show disk usage, which envs reference a keg, all-Cellar listing.
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T40 Go provider
- **Value**: 3
- **Cost**: 3
- **Acceptance**: TODO
- **Context**: - `den install gopls` runs `go install` into environment-local GOBIN. - `den use go@1.22` switches Go toolchain version. - Manifest tracks under `go` key with full module paths. - Go binaries are statically linked so no site-packages equivalent is needed — just the binary in `bin/`.  **Replaces:** manual `go install` + PATH management.
- **Depends on**: 🎯T23
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T41 Cargo provider
- **Value**: 3
- **Cost**: 3
- **Acceptance**: TODO
- **Context**: - `den install bat` runs `cargo install` into environment-local CARGO_INSTALL_ROOT. - `den use rust@1.84` switches Rust toolchain (delegates to rustup if available, or manages versions directly). - Manifest tracks under `cargo` key with crate names. - Like Go, Rust binaries are typically self-contained.  **Replaces:** manual `cargo install` + scattered `~/.cargo/bin`.
- **Depends on**: 🎯T23
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T42 Independent hash verification pipeline
- **Value**: 8
- **Cost**: 8
- **Acceptance**: TODO
- **Context**: den maintains a verified replica of Homebrew bottle SHA256 hashes in its own GitHub repo, hosted on a different CDN than formulae.brew.sh. On install, den cross-references both sources — an attacker must compromise both Homebrew's CDN and den's GitHub repo simultaneously.  The CI replication job does NOT blindly trust the upstream index. It builds a diff-based trust model:  1. Fetch the current formula index from formulae.brew.sh. 2. Diff against the previous verified snapshot — identify changed hashes. 3. For each changed hash, independently verify by downloading the actual bottle from GHCR and computing SHA256 locally. If the bottle's computed hash matches the index claim, the entry is genuine. If not, reject it and alert (open a GitHub issue, fail the CI run). 4. Only commit verified changes to the replica (`known_hashes.json`).  This means an attacker who modifies the index must also host a matching bottle at the correct GHCR path — they can't just change the hash arbitrarily. The CI job verifies actual bottle content.  Optimisation: only download bottles whose hash changed since the last snapshot. Typical daily churn is a few dozen formulae, making the verification tractable. For very large bottles, a HEAD request + Content-Length check can provide a fast first-pass filter (a hash change with identical Content-Length is suspicious).  The verified hashes file is fetched by den from GitHub raw content (separate CDN) and checked alongside the formula index hashes. If either source is unavailable, den falls back to the other with a warning. If both are available and disagree, den refuses the install.
- **Depends on**: 🎯T43
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T44 Advanced trust model
- **Value**: 13
- **Cost**: 21
- **Acceptance**: TODO
- **Context**: den's trust model should substantially exceed Homebrew's. The foundation is in place (🎯T42 independent hash verification, hash pinning, cache sealing, URL allowlist, 0600 permissions). This target covers the remaining layers.
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T44.5 Manifest file locking
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: Advisory flock-based locking around manifest read-modify-write cycles. All read-modify-write call sites use `with_manifest` / `with_manifest_ret` with exclusive `flock`. **Achieved.**  ---
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T45 Shim-free build toolchain
- **Value**: 3
- **Cost**: 8
- **Acceptance**: TODO
- **Context**: Den source builds use Apple's SDK and linker directly without going through the `xcrun` shim layer. This eliminates the "Xcode CLT needs updating" nag and reduces fragile indirection, while still using Apple's proprietary tools where they're genuinely required.  **What Apple provides that has no replacement:** - `ld` (Mach-O linker) — LLVM's `lld` doesn't fully support macOS - SDK headers and `.tbd` stubs — proprietary, framework headers - `codesign`, `install_name_tool` — Apple-specific binary tools  **What den can replace or bypass:** - `clang` — use Homebrew LLVM or den-managed LLVM instead of Xcode's - `xcrun` — resolve tool paths and SDK location directly rather than through Apple's shim, which does version checks and nags - `ar`, `ranlib` — LLVM equivalents (`llvm-ar`) work fine  **Approach (conservative):** 1. Detect SDK path at den init time (`/Applications/Xcode.app/.../SDKs/MacOSX.sdk`) 2. Set `-isysroot`, `-L`, and `-F` paths explicitly in build environment 3. Invoke `/usr/bin/ld` directly (not through xcrun) 4. Use den-managed clang for compilation (falls back to system clang) 5. Skip version compatibility checks — if it builds and links, it works  **Approach (aggressive, deferred):** - Bundle LLVM/Clang as a den-managed package (like Nix does) - Full toolchain isolation — reproducible builds regardless of system state - Only Apple's `ld` and SDK remain as external dependencies  **Risk:** Subtle, difficult-to-diagnose bugs from using a different linker or missing SDK-specific behaviour. The conservative approach mitigates this by keeping Apple's linker and SDK while only replacing the shim layer. The aggressive approach should wait until den has extensive smoke test coverage to catch regressions.  **Decision:** Start with Homebrew's approach (use xcrun, accept the nag) during initial source-build development. Switch to the conservative shim-free approach once source builds are stable and well-tested. Defer the aggressive approach until there's evidence the conservative one is insufficient.
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T46 Eliminate `brew cat` dependency
- **Value**: 5
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: The C++ source build code path (`source_build.cpp`) shells out to `brew cat` in four places to fetch formula source. This defeats the goal of running without Homebrew installed. The Ruby code path (`extract_formula.rb`) already fetches formula source from the GitHub API via the `ruby_source_path` field in the formulae.brew.sh JSON — the C++ path should do the same.  **What to do:** - Fetch formula source from `https://raw.githubusercontent.com/Homebrew/homebrew-core/master/{ruby_source_path}` using the `ruby_source_path` from the JSON API (already available). - Remove all `brew cat` calls from `source_build.cpp`. - The Ruby path already works correctly — this is C++ parity.
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T47 Linux bundled Ruby
- **Value**: 5
- **Cost**: 3
- **Acceptance**: TODO
- **Context**: The bundled Ruby binary is macOS arm64 only. On Linux, source builds fall back to the text parser, which can't handle complex formulas. Den needs to bundle (or lazy-download) Portable Ruby for Linux x86_64 and arm64 to enable source builds on Linux.  **Approach:** - Homebrew publishes Portable Ruby bottles for Linux x86_64. Use the same download-and-cache strategy as macOS. - The Ruby extraction script (`extract_formula.rb`) is platform-neutral — only the Ruby binary differs. - Consider lazy download on first use (aligned with 🎯T31) rather than bundling in the den binary.
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T48 Bottle relocation at scale
- **Value**: 2
- **Cost**: 2
- **Acceptance**: TODO
- **Context**: With 🎯T49 (shared Cellar at /opt/homebrew), bottles pour at their expected prefix and no relocation is needed for package functionality. Relocation is only relevant for den-specific metadata files in `~/.den/`. This target's urgency is greatly reduced.
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T5 Tap management
- **Value**: 5
- **Cost**: 5
- **Acceptance**: TODO
- **Context**: Third-party taps.
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T50 Self-hosting
- **Value**: 5
- **Cost**: 2
- **Acceptance**: TODO
- **Context**: Den manages itself as a package. `den outdated` reports when a new den release is available. `den self-update` downloads the new binary from GitHub Releases, verifies the SHA256, and atomically replaces the running binary.  **Key constraint:** Den is pinned at the globally installed version in every environment. `den use den <version>` is an error — the only way to change den's version is `den self-update`, which updates the binary and every manifest atomically. This prevents different environments from running different den versions, which would risk manifest format mismatches and subtle bugs.  **Bootstrap:** `install.sh` for first install (can't use den before den exists). After that, den manages its own upgrades.  **Mechanism:** 1. Check latest release tag via GitHub Releases API 2. Compare against `DEN_VERSION` 3. Download platform-appropriate tarball to temp path 4. Verify SHA256 checksum 5. `rename(temp, target)` — atomic, no window where binary is missing 6. Update den's version in all manifests  **Future:** This is the seed of 🎯T23 (multi-provider) — den's own GitHub releases are the first non-Homebrew package source.
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T51 Safe automatic upgrades
- **Value**: 5
- **Cost**: 5
- **Acceptance**: TODO
- **Context**: The daemon (🎯T13) should detect running processes from managed packages before applying automatic upgrades. If `openssl` is in use by a running process, don't replace its files until the process exits or the user explicitly requests it.  **Detection:** Scan `/proc/<pid>/maps` (Linux) or `lsof` (macOS) for open files under the Cellar. If any file from a keg is in use, defer that keg's upgrade.
- **Depends on**: 🎯T13
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T52 Restart services after upgrade
- **Value**: 5
- **Cost**: 5
- **Acceptance**: TODO
- **Context**: When a package with a running service (🎯T33) is upgraded, the supervisor should automatically restart the service using the new version. This should be atomic: stop the old service, swap the version link, start the new service. If the new version fails to start, roll back to the old version.
- **Depends on**: 🎯T33
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T6 Dependency resolution
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den install` resolves the full transitive dependency graph via the JSON API (post-order DFS), pours deps before the target, and tracks auto-installed packages in the manifest. **Achieved.**
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T7 Download caching
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: Content-addressed bottle cache in `~/.den/cache/bottles/` with SHA256-keyed storage. Bottles are fetched once and reused across installs. **Achieved.**
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T8 Bottle pouring
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: Downloads and pours Homebrew bottles with SHA256 verification. Multi-version coinstallation works. **Achieved.**
- **Status**: Identified
- **Discovered**: 2026-04-09

### 🎯T9 Environment management
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: Manifest-based environment hierarchy with path naming (/ is root, /ml, /work/legacy). Inheritance at the manifest level — child envs inherit packages and can override versions. Materialisation produces flat symlink directories. Shell integration via `eval "$(den init)"`. **Achieved.**
- **Status**: Identified
- **Discovered**: 2026-04-09

## Achieved

### 🎯T1 Core infrastructure
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: den builds, runs, has module skeleton. **Achieved.**
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T43 CI/CD pipeline
- **Value**: 13
- **Cost**: 3
- **Acceptance**:
  - CI workflow (.github/workflows/ci.yml) runs build + test on macOS and Linux for every push to master
  - Release workflow (.github/workflows/release.yml) builds binaries for darwin-aarch64, linux-x86_64, linux-aarch64 and uploads to GitHub Release
  - CI is green on master
- **Context**: GitHub Actions workflow for the den project:
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11

### 🎯T49 Shared Cellar at /opt/homebrew, environments in ~/.den
- **Value**: 13
- **Cost**: 1.5
- **Acceptance**: TODO
- **Context**: Den uses `/opt/homebrew/Cellar/` as the blessed package store — the same location Homebrew uses. Bottles pour directly into their expected prefix with zero relocation needed. Den and Homebrew coexist on the same Cellar.  Environments live in `~/.den/envs/` as symlink sets pointing into the Cellar. `den env use /ml` switches which symlinks are on PATH. Multiple versions coexist in the Cellar; environments pick which version to link.  **What this changes:** - `~/.den/store/` → `/opt/homebrew/Cellar/` as the package store - 100% of bottles pour and work immediately — no source builds needed for hardcoded-prefix packages (openssl, python, git, gcc) - Source builds (🎯T11) become nice-to-have (taps, custom patches, platforms without bottles) rather than critical path - Bundled Ruby (🎯T31) urgency drops — not needed for the common case - Bottle relocation (🎯T48) only matters for the `~/.den` metadata, not for package functionality - 🎯T36 (prefix compatibility layer) is subsumed — no compatibility layer needed when the Cellar is already at the expected prefix - Migration (🎯T37) becomes simpler — den reads the existing Cellar in place rather than copying/importing  **Coexistence with Homebrew:** - Den reads and writes to `/opt/homebrew/Cellar/` alongside Homebrew - Both tools can install packages — den tracks what it manages via its own manifest in `~/.den/` - `den migrate` just adopts the existing Cellar contents into den's manifest — zero file movement  **What den adds over Homebrew:** - Named environments (symlink sets with PATH switching) - Multi-version coinstallation (both versions in Cellar, env picks one) - Background upgrades staged without disruption - Built-in process supervisor - Multi-provider package management (go, cargo, pip, npm)
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 2

### 🎯T53 Upgrade activity is logged and queryable via `den log`
- **Value**: 5
- **Cost**: 3
- **Acceptance**:
  - A structured activity log records every upgrade event (package, old version, new version, timestamp, trigger: manual/auto/daemon)
  - `den log` displays recent upgrade activity in human-readable format
  - `den log --json` outputs machine-readable activity
  - Both manual `den upgrade` and daemon auto-upgrades write to the log
  - Log is stored in ~/.den/activity.json (append-only, atomic writes)
- **Context**: Den currently has a daemon.log (unstructured text) and daemon_state.json (current state only, no history). Users need visibility into what was upgraded, when, and by what trigger — especially important when the daemon auto-upgrades in the background. Without a queryable history, users can't answer "what changed?" after something breaks.
- **Tags**: cli, daemon
- **Origin**: user request
- **Status**: Achieved
- **Discovered**: 2026-04-10
- **Achieved**: 2026-04-10
- **Actual-cost**: 3

## Graph

```mermaid
graph TD
    T10["Version switching"]
    T11["��� Source builds"]
    T12["Cask support"]
    T13["Background maintenance daemon"]
    T14["Info, search, and query comma…"]
    T15["Cleanup and maintenance"]
    T16["Services (basic)"]
    T17["Formula metadata parsing (thi…"]
    T18["Testing oracle"]
    T19["Performance"]
    T2["Configuration and environment…"]
    T20["Cellar migration"]
    T21["Uninstall"]
    T22["Update and upgrade"]
    T23["Multi-provider package manage…"]
    T24["Semantic search"]
    T25["Search corpus CI pipeline"]
    T26["Content-addressed Cellar"]
    T27["No-code-at-install policy"]
    T28["Explicit bindings"]
    T29["SAT-based dependency solver"]
    T3["API client"]
    T30["Stability ratings"]
    T31["Bundled Ruby"]
    T32["Opt-in telemetry"]
    T33["Built-in process supervisor"]
    T34["Daemon socket API"]
    T35["Build environment integration"]
    T36["Homebrew prefix compatibility…"]
    T37["Full brew-to-den migration"]
    T38["Python provider (subsume virt…"]
    T39["Node.js/npm provider"]
    T4["Cellar inspection improvements"]
    T40["Go provider"]
    T41["Cargo provider"]
    T42["Independent hash verification…"]
    T44["Advanced trust model"]
    T44_5["Manifest file locking"]
    T45["Shim-free build toolchain"]
    T46["Eliminate `brew cat` dependen…"]
    T47["Linux bundled Ruby"]
    T48["Bottle relocation at scale"]
    T5["Tap management"]
    T50["Self-hosting"]
    T51["Safe automatic upgrades"]
    T52["Restart services after upgrade"]
    T6["Dependency resolution"]
    T7["Download caching"]
    T8["Bottle pouring"]
    T9["Environment management"]
    T11 -.->|needs| T31
    T17 -.->|needs| T31
    T25 -.->|needs| T24
    T30 -.->|needs| T29
    T34 -.->|needs| T33
    T38 -.->|needs| T23
    T39 -.->|needs| T23
    T40 -.->|needs| T23
    T41 -.->|needs| T23
    T51 -.->|needs| T13
    T52 -.->|needs| T33
```

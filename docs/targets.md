# Targets

<!-- last-evaluated: a0e9822 -->

## Active

### 🎯T11 ��� Source builds
- **Value**: 3
- **Cost**: 8
- **Acceptance**: TODO
- **Context**: Delegate to Ruby via bundled Portable Ruby (T31).
- **Depends on**: 🎯T31
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

### 🎯T18 Native formula parser is sound for simple cases and refuses complex ones cleanly
- **Value**: 5
- **Cost**: 8
- **Acceptance**:
  - ✅ **DONE (PR #20, 7e2c073)** — The native parser (`src/build/formula_parser.cpp`) classifies every formula as SIMPLE / COMPLEX / UNSUPPORTED via `FormulaComplexity`. Every unhandled DSL construct (`if`/`unless`/`case`/`on_macos`/`on_linux`/`on_arm`/`resource`/`patch`/`inreplace`/`bottle`/generic `do`) appends a `ComplexityMarker` (with line number and offending text) and downgrades the verdict; the parser keeps reading so every offender is collected, not just the first. Bare `unknown-statement` lines are flagged too. `source_build.cpp` refuses to apply parser output on anything other than Simple.
  - ✅ **DONE (PR #20)** — Complexity explainability: every COMPLEX verdict carries `{construct, line, detail}` markers identifying *why*. Unit tests in `tests/test_formula_parser.cpp` cover each unhandled construct, multi-marker collection, and whole-word keyword matching (`iffy_tool` ≠ `if`).
  - ❌ **REMAINING — soundness oracle.** A test binary runs the native parser *and* the Ruby extraction pipeline (`src/ruby/extract_formula.rb`) on a corpus of real formulae (popularity-tiered). For every formula classified as SIMPLE, the extracted metadata must match Ruby's output field-by-field on the fields den consumes: `url`, `mirrors`, `sha256`, `version`, `dependencies` (build / runtime / optional / recommended / test), `license`, `homepage`, `keg_only`, resolved install commands, env settings. A SIMPLE classification with a field mismatch fails the test.
  - ❌ **REMAINING — curated corpus.** A committed corpus (start with top-100, grow to top-500) with hand-verified expected verdicts at `tests/corpus/`. Each entry names the formula, its expected classification, and — for COMPLEX — the expected complexity markers.
  - ❌ **REMAINING — CI gate.** The oracle runs on every PR. The SIMPLE baseline lives at `tests/corpus/simple_formulae.txt`. Regressions (SIMPLE → COMPLEX, SIMPLE → oracle mismatch) fail the PR. Expanding the SIMPLE set is allowed but requires a baseline update in the same PR.
  - ❌ **REMAINING — installable-for-real check.** A subset of SIMPLE-classified corpus entries are installed end-to-end via den in CI using only the parser's output (no Ruby). A failed install fails the PR. Guards completeness: metadata isn't just equivalent to Ruby's, it's sufficient to drive a real install.
- **Context**: **Premise (corrected 2026-04-11 after audit).** The native C++ parser at `src/build/formula_parser.cpp` (NB: not `src/formula/…`) is a **fast path for simple formulae**, not a replacement for Ruby. Ruby remains permanent infrastructure for the complex path — conditional blocks, resources, patches, bottle specs, inreplace, arbitrary DSL method calls. The native parser exists so den can install simple formulae without spinning up a Ruby interpreter.

**Status (2026-04-12).** PR #20 (7e2c073) landed the core soundness fix: `FormulaComplexity` enum, per-construct complexity markers with line numbers and detail payloads, "no silent skipping" invariant, and 14 new unit tests covering every known unhandled construct. `source_build.cpp` refuses to apply parser output unless the verdict is Simple. All 106 existing tests still green.

**What's left.** The unit tests prove the parser doesn't silently drop *the constructs we've thought of*. The oracle is the missing piece — it proves the parser's SIMPLE extractions actually match Ruby's across real formulae, catches silent drops we haven't anticipated, and establishes a regression gate. The oracle has four sub-pieces (test harness, corpus, CI gate, installable-for-real), any of which could be scoped as its own PR.

**Design questions still open** (for the oracle PR):
- Where does the corpus live? Shipped in-tree (`tests/corpus/*.rb`) vs fetched at test time vs fetched in CI only. In-tree is reproducible but grows the repo.
- How does the C++ test binary invoke Ruby? Via the existing bundled Ruby path (`ensure_ruby_bundle()` + `extract_formula.rb`) — reuses infrastructure already in source_build.cpp.
- Popularity tiering: start with top-100 for speed, grow to top-500 once stable. Track install analytics from formulae.brew.sh.
- How strict is the dependency comparison? Ruby's output groups deps by phase; the native parser currently doesn't extract deps at all (only install commands + env). The oracle likely needs to extend the native parser to emit dependencies, or the comparison is vacuous on that field.

**Relationship to other targets.** This target is not a stepping stone toward retiring Ruby — 🎯T54 (retire bundled Ruby) was retired on the same day this was reframed because its premise was wrong. Ruby is permanent. 🎯T18 is about making the fast path *trustworthy*, not about eliminating the slow path.

**Out of scope:** extending the fast path to handle any new DSL constructs (that's follow-up work, filed as separate targets). Making the parser a general-purpose Ruby evaluator (never). Matching Ruby's extraction on any field den does not consume.
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

### 🎯T30 Stability ratings
- **Value**: 3
- **Cost**: 5
- **Acceptance**: TODO
- **Context**: Each package version carries a stability level: stable, testing, developer, buggy, insecure. The solver respects these levels:
- **Depends on**: 🎯T29
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

### 🎯T45 Shim-free build toolchain
- **Value**: 3
- **Cost**: 8
- **Acceptance**: TODO
- **Context**: Den source builds use Apple's SDK and linker directly without going through the `xcrun` shim layer. This eliminates the "Xcode CLT needs updating" nag and reduces fragile indirection, while still using Apple's proprietary tools where they're genuinely required.  **What Apple provides that has no replacement:** - `ld` (Mach-O linker) — LLVM's `lld` doesn't fully support macOS - SDK headers and `.tbd` stubs — proprietary, framework headers - `codesign`, `install_name_tool` — Apple-specific binary tools  **What den can replace or bypass:** - `clang` — use Homebrew LLVM or den-managed LLVM instead of Xcode's - `xcrun` — resolve tool paths and SDK location directly rather than through Apple's shim, which does version checks and nags - `ar`, `ranlib` — LLVM equivalents (`llvm-ar`) work fine  **Approach (conservative):** 1. Detect SDK path at den init time (`/Applications/Xcode.app/.../SDKs/MacOSX.sdk`) 2. Set `-isysroot`, `-L`, and `-F` paths explicitly in build environment 3. Invoke `/usr/bin/ld` directly (not through xcrun) 4. Use den-managed clang for compilation (falls back to system clang) 5. Skip version compatibility checks — if it builds and links, it works  **Approach (aggressive, deferred):** - Bundle LLVM/Clang as a den-managed package (like Nix does) - Full toolchain isolation — reproducible builds regardless of system state - Only Apple's `ld` and SDK remain as external dependencies  **Risk:** Subtle, difficult-to-diagnose bugs from using a different linker or missing SDK-specific behaviour. The conservative approach mitigates this by keeping Apple's linker and SDK while only replacing the shim layer. The aggressive approach should wait until den has extensive smoke test coverage to catch regressions.  **Decision:** Start with Homebrew's approach (use xcrun, accept the nag) during initial source-build development. Switch to the conservative shim-free approach once source builds are stable and well-tested. Defer the aggressive approach until there's evidence the conservative one is insufficient.
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

### 🎯T57 Native parser refuses unresolvable `#{…}` interpolations
- **Value**: 3
- **Cost**: 3
- **Acceptance**:
  - The native parser (`src/build/formula_parser.cpp`) treats any `#{...}` Ruby interpolation it cannot resolve to a concrete string as a complexity marker (`construct: "interpolation"`) and downgrades the formula to COMPLEX. The current set of resolvable interpolations is the fixed list in `parse_system_call` (`#{prefix}`, `#{lib}`, `#{bin}`, `#{include}`, `#{share}`, `#{sbin}`, `#{libexec}`, `#{man}`, `#{pkgshare}`, `#{etc}`, `#{var}`, `#{rpath}`). Anything else — `#{ENV.cc}`, `#{Formula["openssl@3"].opt_prefix}`, `#{version}`, etc. — is unresolvable and must not be silently passed through.
  - Unit test covers: `system "./configure", "--with-ssl=#{Formula[\"openssl@3\"].opt_prefix}"` classifies as COMPLEX with a `interpolation` marker pointing at the offending substring.
  - Oracle test `wget` moves from the COMPLEX baseline (where it currently sits for the `trailing-conditional` marker) to also carry an `interpolation` expectation, or the wget-specific entry is updated to list both.
- **Context**: **Discovered 2026-04-12** while building the 🎯T18 oracle. `wget`'s install body contains `"--with-libssl-prefix=#{Formula["openssl@3"].opt_prefix}"`, which the native parser currently passes through verbatim as a literal argument string. The resulting "build command" has an unresolved `#{...}` in it that would fail at exec time (or worse, succeed with the wrong path if something later does textual substitution).

**Why this is a soundness bug.** Under the 🎯T18 framing, the parser must either handle a construct or refuse. Unresolvable interpolation is a silent partial-handling case: the line *looks* like it was parsed, build_commands is populated, and the verdict is SIMPLE — but the output is nonsense. This is exactly the class of failure 🎯T18 is meant to prevent.

**Why it's a separate target** and not folded into 🎯T18: 🎯T18 is about the parser refusing unhandled *constructs* (blocks, trailing modifiers, top-level DSL methods). This is a distinct failure mode inside the handled `system`/`ENV`/`mkdir_p` branches, and fixing it is orthogonal — it's about auditing string substitution, not about extending the grammar.

**Forked from 🎯T18.** Found during oracle baseline expansion on 2026-04-12 while adding wget as a COMPLEX example. wget currently classifies as COMPLEX for the right trailing-conditional reason, but if that line were removed, the parser would happily return SIMPLE with a nonsense `--with-libssl-prefix=#{Formula["openssl@3"].opt_prefix}` command.

**Implementation sketch:** After all known interpolations are substituted in `parse_system_call`'s inner loop, check whether the value still contains `#{` — if so, record an `interpolation` complexity marker pointing at the remaining token and downgrade the verdict. Same in the `mkdir_p` branch and anywhere else substitution happens.
- **Origin**: forked from 🎯T18 on 2026-04-12
- **Status**: Identified
- **Discovered**: 2026-04-12

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

### 🎯T10 Version switching
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den use pkg=version` updates manifest and re-materialises. Old and new versions coexist in the Cellar. **Achieved.**
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T12 Cask support
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den install --cask` handles DMG and ZIP casks with app artifacts. Downloads, mounts/extracts, copies .app to /Applications, tracks in manifest. **Achieved.**
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T13 Background maintenance daemon
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den daemon run/stop/status/install/uninstall` implemented. Daemon runs as a long-lived process, refreshes the formula index, downloads bottles for outdated packages, and supports auto-upgrade with configurable maintenance windows. State stored in `~/.den/daemon_state.json`. Granular user control via `daemon.auto_download` (default: on) and `daemon.auto_upgrade` (default: off). **Achieved.**
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T14 Info, search, and query commands
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den info`, `den search`, `den deps --tree` all implemented. **Achieved.**
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T15 Cleanup and maintenance
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den cleanup` (remove old kegs), `den doctor` (system health). **Achieved.**
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T16 Services (basic)
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den services list/start/stop/restart` manages launchd plists. **Achieved.** See T33 for the built-in supervisor that replaces this.
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T20 Cellar migration
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den migrate` scans the Homebrew Cellar, reads INSTALL_RECEIPT.json for each keg, and populates the root manifest. One command imports the entire existing package state. **Achieved.**
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T21 Uninstall
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den uninstall` removes from the manifest and re-materialises. `den autoremove` stub exists. **Achieved.**
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T22 Update and upgrade
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den outdated` compares manifest vs API. `den upgrade` pours new bottles and re-materialises. **Achieved.**
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T27 No-code-at-install policy
- **Value**: 5
- **Cost**: 2
- **Acceptance**:
  - Bottle installs execute zero package-provided code — only unpack + verify + relocate
  - No post_install hooks or arbitrary script execution during den install
  - Source builds require explicit --build-from-source flag
  - install_name_tool calls are den's own relocation, not package scripts
- **Context**: Bottle pours execute zero code — installation is purely unpack + verify digest. No post-install scripts, no hooks, no arbitrary execution. This is enforced as an invariant for den-managed installs.  Source builds (T11) are the only exception and require explicit opt-in (`--build-from-source`). When source builds eventually land, they run in a sandboxed environment with declared capabilities.  This inverts the Homebrew model where `def post_install` runs arbitrary Ruby as the installing user. In den, you can pour a bottle with zero trust in the build system — just verify the hash.
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T3 API client
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: Fetches formula metadata from formulae.brew.sh JSON API. **Achieved.**
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T31 Bundled Ruby
- **Value**: 3
- **Cost**: 5
- **Acceptance**: TODO
- **Context**: **Superseded by 🎯T54** on 2026-04-11.

T31's original framing — "Ruby is bundled, version-pinned, re-fetchable, optimise the bundle pipeline" — treated Ruby as permanent infrastructure and proposed investment to polish it. That framing was invalidated when we identified Ruby as a temporary compatibility shim whose only purpose is evaluating legacy Homebrew formula `.rb` files that the native C++ parser cannot yet handle. Under the correct framing, the target isn't "ship a better Ruby bundle" but "delete Ruby entirely" — that's 🎯T54.

Ongoing Ruby-subsystem maintenance (keeping the Linux lazy-download working, bumping the pinned version if it rots) continues as operational work, not targeted convergence. If it breaks enough to matter before 🎯T54 lands, fix it in-place; don't file new sub-targets against a dead subsystem.

**Original context (for history):** Ruby is bundled in the den binary (Portable Ruby, 6.7MB compressed, unpacked to `~/.den/ruby/` on first use). With 🎯T49 (shared Cellar), the urgency is reduced — bottles pour at the correct prefix without source builds. Content-addressed, non-precious (re-fetchable), version-pinned.
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T35 Build environment integration
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den init` exports LIBRARY_PATH, CPATH, PKG_CONFIG_PATH, CMAKE_PREFIX_PATH, and MANPATH pointing at the active environment. These swap when `den env use` switches environments. **Achieved.**
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T36 Homebrew prefix compatibility layer
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: **Subsumed by 🎯T49.** With the Cellar at `/opt/homebrew`, no compatibility layer is needed — packages are already at their expected prefix.
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

### 🎯T46 Eliminate `brew cat` dependency
- **Value**: 5
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: The C++ source build code path (`source_build.cpp`) shells out to `brew cat` in four places to fetch formula source. This defeats the goal of running without Homebrew installed. The Ruby code path (`extract_formula.rb`) already fetches formula source from the GitHub API via the `ruby_source_path` field in the formulae.brew.sh JSON — the C++ path should do the same.  **What to do:** - Fetch formula source from `https://raw.githubusercontent.com/Homebrew/homebrew-core/master/{ruby_source_path}` using the `ruby_source_path` from the JSON API (already available). - Remove all `brew cat` calls from `source_build.cpp`. - The Ruby path already works correctly — this is C++ parity.
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 2

### 🎯T47 Linux bundled Ruby
- **Value**: 5
- **Cost**: 3
- **Acceptance**:
  - On Linux x86_64 and Linux arm64, `ensure_ruby_bundle()` lazy-downloads Portable Ruby from the pinned `homebrew-portable-ruby` GitHub release, verifies a pinned SHA-256, extracts to `~/.den/ruby/`, and reuses on subsequent runs. (Shipped: src/ruby/portable_ruby.{h,cpp} + portable_ruby_manifest.h.)
  - Platform detection selects the correct asset per host arch: `arm64_linux.bottle.tar.gz` for Linux arm64, `x86_64_linux.bottle.tar.gz` for Linux x86_64. (Shipped: `asset_for_host()` in src/ruby/portable_ruby.cpp.)
  - Linux builds compile and link on CI's `ubuntu-latest` matrix, proving the platform-branched `ensure_ruby_bundle()` works end-to-end through the compiler and linker. (Shipped: PR #17 CI green.)
  - macOS (arm64 + x86_64) continues to use the existing embedded-bundle path with no behavioural change. (Shipped: `#ifdef __APPLE__` in bundle.cpp; `bundle_data.c` gated on `APPLE` in CMakeLists.)
  - Manifest invariants are unit-tested cross-platform: URL format, pinned version presence, hex-only SHA-256, distinct per-arch hashes. (Shipped: tests/test_portable_ruby.cpp.)
- **Context**: Closed 2026-04-11 via PR #17 (merged as `7f90deb`).

**What landed:** A lazy-download path for Portable Ruby 3.4.5 on Linux (both arm64 and x86_64), parallel to the existing macOS embedded-bundle path. `ensure_ruby_bundle()` platform-branches via `#ifdef __APPLE__`. Manifest constants pin version and SHA-256s; unit tests guard the invariants on both platforms. CI matrix (`ubuntu-latest` + `macos-14` + Format) is green.

**Dropped from original acceptance:** The original criterion 4 required a Linux CI integration test that actually drives an end-to-end source build through Ruby (to prove the text-parser fallback is not in use). That criterion was *consciously dropped* when 🎯T54 was identified: under the sunset framing, building new CI infrastructure to verify a subsystem scheduled for deletion is debt, not investment. Compile/link validation on Linux CI is sufficient to prove the code path is structurally sound; functional correctness of the Ruby-backed source-build path is covered by 🎯T18 (testing oracle), which is the right place to measure it once.

**Superseded-by:** 🎯T54 — the real termination of this work is deleting `src/ruby/` entirely.
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 2

### 🎯T49 Shared Cellar at /opt/homebrew, environments in ~/.den
- **Value**: 13
- **Cost**: 1.5
- **Acceptance**: TODO
- **Context**: Den uses `/opt/homebrew/Cellar/` as the blessed package store — the same location Homebrew uses. Bottles pour directly into their expected prefix with zero relocation needed. Den and Homebrew coexist on the same Cellar.  Environments live in `~/.den/envs/` as symlink sets pointing into the Cellar. `den env use /ml` switches which symlinks are on PATH. Multiple versions coexist in the Cellar; environments pick which version to link.  **What this changes:** - `~/.den/store/` → `/opt/homebrew/Cellar/` as the package store - 100% of bottles pour and work immediately — no source builds needed for hardcoded-prefix packages (openssl, python, git, gcc) - Source builds (🎯T11) become nice-to-have (taps, custom patches, platforms without bottles) rather than critical path - Bundled Ruby (🎯T31) urgency drops — not needed for the common case - Bottle relocation (🎯T48) only matters for the `~/.den` metadata, not for package functionality - 🎯T36 (prefix compatibility layer) is subsumed — no compatibility layer needed when the Cellar is already at the expected prefix - Migration (🎯T37) becomes simpler — den reads the existing Cellar in place rather than copying/importing  **Coexistence with Homebrew:** - Den reads and writes to `/opt/homebrew/Cellar/` alongside Homebrew - Both tools can install packages — den tracks what it manages via its own manifest in `~/.den/` - `den migrate` just adopts the existing Cellar contents into den's manifest — zero file movement  **What den adds over Homebrew:** - Named environments (symlink sets with PATH switching) - Multi-version coinstallation (both versions in Cellar, env picks one) - Background upgrades staged without disruption - Built-in process supervisor - Multi-provider package management (go, cargo, pip, npm)
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 2

### 🎯T50 Self-hosting
- **Value**: 5
- **Cost**: 2
- **Acceptance**:
  - den self-update checks GitHub Releases, downloads, verifies SHA256, and atomically replaces the binary
  - den outdated reports when a newer den release is available
  - den status shows the current den version
  - install.sh bootstraps den on a fresh system
- **Context**: Den manages itself as a package. `den outdated` reports when a new den release is available. `den self-update` downloads the new binary from GitHub Releases, verifies the SHA256, and atomically replaces the running binary.  **Key constraint:** Den is pinned at the globally installed version in every environment. `den use den <version>` is an error — the only way to change den's version is `den self-update`, which updates the binary and every manifest atomically. This prevents different environments from running different den versions, which would risk manifest format mismatches and subtle bugs.  **Bootstrap:** `install.sh` for first install (can't use den before den exists). After that, den manages its own upgrades.  **Mechanism:** 1. Check latest release tag via GitHub Releases API 2. Compare against `DEN_VERSION` 3. Download platform-appropriate tarball to temp path 4. Verify SHA256 checksum 5. `rename(temp, target)` — atomic, no window where binary is missing 6. Update den's version in all manifests  **Future:** This is the seed of 🎯T23 (multi-provider) — den's own GitHub releases are the first non-Homebrew package source.
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T54 Bundled Ruby runtime is fully retired — den parses and builds all supported formulae natively in C++, with no Ruby process ever spawned at runtime
- **Value**: 13
- **Cost**: 13
- **Acceptance**:
  - `src/ruby/` is deleted. No `ensure_ruby_bundle`, no Portable Ruby manifest, no `extract_formula.rb`, no `den_build.rb`.
  - `vendor/den-ruby-bundle*.tar.zst` and `src/ruby/bundle_data.c` are deleted. `cmake/embed_resource.cmake` is deleted (it has no other callers).
  - `source_build.cpp` (and its successors) contain no Ruby-path, no `brew ruby` fallback, and no text-parser-as-last-resort code. A single native C++ formula interpreter handles every supported formula.
  - den's smoke test suite installs and source-builds the full popularity-tiered formula set (per 🎯feedback `project_testing_approach`) with no Ruby interpreter present on the host — verified by running tests inside a container that has no Ruby installed.
  - The `_den_ruby_bundle_*` symbols and all `#ifdef __APPLE__` branches added for the 4.0.2 embedded bundle are gone. One code path, one build, no compatibility shim.
- **Context**: **Retired 2026-04-11 — premise was wrong.**

T54 was filed earlier the same day under the framing "Ruby in den is a temporary compatibility shim, delete it when the native parser covers all formulae". An audit of the native parser (`src/formula/formula_parser.cpp`) later that day showed the parser is a regex-based fast-path extractor for simple formulae, never intended to replace Ruby. Under the correct framing, **Ruby is permanent infrastructure for the complex path** — anything involving conditional blocks (`on_macos`/`on_linux`), resources, patches, bottle specs, inreplace, or arbitrary DSL method calls. The native parser is a performance/footprint optimisation with a narrow scope, not an eventual-replacement project.

There is no sunset. T54 should not exist. Retiring it rather than editing further, so the mistake is visible in history and future `/cv` runs don't trip over a stale north star.

**What replaces it:** 🎯T18 (reframed the same day) now owns the native-parser-soundness work — the real near-term target is ensuring the fast path never silently mis-classifies a complex formula as simple. That's valuable on its own terms, independent of any sunset narrative.

**Superseded original context:** den's Ruby runtime was described as a compatibility shim scheduled for retirement. The target's acceptance criteria required deleting `src/ruby/`, `vendor/den-ruby-bundle*`, `cmake/embed_resource.cmake`, and all Ruby-path fallbacks — under the (incorrect) assumption that a future native parser would cover 99%+ of supported formulae. Actual native-parser coverage is ~3.5 of 10 common DSL constructs, and the gap is by design, not by neglect.
- **Tags**: sunset, ruby, simplification
- **Origin**: Discovered during 🎯T47 implementation, 2026-04-11 — reframing Ruby as a temporary compatibility shim rather than permanent infrastructure
- **Status**: Achieved
- **Discovered**: 2026-04-11
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T55 Install-body extractor tracks nesting depth correctly for method-with-do forms
- **Value**: 2
- **Cost**: 2
- **Acceptance**:
  - `src/build/formula_parser.cpp::extract_install_body` recognises any line ending in ` do` or `|` as opening a new scope, not just `def`/`do`/`if`/`unless`/`case`/`begin`. In particular, `resource "name" do`, `Dir.chdir("x") do`, and `each do |x|` all increment depth so the matching `end` does not prematurely terminate the install body.
  - Regression test: a formula whose install body contains `resource "extra" do ... end` followed by further statements (`inreplace`, `system`, etc.) is fully scanned — all trailing statements contribute complexity markers instead of being silently truncated.
  - No existing passing test cases regress.
- **Context**: Discovered on 2026-04-11 while implementing 🎯T18's complexity-marker scaffolding. The `extract_install_body` helper (introduced in the C++ parser) tracks nesting depth with a narrow keyword list (`def`/`do`/`if`/`unless`/`case`/`begin`). Any line of the form `<method> "arg" do` (e.g. `resource "x" do`, `Dir.chdir("x") do`, `each do |x|`) does NOT increment depth, but the matching `end` DOES decrement it — so the extractor closes the install body early. The 🎯T18 parser now flags `resource`/`do` with complexity markers as designed, but any statements *after* the nested block are never seen by the parser loop at all, because the body string was already truncated upstream. This is a silent-data-loss bug in a different layer from the one 🎯T18 closes. **How to apply:** Fix by treating any line that matches ` do` / `|` / starts with `do ` / ends with `do` as a depth-increment, mirroring the complexity detector's heuristics. Add a regression test mirroring the one that's currently working around this (see `tests/test_formula_parser.cpp` — the "collects ALL markers" case had to drop a `resource` block because of this bug). Keep changes scoped to `extract_install_body`; the complexity detector stays authoritative for marker emission.
- **Tags**: parser, soundness, forked-from-T18
- **Origin**: Discovered on 2026-04-11 during 🎯T18 implementation — unit test for marker collection hit early-termination in extract_install_body
- **Status**: Achieved
- **Discovered**: 2026-04-11
- **Achieved**: 2026-04-11
- **Actual-cost**: 2

### 🎯T56 Manifest file locking
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: Advisory flock-based locking around manifest read-modify-write cycles. All read-modify-write call sites use `with_manifest` / `with_manifest_ret` with exclusive `flock`. **Achieved.**  Re-parented 2026-04-12 from 🎯T44.5 — concurrency fix, not a trust-model layer. Original 🎯T44 umbrella remains identified and awaits concrete acceptance criteria.
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T6 Dependency resolution
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: `den install` resolves the full transitive dependency graph via the JSON API (post-order DFS), pours deps before the target, and tracks auto-installed packages in the manifest. **Achieved.**
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T7 Download caching
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: Content-addressed bottle cache in `~/.den/cache/bottles/` with SHA256-keyed storage. Bottles are fetched once and reused across installs. **Achieved.**
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T8 Bottle pouring
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: Downloads and pours Homebrew bottles with SHA256 verification. Multi-version coinstallation works. **Achieved.**
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

### 🎯T9 Environment management
- **Value**: 1
- **Cost**: 1
- **Acceptance**: TODO
- **Context**: Manifest-based environment hierarchy with path naming (/ is root, /ml, /work/legacy). Inheritance at the manifest level — child envs inherit packages and can override versions. Materialisation produces flat symlink directories. Shell integration via `eval "$(den init)"`. **Achieved.**
- **Status**: Achieved
- **Discovered**: 2026-04-09
- **Achieved**: 2026-04-11
- **Actual-cost**: 1

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
    T11["��� Source builds"]
    T17["Formula metadata parsing (thi…"]
    T18["Native formula parser is soun…"]
    T19["Performance"]
    T2["Configuration and environment…"]
    T23["Multi-provider package manage…"]
    T24["Semantic search"]
    T25["Search corpus CI pipeline"]
    T26["Content-addressed Cellar"]
    T28["Explicit bindings"]
    T29["SAT-based dependency solver"]
    T30["Stability ratings"]
    T32["Opt-in telemetry"]
    T33["Built-in process supervisor"]
    T34["Daemon socket API"]
    T37["Full brew-to-den migration"]
    T38["Python provider (subsume virt…"]
    T39["Node.js/npm provider"]
    T4["Cellar inspection improvements"]
    T40["Go provider"]
    T41["Cargo provider"]
    T42["Independent hash verification…"]
    T44["Advanced trust model"]
    T45["Shim-free build toolchain"]
    T48["Bottle relocation at scale"]
    T5["Tap management"]
    T51["Safe automatic upgrades"]
    T52["Restart services after upgrade"]
    T57["Native parser refuses unresol…"]
    T25 -.->|needs| T24
    T30 -.->|needs| T29
    T34 -.->|needs| T33
    T38 -.->|needs| T23
    T39 -.->|needs| T23
    T40 -.->|needs| T23
    T41 -.->|needs| T23
    T52 -.->|needs| T33
```

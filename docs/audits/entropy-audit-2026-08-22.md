# Entropy audit — den

Date: 2026-08-22
Mode: full (entropy + hygiene)
Auditor: entropy-audit owner (this campaign)

## Executive summary

- **Snapshot:** `/Users/marcelo/work/github.com/marcelocantos/den`, branch `master`, commit `37007cb1488f7c8be13db488189f4bf390fd6ce7` (`37007cb Daemon install on macOS + auto_upgrade boolean config (#52)`).
- **Initial dirty state:** clean. `git status --porcelain=v1 -b` showed only `## master...origin/master`. No pre-existing staged or untracked user work.
- **Scope:** C++23 product (`src/`, `tests/*.cpp`, `CMakeLists.txt`, `Makefile`, `install.sh`, `.github/workflows/`, `docs/`, `bullseye.yaml`, `data/known_hashes.json`). Languages judged from manifests: C++ (CMake), Bash (`install.sh`, harness scripts), Python (`scripts/replica_verify.py`), Ruby (formula helpers). No `Cargo.toml`; gitignored `/target/` is leftover from the 2026-03-29 C++ rewrite.
- **Exclusions:** `vendor/` (spdlog submodule + header-only CLI11/doctest/nlohmann/picosha2), `tests/corpus/homebrew-core/` (Homebrew submodule), `build/` and `target/` (gitignored artefacts), generated rustdoc under `target/doc/`.
- **Headline mechanism:** the repo has a coherent per-concern C++ layout and a real provider seam, but several **load-bearing product facts have two owners**. Migration writes a different root-manifest path and schema than the CLI materialises; the two-source trust replica is documented and CMake-installed but omitted from the shipped tarball/installer; slug encoding and SHA-256 hex validation exist in production and in a private replica that already disagree.
- **Highest-consequence findings:** ENT-001 (migrate vs runtime manifest), ENT-002 (trust replica not on the shipped path), ENT-003 (`env_slug` oracle tests a fork), ENT-004 (`tap add` still shells a user URL).
- **Unverified residue:** ctest/`make bullseye` not re-run this session (working tree was already built); live `replica-verify` and macOS soak not executed; syntactic clone detector not installed.

## Scope and exclusions

In scope: `src/` (102 translation units, ~15.7k LOC), top-level `tests/test_*.cpp` (47 files, ~11.9k LOC), install/release/CI, architecture docs, bullseye.

Named exclusions (not silent omissions):

| Tree | Role |
|---|---|
| `vendor/github.com/gabime/spdlog` | git submodule |
| `vendor/include/{CLI11.hpp,doctest.h,nlohmann/json.hpp,picosha2.h}` | vendored headers |
| `tests/corpus/homebrew-core/` | Homebrew-core shallow submodule (oracle corpus) |
| `build/`, `target/` | local CMake/Cargo artefacts; `/target/` gitignored leftover of the Rust rewrite |
| `tests/harness/*/logs/` | gitignored local harness output |

`hygiene.yaml` is absent. Hygiene posture is undeclared (see Hygiene posture).

## Commands run

| Command | Version / notes | Exit | Shipped vs auxiliary | Limitations |
|---|---|---|---|---|
| `git rev-parse HEAD`; `git status --porcelain=v1 -b`; `git log -1` | git | 0 | provenance | — |
| `git ls-files`, `git log --name-only` (churn) | git | 0 | history | Rust-era paths still dominate all-time churn |
| `git describe --tags --abbrev=0` | `v0.13.0` (also `v1.0.0-rc.1`) | 0 | release | — |
| Python include/LOC scan over `src/` | python3 | 0 | auxiliary | First pass failed to resolve `"../mod/x.h"`; topology taken from targeted greps instead |
| `rg` over `src/`, `tests/`, `.github/`, `docs/` | ripgrep | 0 | source | Metrics locate evidence, not verdicts |
| `wc -l src/cli/*.cpp tests/test_*.cpp src/provider/*.cpp` | — | 0 | source | — |
| `clang-format --version` | Homebrew 22.1.8 | 0 | local tool | CI uses clang-format-18 |
| `cmake --version` | 4.3.4 | 0 | local tool | — |
| `/Users/marcelo/.claude/skills/hygiene/hygiene_check.py` | uv-run validator | 1 | hygiene | `FileNotFoundError: hygiene.yaml` — expected; not initialised |
| `ctest` / `make bullseye` | **not run** | — | shipped path | Residue: this audit did not re-execute the standing gate |

No analyzers were installed. jscpd/clang-tidy/include-what-you-use were not present as repo-declared tools.

## Observed architecture

### Entry points and deployable units

- **CLI binary** `den`: `src/main.cpp` (3 lines) → `den::Cli::run` in `src/cli/cli.cpp`.
- **Static library** `den_lib`: `file(GLOB_RECURSE DEN_SOURCES src/*.cpp)` in `CMakeLists.txt:31-40`, linked by `den` and `den_tests`.
- **Installer:** `install.sh` downloads `den-${version}-${os}-${arch}.tar.gz` from GitHub Releases and copies the binary to `~/.den/bin/den`.
- **Daemon:** `den daemon install` writes a LaunchAgent plist and `launchctl bootstrap`s it (macOS only).
- **Background replica job:** `.github/workflows/replica-verify.yml` (daily + path filter) rebuilds den, diffs `data/known_hashes.json`, commits on success.

### Declared vs observed modules

`CLAUDE.md` / `Claude.md` (same inode on this APFS volume; git tracks `CLAUDE.md`) list `src/` as C++23 per-concern modules compiled into one lib. Observed directories that the layout comment omits: `supervisor/`, `tap/`, `trust/`.

```
src/main.cpp          CLI shim
src/cli/              composition root (Cli pImpl) + install/outdated/shell/daemon_status slices
src/core/             Config (paths) + Error taxonomy
src/settings/         ~/.den/config.json (daemon/search/taps)
src/provider/         PackageProvider seam + Homebrew/pip/npm/go/cargo/stub
src/env/              manifests + materialise
src/store/            Cellar kegs + linking
src/index/            formula index + SAT solver
src/download/         HTTP, CAS cache, archive extract, SHA256
src/trust/            two-source hash cross-check
src/build/            native formula parser + source-build
src/ruby/             Portable Ruby + extract/build scripts
src/daemon/           background upgrades
src/supervisor/       built-in process supervisor (replaces launchctl for package services)
src/migrate/          Homebrew → den metadata import
src/tap/              third-party taps
src/doctor/           health + trust block
src/selfupdate/       GitHub-release binary replace
src/smoke/            in-process smoke including supervisor spy
src/platform/         OS/arch + host facts
src/activity/         upgrade activity log
```

### Dependency direction

Intended (and mostly held): `cli` → everyone; `provider` → `store`/`download`/`index`/`trust`; `env` → `store` + `PackageProvider`; `core`/`settings` as leaves.

Observed exceptions:

- `src/env/environment.cpp` includes `provider/package_provider.h` and `provider/registry.h` (materialiser asks providers for binary paths).
- `src/provider/registry.cpp` includes `env/manifest.h` (`resolve_provider` takes a `Manifest`).
- `src/provider/homebrew_provider.cpp` includes `env/environment.h` and `env/manifest.h` and calls `read_manifest` for `conflicts_with`.

That is an **env ↔ provider cycle**. The `PackageProvider` comment (`src/provider/package_provider.h:73-77`) says providers are not responsible for the den manifest; Homebrew still reads it.

`cli.cpp` is the high fan-out hub: it includes activity, build, config, daemon, doctor, env, index, migrate, platform, provider, selfupdate, settings, smoke, store, supervisor, tap, and trust (`cli.cpp:4-31`). 1640 lines. Highest C++-era churn (20 commits).

No header-level SCC was mechanically computed this session (include-resolver limitation). No ArchUnit-style gate exists.

### Public surfaces

- CLI (STABILITY.md catalogue; snapshot labelled v0.10.0).
- `~/.den/config.json`, `~/.den/manifests/<slug>/manifest.json`, shared Cellar at Homebrew prefix.
- `PackageProvider` virtual interface.
- Trust replica JSON (`data/known_hashes.json`) and hidden `den replica-verify`.
- `install.sh` + GitHub Release tarballs.

### Declared rules vs observed

| Rule | Status |
|---|---|
| Per-concern modules, one static lib | **agrees** (enforced only by directory convention + CMake glob) |
| Provider seam; CLI owns manifest/materialise | **contradicted** by HomebrewProvider manifest reads and env↔provider cycle |
| Two-source trust on every install | **declared**; **degraded** on the shipped path (ENT-002) |
| Native parser is a SIMPLE fast path; Ruby is permanent | **agrees** (T18, source_build complexity gate) |
| Fable F1–F5 fail-closed | **agrees** in current source + `tests/test_security_fable.cpp` |
| Format is a standing invariant (`make bullseye`) | **Makefile agrees, CI format job does not fail** (ENT-006) |
| Unified package model, no `--cask` | README/STABILITY agree; T12 context still says `den install --cask` |

## Dimension vector

| Dimension | State | Evidence summary | Change from baseline |
|---|---|---|---|
| Architecture topology | concern | Clear modules and provider seam; cli hub + env↔provider cycle; CMake glob erases module edges | first full entropy report |
| Redundancy / sources of truth | critical | Three slug encodings; two root-manifest layouts/schemas; two native formula extractors; SHA-256 hex duplicated and drifted | first full entropy report |
| Change amplification | concern | cli.cpp is the C++ churn hub; popen wrappers copied in six files; next command still edits the dispatcher | first full entropy report |
| Local code quality | concern | pImpl on Cli; Fable fixes are linear and commented; leftover `parse_formula_source` and shell `run()` sit beside `run_tool` | first full entropy report |
| Correctness / verification | concern | Strong unit/oracle/fable tests; migrate and slug oracles pin the *wrong* contract; macOS soak not in CI; T59 cron disabled | first full entropy report |
| Security / dependencies | concern | Fable F1–F5 fixed and regression-tested; `tap add` still popens a user URL; replica not shipped; install.sh can skip checksums | first full entropy report |
| Build / release / operations | concern | CMake+Ninja+ctest on macOS/Linux; RC/promote workflows exist; release tarball is binary-only vs CMake `share/den` | first full entropy report |
| Documentation / governance | concern | README/agents-guide mostly current; CONTRIBUTING.md still Rust; bullseye `acceptance: TODO` on many achieved targets; no hygiene.yaml | first full entropy report |

Do not aggregate these states into a scalar.

## Findings

### ENT-001: `den migrate` writes a different root manifest than the CLI reads

- **Priority:** P1
- **Dimensions:** Redundancy / sources of truth; Correctness / verification; Architecture topology
- **Status:** observed fact
- **Evidence:**
  - Runtime path: `manifest_file` = `den_home / "manifests" / env_slug(env_path) / "manifest.json"` (`src/env/manifest.cpp:50-51`). For `/` that is `manifests/ROOT/manifest.json`.
  - Runtime schema: nested `packages[provider][name]=version` and `auto_deps[provider]` (`src/env/manifest.h:18-28`; writer at `manifest.cpp:176-184`).
  - Migrate path: `manifest_dir / "ROOT.json"` (`src/migrate/migrate.cpp:471-472`, write at `590-605`, user-facing rollback hint at `632-633`).
  - Migrate schema: flat `packages`, `auto` array, plus `casks` / `taps` / `services` (`migrate.cpp:48-76`, `475-479`).
  - `den list` (default) uses `resolve_per_provider` → `read_manifest` (`src/cli/cli.cpp:453-474`) and prints `No packages installed.` when that file is empty.
  - Tests lock the migrate contract to `ROOT.json`: `tests/test_migration_fixture.cpp:112-114`, `tests/test_migration_non_destructive.cpp:8-9,186`.
  - Installer and command table advertise migrate: `install.sh:121-122`, `README.md:167`.
- **Mechanism:** two files and two schemas claim to be “the root manifest”. Migrate can succeed and still leave the materialised environment empty. Extra migrate keys (`casks`, `taps`, `services`) have no runtime `Manifest` field, so even a path fix would drop them on the next `write_manifest`.
- **Blast radius:** every Homebrew onboarding (`den migrate`); `den list` / `den env show` / install conflict checks; soak `step_migrate_subset` (`tests/harness/macos/soak.sh:141-154`) asserts migrate rc==0, not that `den list` afterwards is non-empty.
- **Counterevidence checked:** `read_manifest` *does* promote a legacy flat `packages` object under `homebrew` (`manifest.cpp:131-134`) — so schema bridging is partial **if** the file were in the right place. Grep shows no `ROOT.json` reader outside `src/migrate/` and its tests. Shared Cellar means bottles already exist on disk; migrate is metadata-only (`migrate.h:113-122`).
- **Smallest coherent remediation:** make migrate call `with_manifest(den_home, "/", …)` (nested homebrew bucket + `auto_deps`), or have `read_manifest` fall back to `ROOT.json` once and rewrite. Keep casks/taps/services in settings/supervisor stores, not a parallel JSON document.
- **Verification:** fixture: migrate a synthetic Cellar, then `read_manifest(den_home, "/")` contains those formulae; `den list` is non-empty. Fail if `ROOT.json` is the only file written.
- **Ratchet candidate:** ctest assertion that migrate output path equals `manifests/ROOT/manifest.json`; hygiene `command:` later if adopted.

### ENT-002: Two-source trust replica is not on the shipped path

- **Priority:** P1
- **Dimensions:** Security / dependencies; Build / release / operations; Redundancy / sources of truth
- **Status:** observed fact (packaging + lookup); inference (typical `install.sh` user always WarnProceeds)
- **Evidence:**
  - Docs: replica “shipped with den (installed to `share/den/known_hashes.json`)” (`docs/trust-model.md:64-66`); CMake `install(FILES … DESTINATION share/den)` (`CMakeLists.txt:93-96`).
  - Release job packages only the binary: `tar czf "${archive}" -C build den` (`.github/workflows/release.yml:56-61`).
  - `install.sh` copies only `den` into `~/.den/bin/` (`install.sh:87-99`). No `share/den`, no `known_hashes.json`.
  - Lookup candidates (`src/trust/trust_model.cpp:110-130`): `den_home/trust/known_hashes.json`, **cwd-relative** `../share/den/known_hashes.json`, `/usr/local/share/den/known_hashes.json`, **cwd-relative** `data/known_hashes.json`.
  - Absent replica → empty map (`trust_model.cpp:136-138`); one source missing → `WarnProceed` (`trust_model.h:22`, `homebrew_provider.cpp:82-88`).
  - Snapshot contains **12** hashes for `tree` and `jq` only (`data/known_hashes.json:6-19`).
  - Same cwd-relative candidate list for `den_build.rb` (`src/build/source_build.cpp:271-276`), also not in the release tarball.
- **Mechanism:** the independent second hash source exists in git and in `cmake --install` layout, not in the artefact `install.sh` and `den self-update` actually install. The hot path never fetches the live CDN (`homebrew_provider.cpp:65-68`). Almost every bottle therefore installs with degraded trust even when the binary is “correctly” installed.
- **Blast radius:** all `den install` bottle pours for users of GitHub Releases; T42/T64 product claim; `den doctor` trust block.
- **Counterevidence checked:** tests cover WarnProceed (`tests/test_trust_model.cpp:64-73,122-130`) — they encode degradation as expected, not as a packaging bug. `replica-verify.yml` keeps the git snapshot honest. User override at `~/.den/trust/known_hashes.json` works if someone copies the file by hand.
- **Smallest coherent remediation:** put `known_hashes.json` (and `den_build.rb` if source-build is shipped) into the release archive at a path `local_replica_path` resolves **from the executable**, not cwd. Fail the release job if the replica is missing from the tarball.
- **Verification:** unpack the published archive; run `den` from `$HOME` with empty `~/.den/trust`; `local_replica_path` must find the bundled file. Install of `tree` must `Proceed` when the replica has that bottle.
- **Ratchet candidate:** release workflow step `tar tzf` asserting `share/den/known_hashes.json`; unit test that lookup is exe-relative.

### ENT-003: `env_slug` tests and comments describe a different encoding than production

- **Priority:** P1
- **Dimensions:** Redundancy / sources of truth; Correctness / verification
- **Status:** observed fact
- **Evidence:**
  - Production (`src/env/manifest.cpp:56-86`): leading `/` stripped; `/` → `%2F`; `-` → `%2D`; `.` → `%2E`; `%` → `%25`. So `env_slug("/work/legacy")` is `work%2Flegacy`. Fable comment at `:67-69`.
  - Public header documents a third mapping: `"/work/legacy" -> "work%2Dlegacy"` (`src/env/manifest.h:34-36`) — `%2D` is hyphen, not slash.
  - `tests/test_manifest.cpp` does **not** include `env/manifest.h`. It copies `env_slug` locally (`:68-98`) joining components with `--` and **not** encoding `.`. Assertions: `env_slug("/work/legacy") == "work--legacy"` (`:139-141`), `env_slug("/a/b/c") == "a--b--c"` (`:151-153`). File header (`:6-13`) describes that replica.
  - Production is covered for `..` only in `tests/test_security_fable.cpp:144-153` (`env_slug("..") == "%2E%2E"`).
- **Mechanism:** the suite named `manifest::env_slug` cannot fail when production encoding changes. A future slug bug in `manifest.cpp` is invisible to that suite. Comments/docs already drifted from both copies.
- **Blast radius:** env create/use/remove paths, shell `DEN_ENV`, any tool that inspects `~/.den/manifests/`.
- **Counterevidence checked:** `tests/test_manifest_schema.cpp:55-57` uses the real `env_slug` for file placement but does not assert encoding of `/` or `.`. Fable F3 regression exists for traversal, not for `/work/legacy`.
- **Smallest coherent remediation:** delete the local replica; `#include "env/manifest.h"` and assert production strings (`work%2Flegacy`, `%2E` for dots). Fix the header example.
- **Verification:** `CHECK(den::env_slug("/work/legacy") == "work%2Flegacy")` in `den_tests`. Must fail if someone reintroduces `--`.
- **Ratchet candidate:** test file must include `env/manifest.h`; grep gate that `tests/test_manifest.cpp` contains no `static std::string env_slug`.

### ENT-004: `den tap add --url` still concatenates into `popen`

- **Priority:** P1
- **Dimensions:** Security / dependencies; Local code quality
- **Status:** observed fact
- **Evidence:**
  - `tap_add` for remotes: `run_capture("git clone --depth 1 " + source + " " + dest.string())` (`src/tap/tap.cpp:178-179`).
  - `run_capture` is `popen((cmd + " 2>&1").c_str(), "r")` (`tap.cpp:50-54`).
  - `source` is CLI `--url` or positional (`src/cli/cli.cpp:1110-1116`); default is `"https://github.com/" + name + ".git"` after `is_valid_tap_name` (`tap.cpp:152-154`, `163-168`).
  - `is_remote_source` only checks scheme prefixes (`tap.cpp:65-72`). A URL such as `https://example.com/x;touch /tmp/pwned` still matches `https://`.
  - `run_tool` already exists as the argv/`posix_spawn` owner (`src/provider/exec.h:20-25`, `exec.cpp`).
  - Fable F1 fixed the analogous `brew info` concatenation (`source_build.cpp:193-195`, `209-214`; `tests/test_security_fable.cpp:53-77`).
- **Mechanism:** the next shell-injection class after F1 is the same wrapper, still used for untrusted URL text. Local-path taps use `fs::copy` (safe); the git path is not.
- **Blast radius:** `den tap add user/repo --url …`. User must pass the URL, but the extra capability vs `git clone` is `/bin/sh -c` metacharacters.
- **Counterevidence checked:** tap *name* is traversal-checked (`tap.cpp:27-47`). Tests use `--url` with a local fixture path (`tests/test_tap_management.cpp:115-130`), so they never exercise `git clone` popen. `run_tool` is used by providers and fable tests.
- **Smallest coherent remediation:** `run_tool({"git", "clone", "--depth", "1", source, dest.string()})`; refuse source bytes outside a URL/path allow-list.
- **Verification:** `tap_add` with `source = "https://example.com/x; touch SENTINEL"` must not create SENTINEL and must not invoke a shell. Prefer spawn-fail or git error.
- **Ratchet candidate:** extend `test_security_fable.cpp`; grep that `src/tap/tap.cpp` does not contain `popen`.

### ENT-005: CLI composition root plus env↔provider cycle amplify every command change

- **Priority:** P2
- **Dimensions:** Architecture topology; Change amplification
- **Status:** observed fact
- **Evidence:** `src/cli/cli.cpp` is 1640 lines, includes 16 product modules (`:4-31`), and still inlines install-from-source, list, info, env, tap, daemon launchctl, services, self-update. Partial extractions exist (`cli/install.cpp` 148 lines, `shell.cpp`, `outdated.cpp`, `daemon_status.cpp`). Git log: 20 commits on `cli.cpp` (highest among remaining C++ files). `homebrew_provider.cpp:185-187` reads the env manifest despite `package_provider.h:73-77`. `environment.cpp:8-9` includes the provider registry; `registry.cpp:7` includes `manifest.h`.
- **Mechanism:** adding a flag or command requires editing the hub; Homebrew conflict checks cannot move without taking env with them. CMake glob (`CMakeLists.txt:31`) plus a single `den_lib` hide the cycle at link time.
- **Blast radius:** every new CLI verb; provider authors who must know env layout; compile times for a one-line CLI change.
- **Counterevidence checked:** `Cli` is pImpl (`cli.h:19-21`) matching `cpp.md`. Extracting `install.cpp` shows the intended direction. One-lib layout is an explicit product choice (`CLAUDE.md` Source Layout).
- **Smallest coherent remediation:** keep one binary; move remaining callbacks behind existing `cli/*.h` files; pass a `ManifestView` into Homebrew so `homebrew_provider.cpp` does not include `env/`.
- **Verification:** include graph test: `src/provider/**` must not include `env/`. cli.cpp line-count ratchet once a baseline is frozen.
- **Ratchet candidate:** clang-tidy / custom include check; or a Python graph in `make bullseye`.

### ENT-006: Format is a failing Makefile gate and a non-failing CI warning

- **Priority:** P2
- **Dimensions:** Correctness / verification; Build / release / operations
- **Status:** observed fact
- **Evidence:**
  - `make format` uses `clang-format --dry-run -Werror` on `src/**` and `tests/*.{h,cpp}` maxdepth 1 (`Makefile:25-34`) and **exits 1** on drift.
  - CI `format` job runs `find src tests -name '*.h' -o -name '*.cpp' | xargs clang-format-18 -i` then `git diff` as `::warning::` with **no `exit 1`** (`.github/workflows/ci.yml:64-72`). Recursive `tests/` includes the homebrew-core submodule if it ever grows `.c`/`.cpp` (Patches already has `.c`).
  - Local clang-format is 22.1.8; CI pins 18.
- **Mechanism:** two oracles for one property. PR CI can be green with format drift that `make bullseye` rejects. Inverse: CI may warn on submodule files Makefile never checks.
- **Blast radius:** every C++ change; `/cv` standing invariants vs GitHub required checks.
- **Counterevidence checked:** `make bullseye` is what `/cv` runs (`Makefile:11`). Format job exists, so the gap is enforcement, not absence.
- **Smallest coherent remediation:** make the CI format job `exit 1` on diff; reuse Makefile’s `find` (tests maxdepth 1). Pin a single clang-format version in both places.
- **Verification:** a misformatted `src/core/types.h` must fail the GitHub `format` job, not only emit a warning.
- **Ratchet candidate:** CI step failure; later `hygiene.yaml` `ci_job: ci.yml#format` with blocking intent.

### ENT-007: Six `popen` wrappers remain beside `run_tool`

- **Priority:** P2
- **Dimensions:** Change amplification; Security / dependencies
- **Status:** observed fact
- **Evidence:** near-identical capture helpers: `source_build.cpp:31-40` (`run`), `migrate.cpp:34-46`, `tap.cpp:50-61`, `platform.cpp:36-59`, `smoke/runner.cpp:33-35`, `smoke/supervisor.cpp:34-36`. `source_build.cpp:60-68` still `std::system`s a quoted shell line for the actual build. `cli.cpp:1217-1224,1241` `std::system`s `launchctl`. `selfupdate.cpp:207` `std::system("tar xzf " + …)`.
- **Mechanism:** F1 fixed one call site; the wrapper type survived. The next interpolation bug has five extra homes. Migrate’s only `run_capture` argument is the constant `"brew services list --json"` (`migrate.cpp:415`) — duplication without current injection.
- **Blast radius:** source-build, tap, daemon install, self-update extract, host-fact probes.
- **Counterevidence checked:** `run_tool` documents “never a shell” (`exec.h:22-24`). `shell_quote` exists next to `run()` in `source_build.cpp:43-57`. Daemon launchctl is macOS service install, not package-name input.
- **Smallest coherent remediation:** delete `run`/`run_capture` where argv suffices; keep a single quoted `std::system` only for formula build recipes that are inherently shell (and only after `is_valid_package_name`).
- **Verification:** `rg popen src/` shrinks to smoke spies and documented exceptions; tap/selfupdate use `run_tool`/`libarchive`.
- **Ratchet candidate:** `rg -n popen src --glob '!smoke/**'` as a bullseye/make check.

### ENT-008: Two native formula extractors can disagree on URL/SHA256

- **Priority:** P2
- **Dimensions:** Redundancy / sources of truth; Correctness / verification
- **Status:** observed fact
- **Evidence:** `parse_formula_source` (`source_build.cpp:86-107`) regex-extracts `url`/`sha256` and a coarse build-system guess; `extract_build_recipe` returns it (`:187-207`). Later the same function calls `parse_formula` (`source_build.cpp:366`, `formula_parser.cpp:345,359-361`) which has complexity classification and is what T18’s oracle tests. The SHA regex is copy-pasted (`source_build.cpp:94` vs `formula_parser.cpp:359`).
- **Mechanism:** download/hash of the tarball can follow the simple parser (`:296-320`) while the build commands follow the full parser (`:362-378`). A formula where they diverge installs the wrong bytes or a truncated recipe.
- **Blast radius:** `den install -s`; T18/T59 SIMPLE path.
- **Counterevidence checked:** T18 oracle (`tests/test_oracle.cpp`) exercises `parse_formula`, not `parse_formula_source`. Complexity gate refuses non-Simple (`source_build.cpp:368-378`). Ruby remains the primary path when bundled ruby succeeds (`source_build.cpp:250-261`).
- **Smallest coherent remediation:** delete `parse_formula_source`; `extract_build_recipe` should call `parse_formula` (or refuse).
- **Verification:** unit test that a fixture with two `sha256` forms cannot yield different hashes on the two paths because only one path exists.
- **Ratchet candidate:** `rg parse_formula_source src/` empty.

### ENT-009: `is_sha256_hex` is duplicated and already drifted

- **Priority:** P2
- **Dimensions:** Redundancy / sources of truth; Security / dependencies
- **Status:** observed fact
- **Evidence:** `trust_model.cpp:26-40` requires 64 chars in `[0-9a-f]` (lowercase only). `selfupdate.cpp:149-159` uses `std::isxdigit` (accepts `A-F`). Both sit on trust boundaries (replica parse vs self-update checksum).
- **Mechanism:** an uppercase checksum asset is valid for self-update and invalid for the replica. A later “fix” to one copy will not update the other.
- **Blast radius:** `den self-update`, replica parse, any new hash gate.
- **Counterevidence checked:** GitHub `shasum -a 256` output is lowercase; current release assets likely match both. `download/sha256.cpp` hashes via picosha2 (single implementation of the digest itself).
- **Smallest coherent remediation:** one `den::is_sha256_hex` in `download/sha256.h`.
- **Verification:** uppercase 64-hex is accepted or rejected **identically** in trust and selfupdate tests.
- **Ratchet candidate:** single definition; `rg 'bool is_sha256_hex' src/` count == 1.

### ENT-010: Contributor and module docs still describe a previous product

- **Priority:** P2
- **Dimensions:** Documentation / governance
- **Status:** observed fact
- **Evidence:** `CONTRIBUTING.md:8-24` says `cargo build`, `cargo test`, `cargo fmt`, `cargo clippy`, Rust edition 2024. Product is CMake/Ninja/ctest (`CLAUDE.md:93-102`, `CMakeLists.txt:1-5`). `STABILITY.md:13` “Snapshot as of v0.10.0” while `project(den VERSION 0.13.0)` and tags include `v0.13.0` / `v1.0.0-rc.1`. `CLAUDE.md` source tree omits `supervisor/`, `tap/`, `trust/`. T12 context still documents `den install --cask` (`bullseye.yaml:61`) vs README `no --cask flag` (`README.md:55`, `STABILITY.md:19`).
- **Mechanism:** a new contributor following CONTRIBUTING.md cannot build. Agents following T12 reintroduce a flag the unified model removed.
- **Blast radius:** onboarding, agent sessions, cask UX.
- **Counterevidence checked:** `agents-guide.md` and README are C++-accurate. `CLAUDE.md`/`Claude.md` are one inode (case-insensitive FS), not two documents.
- **Smallest coherent remediation:** rewrite CONTRIBUTING.md to cmake/ninja/ctest/clang-format; bump STABILITY snapshot; add the three modules to the layout; edit T12 context (achieved target text).
- **Verification:** CONTRIBUTING.md contains `cmake --build` and does not contain `cargo`.
- **Ratchet candidate:** `file:` hygiene item once hygiene.yaml exists.

### ENT-011: Source-build smoke still fail-opens index fetch after T11 is achieved

- **Priority:** P2
- **Dimensions:** Correctness / verification; Build / release / operations
- **Status:** observed fact
- **Evidence:** T11 is `achieved` (`bullseye.yaml` T11). Workflow header still says “All jobs use continue-on-error until the source-build path (🎯T11) is live” (`.github/workflows/source-build-smoke.yml:6-8`). `./build/den update || true` plus `continue-on-error: true` (`:127-132`, `:216-220`). Binary post-check `continue-on-error: true` (`:158-159`, `:243-244`). The `install --build-from-source` step itself now gates (`:134-142`).
- **Mechanism:** a missing index makes the gated install fail for the wrong reason, or the `|| true` hides update regressions. Header comment trains the next editor to keep continue-on-error.
- **Blast radius:** daily T65 job; confidence that SIMPLE source-build works on a runner.
- **Counterevidence checked:** build+unit-test job in that workflow no longer continue-on-error (`:41`). T59 simple-install cron is still dispatch-only with T58 caveats (`simple-install-cron.yml:39-42`) — that one is honestly still residual.
- **Smallest coherent remediation:** drop `|| true` and continue-on-error on `den update` and the binary check; fix the header.
- **Verification:** a broken `den update` fails the smoke workflow.
- **Ratchet candidate:** no `continue-on-error` on those two steps.

### ENT-012: CMake `GLOB_RECURSE` is the module boundary

- **Priority:** P3
- **Dimensions:** Architecture topology; Build / release / operations
- **Status:** observed fact
- **Evidence:** `CMakeLists.txt:31` `file(GLOB_RECURSE DEN_SOURCES src/*.cpp)`; tests use non-recursive `file(GLOB TEST_SOURCES tests/*.cpp)` (`:77`) with an explicit comment about the homebrew-core submodule.
- **Mechanism:** adding a `.cpp` works only after reconfigure; removing one can leave a stale `.o` until clean; no CMake list encodes “cli may depend on env”.
- **Blast radius:** new modules, stale objects, ENT-005 graph.
- **Counterevidence checked:** the glob is deliberate for a single-lib layout. Tests glob is correctly non-recursive.
- **Smallest coherent remediation:** keep the glob **or** list module libraries; if listing, do it when splitting `den_lib`, not as busywork.
- **Verification:** N/A until a split is chosen.
- **Ratchet candidate:** none until the architecture choice is explicit.

### ENT-013: Bullseye acceptance TODOs and an empty `docs/TODO.md`

- **Priority:** P3
- **Dimensions:** Documentation / governance
- **Status:** observed fact
- **Evidence:** `bullseye.yaml` contains 88 `acceptance:` blocks; many achieved targets still have `- TODO` (e.g. T1, T10, T12). T18 is `achieved` while acceptance still lists “REMAINING — dependency extraction” and “installable-for-real” (`bullseye.yaml` T18). `docs/TODO.md` is `# TODO\n\n(none yet)` and `CLAUDE.md` still points at it. Global agent rules ban TODO files as the work ledger.
- **Mechanism:** `/cv` and humans cannot tell which achieved targets have executable acceptance. T58/T59 exist for the T18 remainder — the leftover bullets on T18 compete with those targets.
- **Blast radius:** planning noise, not runtime.
- **Counterevidence checked:** T18/T33/T61 later targets have real acceptance bullets. Frontier is computable (`bullseye_frontier` returned 15 ready targets).
- **Smallest coherent remediation:** strip `TODO` acceptance from achieved targets or replace with the checks that actually landed; delete `docs/TODO.md`; drop the CLAUDE pointer.
- **Verification:** `rg 'acceptance:\n    - TODO' bullseye.yaml` empty for `status: achieved`.
- **Ratchet candidate:** bullseye validate / custom grep in `make bullseye`.

### ENT-014: `install.sh` version and checksum edges

- **Priority:** P3
- **Dimensions:** Security / dependencies; Build / release / operations
- **Status:** observed fact
- **Evidence:** `DEN_VERSION` accepts `vN.N.N` (`install.sh:52-57`) then interpolates into `den-${version}-…tar.gz` (`:75`) while release.yml strips the `v` (`release.yml:58`). `latest_version` strips via `sed 's|.*/v||'` (`install.sh:132`). Missing `shasum`/`sha256sum` prints a warning and **returns success** (`install.sh:162-168`).
- **Mechanism:** `DEN_VERSION=v0.13.0` downloads a name that does not exist. A stripped environment without a sha tool installs an unverified tarball (self-update was fail-closed for the same class — `selfupdate.cpp:167-172`).
- **Blast radius:** installer users pinning `v…`; unusual OS images without sha tools (macOS has `shasum`).
- **Counterevidence checked:** default latest-version path strips `v`. Checksum file is required to contain the archive name (`:157-160`) when a hash tool exists.
- **Smallest coherent remediation:** strip a leading `v` after validation; `err` if no hash tool.
- **Verification:** `DEN_VERSION=v0.13.0` installs; a mock without shasum/sha256sum exits non-zero before extract.
- **Ratchet candidate:** installer test under bash 3.2 (see `bash.md`).

## Redundancy and competing-source-of-truth inventory

| Fact | Owners | Drift |
|---|---|---|
| Root manifest path + schema | `env/manifest.cpp` vs `migrate/migrate.cpp` + migrate tests | **yes** (ENT-001) |
| Env slug encoding | `manifest.cpp` vs `test_manifest.cpp` replica vs `manifest.h` comment | **yes** (ENT-003) |
| Bottle SHA256 (second source) | git `data/known_hashes.json` vs CMake `share/den` vs release tarball vs cwd lookup | **yes** (ENT-002) |
| SHA-256 hex validator | `trust_model.cpp` vs `selfupdate.cpp` | **yes**, lowercase vs `isxdigit` (ENT-009) |
| Native formula URL/SHA | `parse_formula` vs `parse_formula_source` | **latent** (ENT-008) |
| Formula evaluation | Ruby (`den_build.rb` / Portable Ruby) vs C++ SIMPLE parser | **deliberate** (T18); Ruby permanent |
| Shell-out | `run_tool` vs six `popen` helpers | **yes** (ENT-007, ENT-004) |
| Format oracle | `make format` vs CI warning | **yes** (ENT-006) |
| How to build | CONTRIBUTING.md cargo vs CMake | **yes** (ENT-010) |
| Cask UX | README unified vs T12 `--cask` | **yes** (ENT-010) |
| Config vs settings | `core/config.cpp` (paths) vs `settings/` (user prefs) | **no** — clean split |
| CLI help vs agents-guide | `cli.cpp` pointer vs `agents-guide.md` | **no** material drift found |
| CLAUDE.md vs Claude.md | one inode | **not** two files |

Deliberate duplication retained: per-ecosystem name validators in pip/cargo/go/node providers; Homebrew Cellar as shared store with Homebrew; native parser + Ruby.

## Healthy structure worth retaining

- **Provider seam.** `PackageProvider` + `ProviderRegistry` + nested manifest buckets is a real boundary. Stub provider + `tests/test_multi_provider_e2e.cpp` / `tests/test_provider_registry.cpp` keep it honest. Auxiliary providers `accepts()==false` so Homebrew remains the default (`registry.cpp:59-61`).
- **Fable F1–F5 remediations held.** Current `source_build.cpp:193-214`, `archive.cpp:63-107`, `manifest.cpp:67-79,160-166`, `selfupdate.cpp:167-184`, plus `tests/test_security_fable.cpp`. Do not regress these to “audit said so” comments without the tests.
- **T18 oracle.** `tests/test_oracle.cpp` + golden JSON + homebrew-core submodule is a shipped-path classification gate in ctest.
- **Atomic manifest writes + flock.** `write_manifest` temp+rename; `with_manifest` in `manifest_impl.h`.
- **Cli pImpl** (`cli.h`) matches the C++ companion.
- **Release candidate discipline.** `release-candidate.yml` + `promote-rc.yml` copy bits, require macOS harness attestation, refuse PATCH versions.
- **Linux harness in CI** (`.github/workflows/harness-linux.yml`) vs documented macOS-soak-not-in-CI (`docs/release-candidate.md:17-21`, `tests/harness/macos/README.md`).
- **Trust replica CI** (when the file is actually used): diff vs GHCR, fail-closed issue, no commit on mismatch (`replica-verify.yml`).
- **Config vs Settings** split: detect paths from the environment; persist user policy separately.

## Hygiene posture

`hygiene.yaml` is **absent**. Validator invocation from repo root:

```
/Users/marcelo/.claude/skills/hygiene/hygiene_check.py
```

Exit 1, traceback: `FileNotFoundError: …/den/hygiene.yaml`. Per the brief, posture is **not declared**. This audit did not initialise a file.

Observed controls that a future `hygiene.yaml` could point at (not declared, not ratcheted here):

- LICENSE Apache-2.0, README, `.gitignore`
- `make bullseye` (configure, build, ctest, format, clean-tree)
- CI `check` matrix macos-14 + ubuntu-latest; Linux harness; replica-verify; RC/release
- No CODEOWNERS, no Dependabot, no secret scanner config found
- `SECURITY.md` present (email the maintainer)
- Format CI does not block (ENT-006)

Overlap: ENT-006/011 are hygiene-shaped (declared gate vs reality). ENT-001–004 are structural and would not be decided by a file-existence check alone.

## Oracle coverage and residue

| Property | Decided by | Notes |
|---|---|---|
| C++ unit behaviour | `den_tests` via ctest (shipped path) | not re-run this session |
| SIMPLE formula parse vs Ruby golden | `tests/test_oracle.cpp` + corpus submodule | T18 remaining deps/installable tracked as T58/T59 |
| Fable F1–F5 | `tests/test_security_fable.cpp` | held |
| Multi-provider install/list | in-process tests + stub provider | |
| Linux owner-visible smoke | `tests/harness/linux/run.sh` in CI | journey-shaped |
| macOS owner-visible smoke/soak | `make harness-macos` / `soak-macos` | **manual**; promote-rc attestation |
| SIMPLE install-without-Ruby | `simple-install-cron.yml` | dispatch only until T58 |
| Replica vs GHCR | `replica-verify.yml` (CI network) | not run locally (workflow says so) |
| Format | Makefile fails; CI warns | ENT-006 |
| Migrate → runtime env | **nothing** | ENT-001 |
| Slug encoding of `/` | **wrong oracle** | ENT-003 |
| Trust replica present after `install.sh` | **nothing** | ENT-002 |
| `tap add` URL not a shell | **nothing** | ENT-004 |
| Standing invariants | `make bullseye` | not executed this session |

**Owner residue (intent, not mechanical leftover):**

1. Should migrate remain an inventory document (`ROOT.json` + casks/services) with a separate import step, or is the README contract “migrate then `den list` works”?
2. Is a 12-entry replica an accepted seed (WarnProceed for everything else) or a bug relative to T42?
3. Keep one `den_lib` glob forever, or split libraries to make ENT-005 enforceable?
4. Accept macOS soak as manual forever (current RC process) or require a cloud macOS runner?

Failed/skipped checks this session: hygiene.yaml missing; ctest not re-run; jscpd not available; include-graph script incomplete.

## Remediation sequence

1. **Repair oracles that pin the wrong contract:** ENT-003 (test production `env_slug`); ENT-001 (migrate writes `read_manifest`’s file, test `den list` after migrate).
2. **Put trust on the shipped path:** ENT-002 (replica in the tarball, exe-relative lookup). Fail release if missing.
3. **Close the remaining shell sink:** ENT-004 (`run_tool` for `git clone`); then ENT-007 (delete spare `popen`s).
4. **Converge duplicate validators/parsers:** ENT-009 then ENT-008.
5. **Make CI match `make bullseye`:** ENT-006 (format fails the job); ENT-011 (stop `den update || true`).
6. **Docs/governance:** ENT-010, ENT-013, ENT-014. Initialise `hygiene.yaml` only if the owner asks.
7. **Architecture:** ENT-005/012 only after the oracles above exist, so a split cannot hide a red test.
8. Re-run this audit against the same finding IDs and the same snapshot fields.

No code was changed in this pass. No `hygiene.yaml` was created. No push.

## Comparison appendix

First full entropy report for this repository on these definitions. Prior artefacts (`docs/audit-2026-03-25.md`, `docs/audit-2026-03-27.md`, `docs/audit-log.md`) cover the **Rust** product and are not a like-for-like baseline. `docs/audit/fable-2026-07.md` findings F1–F5 were checked against current C++ and are **closed in source**; they are healthy residue, not open ENT items.

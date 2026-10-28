# Release Candidate Process

den ships via release candidates. A GA release is a promoted RC — the same
bits the harness validated are what ships.

## When to cut an RC

Any time a release is ready. The RC **is** the release; promotion is a
one-step flip. Never push a GA tag directly.

## What CI verifies

- **Build**: all three platforms (macOS arm64, Linux x86_64, Linux arm64)
- **Unit tests**: all three platforms (`ctest`)
- **Linux harness**: Docker-based end-to-end smoke of install / env / uninstall flows

## What CI does NOT verify

- **macOS harness**: the test laptop is not a GitHub runner. It must be run
  manually (see below).
- Real-world soak or upgrade paths from older versions.

## How to cut an RC

```sh
# Auto-bump N (recommended):
gh workflow run release-candidate.yml -f version=0.12.0

# Retry a specific N after fixing a bug:
gh workflow run release-candidate.yml -f version=0.12.0 -f rc_number=2
```

`version` is the GA version this RC targets: `X.Y.0`, no `v` prefix, no
`-rc.N` suffix. Patch versions (`X.Y.Z` where `Z > 0`) are rejected — all
releases are minor releases.

The workflow tags `v0.12.0-rc.1` (or the next free N), builds all platforms,
runs the Linux harness, and publishes a GitHub prerelease with the binaries,
Homebrew bottles and an RC `den.rb`, checksums, and harness logs attached.

Numbering: `rc.1` for the first attempt at a given GA version; each retry
auto-bumps to the next available N.

## Manual steps before promotion

After the RC workflow succeeds, run the macOS harness against `den-test-mac`:

```sh
make harness-macos
```

This exercises the same smoke suite on a real macOS host over SSH. See
`tests/harness/macos/README.md` for one-time host setup (SSH alias, Remote
Login, key-based auth).

**Expected output**: `RESULT: PASS` with zero FAIL lines.

### If the macOS harness fails

1. Do not promote.
2. Diagnose and fix the bug.
3. Cut a new RC — `rc_number` auto-bumps:
   ```sh
   gh workflow run release-candidate.yml -f version=0.12.0
   ```
4. Optionally delete the failed RC to keep the list clean:
   ```sh
   gh release delete v0.12.0-rc.1 --cleanup-tag
   ```

## How to promote to GA

Once the macOS harness passes:

```sh
gh workflow run promote-rc.yml \
  -f rc_tag=v0.12.0-rc.3 \
  -f confirm_macos_harness_passed=true
```

Promotion is **no-rebuild** — the exact binaries from the RC release are
copied to the GA release. The commit sha, checksums, and harness logs are
identical to what was validated.

The workflow:
1. Validates the RC tag format and confirms it is still a prerelease.
2. Enforces the macOS harness attestation (fails fast if false), and
   checks the RC's binary version and Homebrew bottles.
3. Creates the GA tag (`v0.12.0`) on the same commit as the RC tag.
4. Creates the GA GitHub release with the same assets.
5. Uploads the Homebrew-named aliases of the tarballs
   (`den-X.Y.0-darwin-arm64` / `linux-amd64` / `linux-arm64.tar.gz`) that
   the tap formula's URLs use.
6. Leaves the RC release in place as a historical record.

It does **not** touch the tap: there is no tap token in Actions.

## Publish the tap formula

After `promote-rc.yml` succeeds, publish `marcelocantos/tap/den` from the
dev Mac with [tapper](https://github.com/marcelocantos/tapper):

```sh
make release-tap RELEASE_TAG=v0.12.0 RELEASE_TAP_FLAGS=--dry-run  # preview
make release-tap RELEASE_TAG=v0.12.0                              # publish
```

`scripts/release-tap.sh` downloads the GA release's assets, checks the
aliases are present and every asset matches the release checksums,
re-verifies the bottles, renders a per-release tapper config (the
committed `tapper.yaml` plus this release's `bottle do` block, via
`homebrew-bottles.py tapper-config`), and runs `tapper push` with it.
After a real push it fetches `Formula/den.rb` from the tap's default
branch and fails unless it is this version with a `bottle do` block whose
`root_url` is the GA release and which has all four tags.

> **Never run a bare `tapper push`.** The committed `tapper.yaml`
> deliberately has no `formula_includes`, so a bare push publishes a
> formula with no bottle block and macOS installs build from source. If
> that happens, rerun `make release-tap` for the same tag to fix the tap.

tapper takes the token from `$HOMEBREW_TAP_TOKEN`, then
`$TAPPER_GITHUB_TOKEN`, then the keychain entry `tapper` /
`marcelocantos/homebrew-tap` (normally the keychain). `tapper token
status` shows which is in use; `tapper token import-op` refreshes the
keychain entry from 1Password.

## Homebrew bottles

Every RC carries Homebrew bottles built by `scripts/homebrew-bottles.py` from
the RC's own darwin-aarch64 tarball (nothing is recompiled):

- `den-X.Y.0.<tag>.bottle.tar.gz` for `arm64_golden_gate` (macOS 27),
  `arm64_tahoe`, `arm64_sequoia` and `arm64_sonoma`. The binary's minimum
  macOS is 14 and there is no Intel macOS build, so there are no older or
  `x86_64` tags, and no `:all` (it is a native binary).
- Each bottle is `den/X.Y.0/{bin/den,share/den/known_hashes.json,INSTALL_RECEIPT.json}`.
  The formula version is `X.Y.0` for RCs too, so RC and GA bottles are the
  same bytes; only the formula's `root_url` differs.
- The bottles use a fixed cellar (`/opt/homebrew/Cellar`) because the
  binary links libarchive from `/opt/homebrew/opt`. A Homebrew in any other
  prefix falls back to installing from the release tarball.
- `den.rb` on the RC release is a complete formula for that RC with its
  `bottle do` block. To try it, put it in a local tap
  (`brew tap-new <you>/den-rc`, then copy it to that tap's `Formula/`),
  `brew install <you>/den-rc/den`, and run
  `make verify-release VR_FORMULA=<you>/den-rc/den RELEASE_TAG=vX.Y.0`.
  The `poured` step fails unless brew poured the bottle.

### Testing an RC with verify-release

`VR_FORMULA` picks the formula verify-release checks. It defaults to
`marcelocantos/tap/den` (the GA tap); for an RC, point it at the RC tap, e.g.
`VR_FORMULA=marcelocantos/den-rc/den` (or your own `<you>/den-rc/den`):

```sh
make verify-release VR_FORMULA=marcelocantos/den-rc/den RELEASE_TAG=vX.Y.0
```

Homebrew 7 refuses to load a formula from a third-party tap it has not been
told to trust (`Refusing to load formula ... from untrusted tap ...`), which
breaks `brew test`, `brew info` and `brew --prefix <formula>`. So on brew 7
and later, before anything else touches the formula, verify-release runs
`brew trust --formula "$VR_FORMULA"` for that one formula (never the whole
tap) and reports it as the `brew-trust` step. If the trust fails, or
`VR_BREW_TRUST=0` and the formula is not already trusted, it stops with the
`brew trust --formula ...` command to run. On brew older than 7, or for an
official (homebrew/core) formula, nothing changes. Brew keeps its trust list
under your `HOME` (`~/.homebrew/trust.json`, or
`$XDG_CONFIG_HOME/homebrew/trust.json`), but verify-release runs brew under a
scratch `HOME`. It therefore copies that trust store into the scratch `HOME`
read-only (file 0400 in a 0500 directory), so `brew test` and the `poured`
check see the trust while brew can't write to either trust store. `brew-trust`
then confirms that brew under the scratch `HOME` loads the formula, before
`brew-test` runs.

Only a failed `version` (wrong binary) or `isolation` (den's config escapes
the scratch root) stops the run before den writes anything. A `brew-trust`,
`brew-test` or other step failure is reported and the run carries on.

`brew-test` runs `brew test` on the formula. On macOS 27, brew 7 refuses
`brew test` (and any source build) for every formula unless Xcode/CLT 27 is
installed. Only when brew refuses for that reason (`Your Xcode (...) is too
outdated` or `Your Command Line Tools are too outdated`) is `brew-test`
reported as `SKIP` with brew's message; any other `brew test` failure is a
`FAIL`. The separate `formula-test` step runs the formula's own test commands
directly (`den --version` and `den list` against a scratch
`DEN_HOME`/`DEN_PREFIX`), so the formula's checks still run when brew won't.

Homebrew only pours a bottle whose tag it can name. A brew older than 6.0
has no symbol for macOS 27, so on macOS 27 it pours no bottles at all (not
even homebrew-core's) and builds everything from source.

Promotion checks the RC's bottles (every tag present, matching the RC
checksums, holding exactly the RC's darwin binary) and refuses an RC
without them. It copies them to the GA release with the other assets.
`make release-tap` then publishes a tap formula whose bottle block points
at the GA release.

## How to abort

Simply don't promote. The RC prerelease is harmless — `install.sh` ignores
prereleases. If you want to free the tag and clean up the release list:

```sh
gh release delete v0.12.0-rc.1 --cleanup-tag
```

## Workflow concurrency

`release-candidate.yml` uses `cancel-in-progress: false` — a running RC build
is never cancelled. If you trigger a second run for the same version while one
is in flight, the second run will use the next free `rc.N` and run in parallel.
Both will publish separate prerelease tags.

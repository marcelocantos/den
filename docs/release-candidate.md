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
5. Bumps the tap formula, with a bottle block pointing at the GA release.
6. Leaves the RC release in place as a historical record.

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

Homebrew only pours a bottle whose tag it can name. A brew older than 6.0
has no symbol for macOS 27, so on macOS 27 it pours no bottles at all (not
even homebrew-core's) and builds everything from source.

Promotion checks the RC's bottles (every tag present, matching the RC
checksums, holding exactly the RC's darwin binary) and refuses an RC
without them. It copies them to the GA release with the other assets.
The tap formula's bottle block then points at the GA release.

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

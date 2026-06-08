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
checksums, and harness logs attached.

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
2. Enforces the macOS harness attestation (fails fast if false).
3. Creates the GA tag (`v0.12.0`) on the same commit as the RC tag.
4. Creates the GA GitHub release with the same assets.
5. Leaves the RC release in place as a historical record.

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

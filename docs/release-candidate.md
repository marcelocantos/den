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

### Verify the RC with verify-release

`make verify-release RELEASE_TAG=vX.Y.0-rc.N` runs the end-to-end battery
against the RC exactly as a user gets it: `install.sh` installs the tag
(`DEN_VERSION=vX.Y.0-rc.N`, which fetches the RC's GA-named
`den-X.Y.0-<os>-<arch>.tar.gz`) into a scratch root. Homebrew is not used or
needed:

- `HOME`, `DEN_HOME`, `DEN_PREFIX` and `DEN_INSTALL_DIR` all live under a
  fresh `mktemp -d` root (or `VR_ROOT`); a scratch path under `/opt/homebrew`,
  `/usr/local`, Linuxbrew, `$HOMEBREW_PREFIX` or the real home is refused.
- `/opt/homebrew` and Linuxbrew directories are dropped from `PATH`, and
  every `DYLD_*` variable is unset, for install.sh and den alike.

Steps (`VR_ONLY` / `VR_SKIP` pick a subset):

| Step | Checks |
|---|---|
| `install` | install.sh installs the tag into the scratch `DEN_INSTALL_DIR` (never skipped) |
| `version` | `den --version` prints `X.Y.0` (an RC reports its GA version) |
| `isolation` | `den config` keeps `den_home`, `store` and `prefix` under the scratch root (never skipped) |
| `links` | `scripts/check-links.sh`: the binary links only base-OS libraries |
| `checksum` | the release tarball matches its `.sha256` and `den-X.Y.0-checksums.txt`, and its `den` and trust replica are the bytes install.sh installed |
| `self-update` | install an earlier release with install.sh, then `den self-update --version <tag>`; the result must match the `install` bytes, replica included. The start is the newest earlier tag, RCs included, whose binary supports `self-update --version` (at most `VR_PREV_MAX`, default 5, candidates are installed; one that fails to install is skipped); `VR_PREV_TAG` pins it instead. With no capable candidate it falls back to the newest earlier GA, and SKIPs when that predates `self-update --version` and the tag isn't Latest |
| `empty-store` | `den list` in a fresh store reports no packages |
| `update` … `cleanup` | index update, bottle install, opt link, trust replica, pin, upgrade, dependents, tap, font and app casks (macOS), uninstall, doctor, cleanup |
| `launchd` | macOS: `den daemon install` writes `dev.den.daemon.plist` into the scratch `~/Library/LaunchAgents` (its own `launchctl` load is stubbed so the real agent is untouched); that plist is checked, relabelled `den.verify-release.<pid>`, loaded, watched for `VR_LAUNCHD_SECS` and removed |

A failed `install`, `version` or `isolation` stops the run before den writes
anything; any other failure is reported and the run carries on. The scratch
root is removed on success and kept (path printed) on failure or with
`VR_KEEP=1`. See the header of `scripts/verify-release.sh` for every knob.

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
   checks the RC's binary version.
3. Creates the GA tag (`v0.12.0`) on the same commit as the RC tag.
4. Creates the GA GitHub release with the same assets.
5. Leaves the RC release in place as a historical record.

den is not distributed through Homebrew: releases carry no bottles and no
formula, and nothing publishes to a tap. Install with `install.sh` or
`den self-update`.

## How to abort

Simply don't promote. The RC prerelease is harmless — `install.sh` ignores
prereleases unless `DEN_VERSION` names one. If you want to free the tag and clean up the release list:

```sh
gh release delete v0.12.0-rc.1 --cleanup-tag
```

## Workflow concurrency

`release-candidate.yml` uses `cancel-in-progress: false` — a running RC build
is never cancelled. If you trigger a second run for the same version while one
is in flight, the second run will use the next free `rc.N` and run in parallel.
Both will publish separate prerelease tags.

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
| `self-update` | install the previous GA (`VR_PREV_TAG`, default: the newest earlier `vX.Y.Z` tag) with install.sh, then `den self-update --version <tag>`; the result must match the `install` bytes, replica included. SKIP when that GA predates `self-update --version` and the tag isn't Latest |
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
  then `brew install <you>/den-rc/den`. (verify-release no longer goes
  through Homebrew; see [Verify the RC](#verify-the-rc-with-verify-release).)

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
prereleases unless `DEN_VERSION` names one. If you want to free the tag and clean up the release list:

```sh
gh release delete v0.12.0-rc.1 --cleanup-tag
```

## Workflow concurrency

`release-candidate.yml` uses `cancel-in-progress: false` — a running RC build
is never cancelled. If you trigger a second run for the same version while one
is in flight, the second run will use the next free `rc.N` and run in parallel.
Both will publish separate prerelease tags.

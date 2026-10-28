# den trust model (🎯T64 / 🎯T42 / 🎯T44)

Every install cross-checks a bottle's SHA256 against **two hash sources**:

| Source | Where den reads it | Role |
|--------|--------------------|------|
| A. Homebrew API | `formulae.brew.sh`, via den's package index | The hash Homebrew publishes for the bottle. |
| B. den replica | the local snapshot shipped with den (`data/known_hashes.json` → `share/den/known_hashes.json`, also compiled into the binary), or `~/.den/trust/known_hashes.json` | A GHCR-confirmed copy maintained in den's repo. |

Nothing is fetched at install time: source B is always the local snapshot.
It reaches users with each den release (or a user-placed override), not from
`raw.githubusercontent.com` at install time.

**What source B is, and is not.** Replica entries are seeded by CI
(`.github/workflows/replica-verify.yml`) from formulae.brew.sh's own hashes;
each new or changed hash is confirmed by pulling that bottle from GHCR and
recomputing its digest before it is committed. So B is an *earlier,
GHCR-confirmed copy of A*, not an independent root of trust. It detects
Homebrew's API changing a covered bottle's hash after the snapshot was taken
(an attacker would also have to land the forged hash in den's repo and a den
release), but it cannot detect a bottle that was already bad when it was
seeded, and it covers only the bottles in the snapshot.

## Install-time cross-check

`install_one()` (`src/provider/homebrew_provider.cpp`) calls
`cross_check_hashes()` (`src/trust/trust_model.*`) **before** any download or
extraction:

| A vs B | Decision | Behaviour |
|--------|----------|-----------|
| agree | `Proceed` | install continues |
| disagree | `Refuse` | throws `UserError`, no extraction |
| snapshot exists but is unreadable/corrupt | `Refuse` | fails closed: "replica snapshot … is unreadable" |
| snapshot has no entry for the bottle | `WarnProceed` | "no replica entry for this bottle"; recorded as a degraded install |
| no snapshot at all | `WarnProceed` | "no replica snapshot available"; recorded as a degraded install |
| neither source has a hash | `Refuse` | cannot verify integrity |

Source B is resolved by `locate_local_replica()` (den_home override, then the
`share/den` beside the executable, then the build tree's `data/`, then the
embedded copy). `parse_replica_document()` rejects malformed JSON and any
digest that is not 64 lowercase hex characters; such a snapshot refuses
installs rather than being treated as absent, so corrupting the file cannot
switch the cross-check off.

Every `WarnProceed` install that pours is recorded in
`~/.den/trust/degraded_installs.json` (name, version, reason, bottle tag,
keg). Re-pouring the same version with both sources agreeing clears it, and
kegs no longer on disk are not counted.

## Observability — `den doctor`

`den doctor` prints a trust block (`src/doctor/trust_checks.*`):

```
[trust] sources: formulae.brew.sh + den-replica-snapshot
[trust] replica: active, entries: 12, last sync: bundled-with-den-0.12.0
[trust] replica source: /opt/homebrew/share/den/known_hashes.json
[trust] degraded installs: 2 (no-replica-entry: 2)
[trust] layers: cross-source-hash-agreement
```

Findings: an **error** when the snapshot is unreadable (installs are refused
until it is repaired or removed), a warning when there is no snapshot, a
warning listing kegs installed on one hash source, and a warning when no
advanced trust layer is wired (🎯T44). `den daemon status` prints the same
degraded-install count (`Degraded-trust installs: …`).

## The replica snapshot (`data/known_hashes.json`)

A flat JSON document. Keys are `"<formula>--<version>--<platform_tag>"`; values
are the verified SHA256 of that bottle.

```json
{
  "schema_version": 1,
  "generated_at": "…",
  "source": "…",
  "hashes": { "tree--2.3.2--arm64_sequoia": "ef36…ca668" }
}
```

The file is shipped with den (installed to `share/den/known_hashes.json`, see
`CMakeLists.txt`) so offline installs still cross-check. A user-synced copy at
`~/.den/trust/known_hashes.json` overrides the bundled one.

## Updating the replica

The replica is **diff-based** — only changed hashes are re-verified.

1. `.github/workflows/replica-verify.yml` runs daily (and on demand).
2. `scripts/replica_verify.py` fetches each tracked formula's current bottle
   hashes from `formulae.brew.sh` and diffs them against the committed snapshot.
3. For every **changed** hash it downloads the bottle from GHCR by digest and
   runs `den replica-verify`, which recomputes the SHA256
   (`verify_diff_entry`).
4. A mismatch (Homebrew API ≠ GHCR content) **fails the run and opens an
   issue**; the snapshot is not updated.
5. Verified changes are committed back to `data/known_hashes.json`.

GHCR access and the live Homebrew API are only reachable from CI, so this
pipeline runs in CI. The verification primitive it calls (`verify_diff_entry` /
`den replica-verify`) is covered by `den_tests`.

### Adding a formula to the replica

Add its `"<formula>--<version>--<tag>"` entries (with current SHA256s from
`formulae.brew.sh`) to `data/known_hashes.json`. The next workflow run
GHCR-verifies them; thereafter they are tracked automatically.

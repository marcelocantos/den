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

## Unattended installs — `trust.unattended`

An interactive install that only one hash source vouches for pours with a
warning that the person who asked for it sees. Nobody sees the warning from
an unattended run, so `trust.unattended` decides what happens instead
(`src/trust/unattended.*`):

| cross-check | interactive | `trust.unattended=warn` (default) | `trust.unattended=refuse` |
|---|---|---|---|
| both sources agree | install | install | install |
| hashes disagree | refuse | refuse | refuse |
| no replica entry / no snapshot | warn, pour, ledger | pour, ledger | hold back, ledger "refused" |
| snapshot unreadable | refuse | refuse | refuse |

A run is unattended when it is the daemon, when `--unattended` is passed
(`den --unattended upgrade`), or when it runs as den's launchd job
(`XPC_SERVICE_NAME=dev.den.daemon`). A missing TTY on its own does not count,
so CI and scripted installs behave as before, and `--yes` never counts.
An unknown `trust.unattended` value (or one of the wrong type) is treated as
`refuse`, an unknown `trust.unattended_source_builds` as `off`, and
`den doctor` warns about either.

**Holding back skips, it does not abort.** Before each upgrade pass the
daemon screens its pending upgrades, and `den --unattended upgrade` screens
its candidates with the same code. A formula that would be held back (single
source under `refuse`) or refused (hashes disagree, snapshot unreadable)
stays pending and sits the run out, and so does every pending upgrade that
would have to install it. The rest carries on. The CLI prints the held set
once and exits non-zero after upgrading the rest; the daemon notifies once.
If the screen itself fails, nothing is upgraded.

Every hold goes in the ledger as `{state: "refused", reason, status,
bottle_sha, bottle_tag, recorded_at}`, so `den trust review` lists all of
them. `reason` is one of:

| reason | meaning |
|---|---|
| `single-source` | one hash source only (`status` says which is missing) |
| `hash-mismatch` | Homebrew and the replica disagree |
| `replica-unreadable` | the replica snapshot exists but cannot be read |
| `unverifiable` | neither source has a hash |
| `depends on <formula>: …` | held back because it would install `<formula>` |
| `unattended-source-build` | a source build deferred (see source builds) |

An unattended install that refuses outside a screened upgrade (for example
a dependency) is recorded the same way.

**Notifications.** At most one macOS notification per daemon pass, and only
when the pass holds back something it did not hold back in the previous one.
It names the count and points to `den doctor`. Turn it off with
`den set trust.notify false`. `den daemon status` shows what is held back and
when the last notification went out.

**Review and accept.** `den trust review` lists what is held back or
deferred, what has been accepted, and what was installed on a single source.
`den trust accept <formula>@<version>` (for example
`den trust accept openssl@3@3.6.3`) marks one exact version accepted. Only a
`single-source` hold or a deferred source build can be accepted; a mismatch
or an unreadable snapshot has to be fixed at the source. The next
unattended run installs it (recorded as a degraded install) and the refused
entry goes away. A newer version is screened afresh. `accept` must be run
interactively.

**Source builds.** See [source-builds.md](source-builds.md#unattended-source-builds).

Self-update is not governed by this policy; it keeps its own
release-signature path ([self-update.md](self-update.md)).

## Observability — `den doctor`

`den doctor` prints a trust block (`src/doctor/trust_checks.*`):

```
[trust] sources: formulae.brew.sh + den-replica-snapshot
[trust] replica: active, entries: 12, last sync: bundled-with-den-0.12.0
[trust] replica source: /opt/homebrew/share/den/known_hashes.json
[trust] degraded installs: 2 (no-replica-entry: 2)
[trust] held back (unattended): 0
[trust] layers: cross-source-hash-agreement
```

Findings: an **error** when the snapshot is unreadable (installs are refused
until it is repaired or removed), a warning when there is no snapshot, a
warning listing kegs installed on one hash source, and a warning when no
advanced trust layer is wired (🎯T44), plus a warning
"N refused, M degraded since <time>" listing what the unattended trust policy
held back. `den daemon status` prints the same degraded-install count
(`Degraded-trust installs: …`), the ledger summary and the policy in force.

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

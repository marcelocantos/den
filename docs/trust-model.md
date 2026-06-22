# den trust model (🎯T64 / 🎯T42 / 🎯T44)

den's bottle-integrity guarantee does not rest on a single party. Every
install cross-checks a bottle's SHA256 against **two independent hash
sources**, each rooted in a different origin/CDN:

| Source | Origin | Role |
|--------|--------|------|
| A. Homebrew API | `formulae.brew.sh` | The hash baked into den's package index. |
| B. den replica | `raw.githubusercontent.com` (`data/known_hashes.json`) | An independent, GHCR-verified copy maintained by den. |

`raw.githubusercontent.com` is a separate origin and CDN from
`formulae.brew.sh`. To swap a bottle undetected an attacker must compromise
**both**: forge a matching SHA256 in Homebrew's API *and* land a matching
forged hash in den's GitHub repo. The latter is gated by CI
(`.github/workflows/replica-verify.yml`), which re-downloads each changed
bottle from GHCR and recomputes its digest before the hash is ever committed.

## Install-time cross-check

`install_one()` (`src/provider/homebrew_provider.cpp`) calls
`cross_check_hashes()` (`src/trust/trust_model.*`) **before** any download or
extraction:

| A vs B | Decision | Behaviour |
|--------|----------|-----------|
| agree | `Proceed` | install continues |
| disagree (both reachable) | `Refuse` | throws `UserError`, no extraction |
| one source absent | `WarnProceed` | `SPDLOG_WARN`, install continues (degraded trust) |
| neither reachable | `Refuse` | cannot verify integrity |

Source B is read from the bundled snapshot (resolved via the candidate list in
`local_replica_path()`); the hot install path never blocks on a live network
fetch. A corrupt snapshot fails closed — `parse_replica_document()` rejects
malformed JSON and any digest that is not 64 lowercase hex characters.

## Observability — `den doctor`

`den doctor` prints a trust block (`src/doctor/trust_checks.*`):

```
[trust] sources: formulae.brew.sh + den-replica-cdn
[trust] replica: active, entries: 12, last sync: bundled-with-den-0.12.0
[trust] layers: cross-source-hash-agreement
```

It also emits findings when the replica is inactive/degraded or when no
advanced trust layer is wired (🎯T44).

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

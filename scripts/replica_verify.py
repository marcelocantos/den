#!/usr/bin/env python3
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# 🎯T42/T64: diff-based replica verifier driver.
#
# Compares the current bottle SHA256s from formulae.brew.sh against den's
# committed replica snapshot (data/known_hashes.json). For every CHANGED hash,
# downloads the bottle from GHCR and re-verifies it via `den replica-verify`
# (which recomputes the SHA256). A mismatch fails the run; verified changes are
# written back to the snapshot so the workflow can commit them.
#
# Source archives are tracked the same way. A formula with a
# "<formula>--<version>--source" key has its current source archive URL and
# SHA256 read from formulae.brew.sh; when that hash is new, the archive is
# downloaded from its upstream URL (the project's own server, not Homebrew's)
# and re-verified with `den replica-verify --tag source`. Unattended
# allowlisted source builds (trust.source_build_allowlist) need these entries
# to cross-check.
#
# This only runs in CI: it needs the live formulae.brew.sh API and GHCR. The
# verification primitive it invokes (verify_diff_entry) is unit-tested locally.

import json
import os
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

REPLICA_PATH = Path(os.environ.get("REPLICA_PATH", "data/known_hashes.json"))
DEN_BIN = os.environ.get("DEN_BIN", "build/den")
# formulae.brew.sh's formula API; overridable (e.g. a file:// fixture) for tests.
API_BASE = os.environ.get("REPLICA_API_BASE", "https://formulae.brew.sh/api/formula")
SOURCE_TAG = "source"
USER_AGENT = "den-replica-verify"
GHCR_TOKEN_URL = (
    "https://ghcr.io/token?scope=repository:homebrew/core/{repo}:pull&service=ghcr.io"
)
GHCR_BLOB_URL = "https://ghcr.io/v2/homebrew/core/{repo}/blobs/sha256:{sha}"


def http_get(url: str, headers: dict | None = None) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, **(headers or {})})
    with urllib.request.urlopen(req, timeout=120) as resp:
        return resp.read()


def http_get_json(url: str) -> dict:
    return json.loads(http_get(url).decode("utf-8"))


def ghcr_repo_path(formula: str) -> str:
    # "python@3.12" → "python/3.12"; "tree" → "tree".
    return formula.replace("@", "/")


def current_hashes_for(formula: str) -> tuple[str, dict[str, str]]:
    """Return (version, {tag: sha256}) from formulae.brew.sh for one formula."""
    data = http_get_json(f"{API_BASE}/{formula}.json")
    version = data["versions"]["stable"]
    bottle = data.get("bottle", {}).get("stable", {})
    files = bottle.get("files", {})
    return version, {tag: info["sha256"] for tag, info in files.items()}


def current_source_for(formula: str) -> tuple[str, str, str]:
    """Return (version, url, sha256) of the stable source archive from formulae.brew.sh."""
    data = http_get_json(f"{API_BASE}/{formula}.json")
    stable = data["urls"]["stable"]
    sha = stable.get("checksum")
    if not isinstance(sha, str) or not sha:
        raise ValueError(f"{formula} has no stable source checksum")
    return data["versions"]["stable"], stable["url"], sha


def download_source(url: str, dest: Path) -> bool:
    """Download a source archive from its upstream URL. Returns True on success."""
    try:
        dest.write_bytes(http_get(url))
    except Exception as exc:  # noqa: BLE001 — network failure is reported, not fatal
        print(f"  WARN: source download failed for {url}: {exc}")
        return False
    return True


def tracked_formulae(snapshot: dict[str, str]) -> tuple[list[str], list[str]]:
    """(bottle-tracked, source-tracked) formulae from "<formula>--<version>--<tag>" keys."""
    bottles: set[str] = set()
    sources: set[str] = set()
    for key in snapshot:
        parts = key.split("--")
        if len(parts) < 3:
            continue
        formula, tag = parts[0], parts[-1]
        (sources if tag == SOURCE_TAG else bottles).add(formula)
    return sorted(bottles), sorted(sources)


def download_ghcr_blob(formula: str, sha: str, dest: Path) -> bool:
    """Download a bottle blob from GHCR by digest. Returns True on success."""
    repo = ghcr_repo_path(formula)
    try:
        token = http_get_json(GHCR_TOKEN_URL.format(repo=repo))["token"]
        blob = http_get(
            GHCR_BLOB_URL.format(repo=repo, sha=sha),
            headers={"Authorization": f"Bearer {token}"},
        )
    except Exception as exc:  # noqa: BLE001 — network failure is reported, not fatal
        print(f"  WARN: GHCR download failed for {formula} sha256:{sha[:12]}…: {exc}")
        return False
    dest.write_bytes(blob)
    return True


def verify_entry(formula: str, version: str, tag: str, sha: str, bottle: Path) -> bool:
    """Invoke `den replica-verify` on a downloaded bottle. True == match."""
    result = subprocess.run(
        [
            DEN_BIN, "replica-verify",
            "--name", formula, "--version", version,
            "--tag", tag, "--sha256", sha, "--bottle", str(bottle),
        ],
        capture_output=True, text=True,
    )
    print(f"  {result.stdout.strip()}")
    if result.returncode != 0:
        print(f"  {result.stderr.strip()}")
    return result.returncode == 0


def main() -> int:
    doc = json.loads(REPLICA_PATH.read_text())
    snapshot: dict[str, str] = dict(doc.get("hashes", {}))

    # The formulae to track come from the existing snapshot keys
    # ("<formula>--<version>--<tag>"): bottle tags track the formula's
    # bottles, a "source" tag tracks its source archive. The replica starts
    # small and grows as formulae are added to the bootstrap snapshot.
    bottle_formulae, source_formulae = tracked_formulae(snapshot)
    if not bottle_formulae and not source_formulae:
        print("Replica snapshot is empty — nothing to verify.")
        return 0

    if bottle_formulae:
        print(f"Tracking bottles of {len(bottle_formulae)} formula(e): "
              f"{', '.join(bottle_formulae)}")
    if source_formulae:
        print(f"Tracking sources of {len(source_formulae)} formula(e): "
              f"{', '.join(source_formulae)}")

    changed = 0
    failures = 0
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        for formula in bottle_formulae:
            try:
                version, tag_hashes = current_hashes_for(formula)
            except Exception as exc:  # noqa: BLE001
                print(f"WARN: could not fetch {formula} from formulae.brew.sh: {exc}")
                continue

            for tag, sha in tag_hashes.items():
                key = f"{formula}--{version}--{tag}"
                if snapshot.get(key) == sha:
                    continue  # unchanged — skip (diff-based)

                print(f"CHANGED {key}")
                changed += 1
                bottle = tmp / f"{formula}-{version}-{tag}.tar.gz"
                if not download_ghcr_blob(formula, sha, bottle):
                    # Cannot verify against GHCR — do NOT trust the new hash.
                    failures += 1
                    continue
                if verify_entry(formula, version, tag, sha, bottle):
                    snapshot[key] = sha  # GHCR-verified — accept into replica
                else:
                    # formulae.brew.sh and GHCR disagree: a fabrication. Reject.
                    print(f"  REJECT {key}: GHCR content does not match claimed SHA256")
                    failures += 1

        for formula in source_formulae:
            try:
                version, url, sha = current_source_for(formula)
            except Exception as exc:  # noqa: BLE001
                print(f"WARN: could not fetch {formula} source from formulae.brew.sh: {exc}")
                continue

            key = f"{formula}--{version}--{SOURCE_TAG}"
            if snapshot.get(key) == sha:
                continue  # unchanged — skip (diff-based)

            print(f"CHANGED {key}")
            changed += 1
            archive = tmp / f"{formula}-{version}-{SOURCE_TAG}"
            if not download_source(url, archive):
                # Cannot verify against upstream — do NOT trust the new hash.
                failures += 1
                continue
            if verify_entry(formula, version, SOURCE_TAG, sha, archive):
                snapshot[key] = sha  # upstream-verified — accept into replica
            else:
                print(f"  REJECT {key}: upstream archive does not match claimed SHA256")
                failures += 1

    if failures:
        print(f"\n{failures} verification failure(s) — replica NOT updated.")
        return 1

    if changed:
        doc["hashes"] = dict(sorted(snapshot.items()))
        REPLICA_PATH.write_text(json.dumps(doc, indent=2) + "\n")
        print(f"\n{changed} hash(es) verified and written to {REPLICA_PATH}.")
    else:
        print("\nNo changed hashes — replica is up to date.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

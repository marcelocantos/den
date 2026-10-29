#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Tests for scripts/replica_verify.py's source-archive tracking: a
# "<formula>--<version>--source" key tracks that formula's source archive
# (not its bottles); a changed source hash is accepted only when the archive
# downloaded from its upstream URL hashes to it. formulae.brew.sh and the
# upstream are file:// fixtures, so this runs offline.
#
# Run:
#   DEN_TEST_BIN=build/den sh tests/scripts/test_replica_verify.sh
# or via ctest (name: script_test_replica_verify).

set -u

_SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT="${_SELF_DIR}/../.."
SCRIPT="${ROOT}/scripts/replica_verify.py"

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not found"
    exit 0
fi
if [ -z "${DEN_TEST_BIN:-}" ] || [ ! -x "${DEN_TEST_BIN}" ]; then
    echo "SKIP: DEN_TEST_BIN not set"
    exit 0
fi

_pass=0
_fail=0
_tmp=$(mktemp -d "${TMPDIR:-/tmp}/replica-verify-test.XXXXXX")
trap 'rm -rf "$_tmp"' EXIT

ok() { _pass=$((_pass + 1)); echo "PASS: $1"; }
ko() { _fail=$((_fail + 1)); echo "FAIL: $1"; [ -f "$_tmp/out" ] && sed 's/^/    /' "$_tmp/out"; }

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

API="$_tmp/api"
UP="$_tmp/upstream"
mkdir -p "$API" "$UP"
printf 'widget 2.0 source\n' >"$UP/widget-2.0.tar.gz"
GOOD=$(sha256 "$UP/widget-2.0.tar.gz")
OLD=$(printf '%064d' 1)
BAD=$(printf '%064d' 2)

# api VERSION SHA [URL] — formulae.brew.sh's widget.json. It also lists a
# bottle, so a run that wrongly tracked bottles would try GHCR and fail.
api() {
    cat >"$API/widget.json" <<JSON
{"name": "widget", "versions": {"stable": "$1"},
 "urls": {"stable": {"url": "${3:-file://$UP/widget-$1.tar.gz}", "checksum": "$2"}},
 "bottle": {"stable": {"files": {"arm64_tahoe": {"sha256": "$BAD"}}}}}
JSON
}

# replica KEY SHA — a one-entry snapshot.
replica() {
    printf '{"schema_version": 1, "hashes": {"%s": "%s"}}\n' "$1" "$2" >"$_tmp/known_hashes.json"
}

# run WANT_EXIT NAME — run the verifier, compare exit status.
run() {
    REPLICA_PATH="$_tmp/known_hashes.json" REPLICA_API_BASE="file://$API" \
        DEN_BIN="$DEN_TEST_BIN" python3 "$SCRIPT" >"$_tmp/out" 2>&1
    _got=$?
    if [ "$_got" -eq "$1" ]; then ok "$2"; else ko "$2 (exit $_got, want $1)"; fi
}

has_entry() { # NAME KEY SHA
    if python3 - "$_tmp/known_hashes.json" "$2" "$3" <<'PY'
import json, sys
h = json.load(open(sys.argv[1]))["hashes"]
sys.exit(0 if h.get(sys.argv[2]) == sys.argv[3] else 1)
PY
    then ok "$1"; else ko "$1 (want $2 = $3)"; fi
}

lacks_entry() { # NAME KEY
    if python3 - "$_tmp/known_hashes.json" "$2" <<'PY'
import json, sys
sys.exit(1 if sys.argv[2] in json.load(open(sys.argv[1]))["hashes"] else 0)
PY
    then ok "$1"; else ko "$1 (unexpected $2)"; fi
}

# Unchanged: the current source hash is already in the snapshot. Nothing is
# downloaded (the URL would fail) and the bottle is not tracked.
api 2.0 "$GOOD" "file://$UP/missing.tar.gz"
replica "widget--2.0--source" "$GOOD"
cp "$_tmp/known_hashes.json" "$_tmp/before.json"
run 0 "unchanged source entry: no work"
if cmp -s "$_tmp/before.json" "$_tmp/known_hashes.json"; then ok "unchanged: snapshot untouched"
else ko "unchanged: snapshot untouched"; fi
lacks_entry "source-only formula: bottles not tracked" "widget--2.0--arm64_tahoe"

# A new version whose upstream archive matches the claimed hash is added.
api 2.0 "$GOOD"
replica "widget--1.0--source" "$OLD"
run 0 "new version verified against upstream"
has_entry "new version: entry added" "widget--2.0--source" "$GOOD"
has_entry "new version: old entry kept" "widget--1.0--source" "$OLD"
grep -q "MATCH widget 2.0 source" "$_tmp/out" && ok "new version: den replica-verify matched" \
    || ko "new version: den replica-verify matched"

# The upstream archive does not hash to the claimed value: rejected.
api 2.0 "$BAD"
replica "widget--1.0--source" "$OLD"
run 1 "mismatched upstream archive fails the run"
lacks_entry "mismatch: entry not added" "widget--2.0--source"
grep -q "REJECT widget--2.0--source" "$_tmp/out" && ok "mismatch: reported" \
    || ko "mismatch: reported"

# The upstream archive cannot be downloaded: not trusted.
api 2.0 "$GOOD" "file://$UP/missing.tar.gz"
replica "widget--1.0--source" "$OLD"
run 1 "unreachable upstream fails the run"
lacks_entry "unreachable: entry not added" "widget--2.0--source"

# No checksum in the API: a warning, nothing changes.
cat >"$API/widget.json" <<JSON
{"name": "widget", "versions": {"stable": "2.0"}, "urls": {"stable": {"url": "file://$UP/widget-2.0.tar.gz"}}}
JSON
replica "widget--1.0--source" "$OLD"
run 0 "no source checksum: skipped with a warning"
grep -q "WARN: could not fetch widget source" "$_tmp/out" && ok "no checksum: warned" \
    || ko "no checksum: warned"
lacks_entry "no checksum: entry not added" "widget--2.0--source"

echo
echo "replica_verify: $_pass passed, $_fail failed"
[ "$_fail" -eq 0 ]

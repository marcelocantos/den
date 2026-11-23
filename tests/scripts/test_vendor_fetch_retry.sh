#!/bin/sh
# Regression test for cmake/den_fetch_retry.cmake and cmake/DenFetchRetry.cmake:
# bounded retries for the vendored libarchive/codec source downloads.
#
# Everything runs against a local HTTP fixture (tests/scripts/fixtures/
# flaky_http_server.py) on 127.0.0.1: no network access needed.
#
#   1. Script cases: transient failures (HTTP 504/429, 504 behind a 302,
#      curl 18 truncation, curl 56 reset) recover with the exact attempt
#      count; exhausted 504 stops at 3; 404 and hash mismatch fail after one
#      attempt and leave no file; a verified cached file makes no request.
#   2. ExternalProject cases: a URL ExternalProject with the fetch_retry step
#      survives one 504 and still extracts via CMake's URL_HASH path; the same
#      project without the step (DEN_VENDOR_FETCH_RETRY=OFF) fails, proving
#      the fixture reproduces the stock single-attempt failure.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CMAKE=${CMAKE:-cmake}
SCRIPT="$ROOT/cmake/den_fetch_retry.cmake"

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not found"
    exit 0
fi

tmp=$(mktemp -d)
server_pid=
cleanup() {
    [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null || true
    rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

# Payload: a real tar.gz so the ExternalProject cases can extract it.
mkdir -p "$tmp/payload/fx-1.0"
echo "fixture payload" > "$tmp/payload/fx-1.0/README"
(cd "$tmp/payload" && "$CMAKE" -E tar czf "$tmp/fx.tar.gz" fx-1.0)
SHA=$("$CMAKE" -E sha256sum "$tmp/fx.tar.gz" | cut -d' ' -f1)

python3 "$ROOT/tests/scripts/fixtures/flaky_http_server.py" \
    "$tmp/fx.tar.gz" "$tmp/port" "$tmp/requests.log" &
server_pid=$!
i=0
while [ ! -s "$tmp/port" ]; do
    i=$((i + 1))
    [ $i -gt 100 ] && { echo "FAIL: fixture server did not start"; exit 1; }
    sleep 0.1
done
BASE="http://127.0.0.1:$(cat "$tmp/port")"
: > "$tmp/requests.log"

export DEN_FETCH_BACKOFF_SECONDS=0
fails=0
n=0

requests() { grep -c "^$1\$" "$tmp/requests.log" || true; }

# fetch_case <name> <path> <expect: ok|fail> <expected requests to path> [count-path]
fetch_case() {
    name=$1 path=$2 expect=$3 want=$4 countpath=${5:-$2}
    n=$((n + 1))
    dest="$tmp/dl/$n/fx.tar.gz"
    rc=0
    "$CMAKE" -DURL="$BASE$path" -DSHA256="$SHA" -DDEST="$dest" -P "$SCRIPT" \
        >"$tmp/out.$n" 2>&1 || rc=$?
    got=$(requests "$countpath")
    ok=1
    if [ "$expect" = ok ]; then
        [ $rc -eq 0 ] || ok=0
        [ -f "$dest" ] && [ "$("$CMAKE" -E sha256sum "$dest" | cut -d' ' -f1)" = "$SHA" ] || ok=0
    else
        [ $rc -ne 0 ] || ok=0
        [ ! -e "$dest" ] || ok=0
    fi
    [ ! -e "$dest.partial" ] || ok=0
    [ "$got" = "$want" ] || ok=0
    if [ $ok -eq 1 ]; then
        echo "ok   $name (exit $rc, $got request(s))"
        grep -o 'den_fetch_retry: .*' "$tmp/out.$n" | sed 's/^/     | /' || true
    else
        echo "FAIL $name: exit $rc, $got request(s) (want $expect, $want)"
        sed 's/^/     | /' "$tmp/out.$n"
        fails=$((fails + 1))
    fi
}

fetch_case "200 first try"                 /ok/a/fx.tar.gz          ok   1
fetch_case "504 then 200"                  /e504/1/b/fx.tar.gz      ok   2
fetch_case "504 x2 then 200"               /e504/2/c/fx.tar.gz      ok   3
fetch_case "429 then 200"                  /e429/1/d/fx.tar.gz      ok   2
fetch_case "302 to 504, then 200"          /redir504/1/e/fx.tar.gz  ok   2 /e504/1/e/fx.tar.gz
fetch_case "truncated body (curl 18)"      /trunc/1/f/fx.tar.gz     ok   2
fetch_case "connection reset (curl 56)"    /reset/1/g/fx.tar.gz     ok   2
fetch_case "504 exhausted: 3 attempts"     /e504/9/h/fx.tar.gz      fail 3
fetch_case "404: no retry"                 /e404/i/fx.tar.gz        fail 1
fetch_case "hash mismatch: no retry"       /bad/j/fx.tar.gz         fail 1

# Verified cache: a correct file already at DEST makes no request at all.
n=$((n + 1))
mkdir -p "$tmp/dl/$n"
cp "$tmp/fx.tar.gz" "$tmp/dl/$n/fx.tar.gz"
"$CMAKE" -DURL="$BASE/ok/cache/fx.tar.gz" -DSHA256="$SHA" -DDEST="$tmp/dl/$n/fx.tar.gz" \
    -P "$SCRIPT" >"$tmp/out.$n" 2>&1 || true
if [ "$(requests /ok/cache/fx.tar.gz)" = 0 ] && grep -q "cached, SHA-256 verified" "$tmp/out.$n"; then
    echo "ok   verified cache: no request"
else
    echo "FAIL verified cache made a request"; sed 's/^/     | /' "$tmp/out.$n"
    fails=$((fails + 1))
fi

# ExternalProject integration: one 504 before the archive.
ep_case() {
    name=$1 retry=$2 path=$3 expect=$4
    n=$((n + 1))
    proj="$tmp/ep$n"
    mkdir -p "$proj"
    cat > "$proj/CMakeLists.txt" <<CMAKE
cmake_minimum_required(VERSION 3.24)
project(fetch_retry_fixture NONE)
include(ExternalProject)
include("$ROOT/cmake/DenFetchRetry.cmake")
ExternalProject_Add(fx
    URL "$BASE$path"
    URL_HASH SHA256=$SHA
    DOWNLOAD_NAME fx.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PREFIX "\${CMAKE_BINARY_DIR}/ep"
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND "")
den_vendor_fetch_retry(fx URL "$BASE$path" SHA256 $SHA DOWNLOAD_NAME fx.tar.gz)
CMAKE
    rc=0
    { "$CMAKE" -S "$proj" -B "$proj/build" -DDEN_VENDOR_FETCH_RETRY=$retry \
        && "$CMAKE" --build "$proj/build"; } >"$tmp/out.$n" 2>&1 || rc=$?
    extracted="$proj/build/ep/src/fx/README"
    if [ "$expect" = ok ] && [ $rc -eq 0 ] && [ -f "$extracted" ]; then
        echo "ok   $name (built, extracted)"
    elif [ "$expect" = fail ] && [ $rc -ne 0 ] && [ ! -f "$extracted" ]; then
        echo "ok   $name (build failed as expected)"
    else
        echo "FAIL $name: exit $rc, expected $expect"
        sed 's/^/     | /' "$tmp/out.$n"
        fails=$((fails + 1))
    fi
}

ep_case "ExternalProject + fetch_retry survives a 504"  ON  /e504/1/ep-on/fx.tar.gz  ok
ep_case "ExternalProject without it fails on a 504"     OFF /e504/1/ep-off/fx.tar.gz fail

if [ $fails -ne 0 ]; then
    echo "$fails case(s) failed"
    exit 1
fi
echo "all vendor fetch-retry cases passed"

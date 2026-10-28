#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Tests for scripts/release-tap.sh (make release-tap): the GA-tag guard, the
# alias and checksum checks on the downloaded release assets, the tapper
# invocation, and the post-push check of the published Formula/den.rb.
# gh, tapper and shasum are stubbed on PATH; nothing touches the network.
#
# Run:
#   sh tests/scripts/test_release_tap.sh
# or via ctest (name: script_test_release_tap).

set -u

_SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT="${_SELF_DIR}/../.."
SCRIPT="${ROOT}/scripts/release-tap.sh"
BOTTLES="${ROOT}/scripts/homebrew-bottles.py"

for t in bash python3; do
    if ! command -v "$t" >/dev/null 2>&1; then
        echo "SKIP: $t not found"
        exit 0
    fi
done

_pass=0
_fail=0
_tmp=$(mktemp -d "${TMPDIR:-/tmp}/release-tap-test.XXXXXX")
trap 'rm -rf "$_tmp"' EXIT

ok() { _pass=$((_pass + 1)); echo "PASS: $1"; }
ko() { _fail=$((_fail + 1)); echo "FAIL: $1"; [ -f "$_tmp/out" ] && sed 's/^/    /' "$_tmp/out"; }

# run NAME WANT_EXIT ARGS... — run release-tap.sh with the stubs.
run() {
    _name="$1"
    _want="$2"
    shift 2
    : >"$_tmp/tapper.log"
    : >"$_tmp/gh.log"
    PATH="$_tmp/bin:$PATH" bash "$SCRIPT" "$@" >"$_tmp/out" 2>&1
    _got=$?
    if [ "$_got" -eq "$_want" ]; then ok "$_name"; else ko "$_name (exit $_got, want $_want)"; fi
}

contains() {
    if grep -qF -- "$3" "$2"; then ok "$1"; else ko "$1 (missing: $3)"; fi
}

V=1.2.0
TAG=v$V
TAGS="arm64_golden_gate arm64_tahoe arm64_sequoia arm64_sonoma"
export FAKE_ASSETS="$_tmp/assets" FAKE_FORMULA="$_tmp/tap-den.rb" FAKE_LOG="$_tmp"
export RELEASE_TAP_CHECK_RETRIES=2 RELEASE_TAP_CHECK_DELAY=0
unset KEEP_WORK TAPPER_GITHUB_TOKEN HOMEBREW_TAP_TOKEN 2>/dev/null || true

# --- stubs --------------------------------------------------------------------
mkdir -p "$_tmp/bin"
cat >"$_tmp/bin/gh" <<'SH'
#!/bin/sh
echo "gh $*" >>"$FAKE_LOG/gh.log"
case "$1 $2" in
"release download")
    dir=""
    while [ $# -gt 0 ]; do [ "$1" = --dir ] && dir="$2"; shift; done
    mkdir -p "$dir" && cp "$FAKE_ASSETS"/* "$dir"/ ;;
"api repos/marcelocantos/homebrew-tap/contents/Formula/den.rb")
    [ -f "$FAKE_FORMULA" ] || { echo "HTTP 404: Not Found" >&2; exit 1; }
    cat "$FAKE_FORMULA" ;;
*) echo "gh stub: unexpected: $*" >&2; exit 99 ;;
esac
SH
cat >"$_tmp/bin/tapper" <<'SH'
#!/bin/sh
echo "tapper $*" >>"$FAKE_LOG/tapper.log"
# Keep the rendered config so the test can inspect it.
while [ $# -gt 0 ]; do [ "$1" = --config ] && cp "$2" "$FAKE_LOG/pushed-tapper.yaml"; shift; done
exit 0
SH
# shasum -a 256 -c FILE, done for real in Python (shasum is absent on some runners).
cat >"$_tmp/bin/shasum" <<'SH'
#!/bin/sh
[ "$1 $2 $3" = "-a 256 -c" ] || { echo "shasum stub: unexpected: $*" >&2; exit 99; }
exec python3 - "$4" <<'PY'
import hashlib, sys
bad = 0
for line in open(sys.argv[1]):
    h, name = line.split()
    ok = hashlib.sha256(open(name, "rb").read()).hexdigest() == h
    print(f"{name}: {'OK' if ok else 'FAILED'}")
    bad += not ok
sys.exit(1 if bad else 0)
PY
SH
chmod +x "$_tmp/bin/gh" "$_tmp/bin/tapper" "$_tmp/bin/shasum"

# --- a GA release's assets, as promote-rc leaves them ---------------------------
mkdir -p "$_tmp/stage/share/den" "$FAKE_ASSETS"
printf '#!/bin/sh\necho den\n' >"$_tmp/stage/den"
chmod 755 "$_tmp/stage/den"
echo '{}' >"$_tmp/stage/share/den/known_hashes.json"
(cd "$_tmp/stage" && tar czf "$FAKE_ASSETS/den-$V-darwin-aarch64.tar.gz" den share)
echo linux-x86 >"$FAKE_ASSETS/den-$V-linux-x86_64.tar.gz"
echo linux-arm >"$FAKE_ASSETS/den-$V-linux-aarch64.tar.gz"
python3 "$BOTTLES" build --version "$V" --tarball "$FAKE_ASSETS/den-$V-darwin-aarch64.tar.gz" --out "$FAKE_ASSETS" >/dev/null
rm -f "$FAKE_ASSETS"/*.sha256
cp "$FAKE_ASSETS/den-$V-darwin-aarch64.tar.gz" "$FAKE_ASSETS/den-$V-darwin-arm64.tar.gz"
cp "$FAKE_ASSETS/den-$V-linux-x86_64.tar.gz" "$FAKE_ASSETS/den-$V-linux-amd64.tar.gz"
cp "$FAKE_ASSETS/den-$V-linux-aarch64.tar.gz" "$FAKE_ASSETS/den-$V-linux-arm64.tar.gz"
(cd "$FAKE_ASSETS" && python3 -c '
import hashlib, os
for n in sorted(os.listdir(".")):
    print(hashlib.sha256(open(n, "rb").read()).hexdigest() + "  " + n)
' >"$_tmp/checksums" && mv "$_tmp/checksums" "den-$V-checksums.txt")
cp -R "$FAKE_ASSETS" "$_tmp/good-assets"

# The formula homebrew-releaser publishes for this release (same shape as the
# live tap's den.rb).
bsha=$(python3 -c 'import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$FAKE_ASSETS/den-$V.arm64_tahoe.bottle.tar.gz")
{
    echo 'class Den < Formula'
    echo '  desc "den"'
    echo '  homepage "https://github.com/marcelocantos/den"'
    echo "  url \"https://github.com/marcelocantos/den/archive/refs/tags/$TAG.tar.gz\""
    echo '  sha256 "0000000000000000000000000000000000000000000000000000000000000000"'
    echo '  bottle do'
    echo "    root_url \"https://github.com/marcelocantos/den/releases/download/$TAG\""
    for t in $TAGS; do echo "    sha256 cellar: \"/opt/homebrew/Cellar\", $t: \"$bsha\""; done
    echo '  end'
    echo "  version \"$V\""
    echo 'end'
} >"$_tmp/good-den.rb"

reset_fixtures() {
    rm -rf "$FAKE_ASSETS" && cp -R "$_tmp/good-assets" "$FAKE_ASSETS"
    cp "$_tmp/good-den.rb" "$FAKE_FORMULA"
}

# --- GA-tag guard ---------------------------------------------------------------
reset_fixtures
run "refuses an -rc tag" 1 "$TAG-rc.1"
contains "  says it is not a GA tag" "$_tmp/out" "is not a GA tag"
[ -s "$_tmp/tapper.log" ] && ko "  tapper not run for -rc tag" || ok "  tapper not run for -rc tag"
[ -s "$_tmp/gh.log" ] && ko "  nothing downloaded for -rc tag" || ok "  nothing downloaded for -rc tag"
run "refuses a non-semver tag" 1 v1.2
run "refuses unknown arguments" 2 "$TAG" --force

# --- alias assets ---------------------------------------------------------------
for a in darwin-arm64 linux-amd64 linux-arm64; do
    reset_fixtures
    rm "$FAKE_ASSETS/den-$V-$a.tar.gz"
    run "refuses a release missing den-$V-$a.tar.gz" 1 "$TAG"
    contains "  names the missing alias $a" "$_tmp/out" "lacks den-$V-$a.tar.gz"
    [ -s "$_tmp/tapper.log" ] && ko "  tapper not run without $a" || ok "  tapper not run without $a"
done

# --- checksums ------------------------------------------------------------------
reset_fixtures
echo tampered >>"$FAKE_ASSETS/den-$V-linux-amd64.tar.gz"
run "refuses a checksum mismatch" 1 "$TAG"
contains "  reports the checksum failure" "$_tmp/out" "do not match den-$V-checksums.txt"
[ -s "$_tmp/tapper.log" ] && ko "  tapper not run on checksum mismatch" || ok "  tapper not run on checksum mismatch"

# --- dry run ----------------------------------------------------------------------
reset_fixtures
rm "$FAKE_FORMULA"
run "dry run succeeds" 0 "$TAG" --dry-run
contains "  tapper push --dry-run" "$_tmp/tapper.log" "--version $TAG --dry-run"
grep -q 'contents/Formula' "$_tmp/gh.log" && ko "  dry run skips the post-push check" || ok "  dry run skips the post-push check"
contains "  pushed config carries the bottle block" "$_tmp/pushed-tapper.yaml" "    root_url \"https://github.com/marcelocantos/den/releases/download/$TAG\""
contains "  pushed config uses tapper.yaml" "$_tmp/pushed-tapper.yaml" "repo: marcelocantos/den"

# --- post-push check ------------------------------------------------------------
reset_fixtures
run "push + post-push check passes" 0 "$TAG"
contains "  tapper push (no --dry-run)" "$_tmp/tapper.log" "--version $TAG"
grep -q -- '--dry-run' "$_tmp/tapper.log" && ko "  real push has no --dry-run" || ok "  real push has no --dry-run"
contains "  fetched den.rb from the tap's default branch" "$_tmp/gh.log" "gh api repos/marcelocantos/homebrew-tap/contents/Formula/den.rb"
contains "  reports success" "$_tmp/out" "is $V with bottles"

postpush_fails() {
    run "post-push check fails: $1" 1 "$TAG"
    contains "  reason: $2" "$_tmp/out" "$2"
}

reset_fixtures
sed '/bottle do/,/^  end/d' "$_tmp/good-den.rb" >"$FAKE_FORMULA"
postpush_fails "no bottle block" "missing 'bottle do' block"

reset_fixtures
sed "s|download/$TAG\"|download/v1.1.0\"|" "$_tmp/good-den.rb" >"$FAKE_FORMULA"
postpush_fails "stale root_url" "missing root_url .../download/$TAG"

for t in $TAGS; do
    reset_fixtures
    grep -v "$t:" "$_tmp/good-den.rb" >"$FAKE_FORMULA"
    postpush_fails "no $t bottle" "missing bottle tag $t"
done

reset_fixtures
sed "s|version \"$V\"|version \"1.1.0\"|" "$_tmp/good-den.rb" >"$FAKE_FORMULA"
postpush_fails "old version" "version is not $V"

reset_fixtures
rm "$FAKE_FORMULA"
postpush_fails "fetch fails" "could not fetch Formula/den.rb"
n=$(grep -c 'contents/Formula' "$_tmp/gh.log")
[ "$n" -eq 2 ] && ok "  fetch retried RELEASE_TAP_CHECK_RETRIES times" || ko "  fetch retried RELEASE_TAP_CHECK_RETRIES times (got $n)"

echo ""
echo "Results: ${_pass} passed, ${_fail} failed"
[ "$_fail" -eq 0 ]

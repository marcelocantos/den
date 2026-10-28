#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Tests for scripts/homebrew-bottles.py: bottle tarball layout and receipt,
# reproducibility, verification failures, and the bottle block / den.rb /
# homebrew-releaser inputs rendered from the bottles.
#
# Run:
#   sh tests/scripts/test_homebrew_bottles.sh
# or via ctest (name: script_test_homebrew_bottles).

set -u

_SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT="${_SELF_DIR}/../.."
SCRIPT="${ROOT}/scripts/homebrew-bottles.py"

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not found"
    exit 0
fi

_pass=0
_fail=0
_tmp=$(mktemp -d "${TMPDIR:-/tmp}/homebrew-bottles-test.XXXXXX")
trap 'rm -rf "$_tmp"' EXIT

ok() { _pass=$((_pass + 1)); echo "PASS: $1"; }
ko() { _fail=$((_fail + 1)); echo "FAIL: $1"; [ -f "$_tmp/out" ] && sed 's/^/    /' "$_tmp/out"; }

# check NAME WANT_EXIT ARGS... — run the script, compare exit status.
check() {
    _name="$1"
    _want="$2"
    shift 2
    python3 "$SCRIPT" "$@" >"$_tmp/out" 2>&1
    _got=$?
    if [ "$_got" -eq "$_want" ]; then ok "$_name"; else ko "$_name (exit $_got, want $_want)"; fi
}

# contains NAME FILE STRING — FILE has a line containing STRING verbatim.
contains() {
    if grep -qF -- "$3" "$2"; then ok "$1"; else ko "$1 (missing: $3)"; fi
}

V=1.2.0
TAGS="arm64_golden_gate arm64_tahoe arm64_sequoia arm64_sonoma"

# A release tarball shaped like scripts/package-release.sh output.
mkrelease() {
    _d="$_tmp/stage-$1"
    rm -rf "$_d"
    mkdir -p "$_d/share/den"
    printf '#!/bin/sh\necho %s\n' "$1" >"$_d/den"
    chmod 755 "$_d/den"
    echo '{"sha":"abc"}' >"$_d/share/den/known_hashes.json"
    (cd "$_d" && tar czf "$2" den share)
}

dist="$_tmp/dist"
mkdir -p "$dist"
mkrelease good "$dist/den-$V-darwin-aarch64.tar.gz"
echo linux-x86 >"$dist/den-$V-linux-x86_64.tar.gz"
echo linux-arm >"$dist/den-$V-linux-aarch64.tar.gz"

# --- build ------------------------------------------------------------------
check "build succeeds" 0 build --version "$V" --tarball "$dist/den-$V-darwin-aarch64.tar.gz" --out "$dist"

for t in $TAGS; do
    b="$dist/den-$V.$t.bottle.tar.gz"
    if [ -f "$b" ] && [ -f "$b.sha256" ]; then ok "bottle + sha256 for $t"; else ko "bottle + sha256 for $t"; fi
done
n=0
for f in "$dist"/*.bottle.tar.gz; do [ -f "$f" ] && n=$((n + 1)); done
[ "$n" -eq 4 ] && ok "exactly four bottles (no :all, no x86_64)" || ko "exactly four bottles (got $n)"

# brew's download name uses a single dash (Bottle::Filename#url_encode).
dd=no
for f in "$dist"/*--*; do [ -e "$f" ] && dd=yes; done
[ "$dd" = no ] && ok "no double-dash asset names" || ko "no double-dash asset names"

b="$dist/den-$V.arm64_tahoe.bottle.tar.gz"
tar tzf "$b" | LC_ALL=C sort >"$_tmp/list"
cat >"$_tmp/want" <<LIST
den/
den/$V/
den/$V/INSTALL_RECEIPT.json
den/$V/bin/
den/$V/bin/den
den/$V/share/
den/$V/share/den/
den/$V/share/den/known_hashes.json
LIST
if diff "$_tmp/want" "$_tmp/list" >"$_tmp/out"; then ok "layout is den/<version>/..."; else ko "layout is den/<version>/..."; fi

mkdir -p "$_tmp/x"
tar xzf "$b" -C "$_tmp/x"
"$_tmp/x/den/$V/bin/den" | grep -q good && ok "bin/den is the release binary" || ko "bin/den is the release binary"
[ -x "$_tmp/x/den/$V/bin/den" ] && ok "bin/den is executable" || ko "bin/den is executable"

python3 - "$_tmp/x/den/$V/INSTALL_RECEIPT.json" "$V" >"$_tmp/out" 2>&1 <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
assert r["built_as_bottle"] is True, r
assert r["poured_from_bottle"] is False, r
assert r["changed_files"] == [], r
assert r["time"] is None, r
assert r["arch"] == "arm64", r
assert r["source"]["spec"] == "stable", r
assert r["source"]["versions"]["stable"] == sys.argv[2], r
PY
[ $? -eq 0 ] && ok "receipt fields (pre-pour)" || ko "receipt fields (pre-pour)"

# sha256 files use the "<hex>  <name>" format of the other release assets.
read -r h f <"$b.sha256"
[ "$f" = "den-$V.arm64_tahoe.bottle.tar.gz" ] && [ ${#h} -eq 64 ] && ok "sha256 file format" || ko "sha256 file format"

# Reproducible: rebuilding gives identical bytes.
mkdir -p "$_tmp/again"
python3 "$SCRIPT" build --version "$V" --tarball "$dist/den-$V-darwin-aarch64.tar.gz" --out "$_tmp/again" >/dev/null 2>&1
cmp -s "$b" "$_tmp/again/den-$V.arm64_tahoe.bottle.tar.gz" && ok "build is reproducible" || ko "build is reproducible"

check "rejects -rc version" 1 build --version "$V-rc.2" --tarball "$dist/den-$V-darwin-aarch64.tar.gz" --out "$_tmp/rc"
check "rejects v prefix" 1 build --version "v$V" --tarball "$dist/den-$V-darwin-aarch64.tar.gz" --out "$_tmp/rc"
mkdir -p "$_tmp/nobin" && echo x >"$_tmp/nobin/other" && (cd "$_tmp/nobin" && tar czf ../nobin.tar.gz other)
check "rejects tarball without den" 1 build --version "$V" --tarball "$_tmp/nobin.tar.gz" --out "$_tmp/rc"

# --- verify -----------------------------------------------------------------
cat "$dist"/*.sha256 >"$_tmp/checksums.txt"
check "verify passes" 0 verify --version "$V" --tarball "$dist/den-$V-darwin-aarch64.tar.gz" --dir "$dist" --checksums "$_tmp/checksums.txt"

mkrelease other "$_tmp/other.tar.gz"
check "verify rejects a different binary" 1 verify --version "$V" --tarball "$_tmp/other.tar.gz" --dir "$dist"
check "verify rejects version mismatch" 1 verify --version 1.3.0 --tarball "$dist/den-$V-darwin-aarch64.tar.gz" --dir "$dist"

sed 's/^[0-9a-f]\{8\}/00000000/' "$_tmp/checksums.txt" >"$_tmp/bad-checksums.txt"
check "verify rejects checksum mismatch" 1 verify --version "$V" --tarball "$dist/den-$V-darwin-aarch64.tar.gz" --dir "$dist" --checksums "$_tmp/bad-checksums.txt"

cp -R "$dist" "$_tmp/partial"
rm "$_tmp/partial/den-$V.arm64_sonoma.bottle.tar.gz"
check "verify rejects a missing tag" 1 verify --version "$V" --tarball "$dist/den-$V-darwin-aarch64.tar.gz" --dir "$_tmp/partial"

# --- bottle block -----------------------------------------------------------
check "block renders" 0 block --version "$V" --release-tag "v$V-rc.3" --dir "$dist"
cp "$_tmp/out" "$_tmp/block"
sha=$(cut -d' ' -f1 "$b.sha256")
contains "block root_url is the release download URL" "$_tmp/block" "root_url \"https://github.com/marcelocantos/den/releases/download/v$V-rc.3\""
for t in $TAGS; do
    contains "block sha256 for $t" "$_tmp/block" "sha256 cellar: \"/opt/homebrew/Cellar\", $t: \"$sha\""
done
grep -q ':all\|x86_64\|:any' "$_tmp/block" && ko "block has no :all/:any/x86_64" || ok "block has no :all/:any/x86_64"
[ "$(head -1 "$_tmp/block")" = "bottle do" ] && [ "$(tail -1 "$_tmp/block")" = "end" ] && ok "block is a bottle do ... end" || ko "block is a bottle do ... end"
check "block fails without bottles" 1 block --version "$V" --release-tag "v$V" --dir "$_tmp/partial"

# --- RC den.rb --------------------------------------------------------------
check "formula renders" 0 formula --version "$V" --release-tag "v$V-rc.3" --dir "$dist"
cp "$_tmp/out" "$_tmp/den.rb"
contains "formula version is X.Y.Z (rc-free)" "$_tmp/den.rb" "  version \"$V\""
contains "formula has bottle block" "$_tmp/den.rb" "  bottle do"
contains "formula bottle root_url" "$_tmp/den.rb" "    root_url \"https://github.com/marcelocantos/den/releases/download/v$V-rc.3\""
contains "formula keeps libarchive dep" "$_tmp/den.rb" '    depends_on "libarchive"'
contains "formula keeps service" "$_tmp/den.rb" '    run [opt_bin/"den", "daemon", "run"]'
darwin_sha=$(python3 -c 'import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$dist/den-$V-darwin-aarch64.tar.gz")
contains "formula darwin url sha" "$_tmp/den.rb" "sha256 \"$darwin_sha\""
contains "formula darwin url" "$_tmp/den.rb" "url \"https://github.com/marcelocantos/den/releases/download/v$V-rc.3/den-$V-darwin-aarch64.tar.gz\""
if command -v ruby >/dev/null 2>&1; then
    ruby -c "$_tmp/den.rb" >"$_tmp/out" 2>&1 && ok "formula is valid Ruby" || ko "formula is valid Ruby"
fi

# --- homebrew-releaser inputs (GA) ------------------------------------------
gho="$_tmp/github_output"
: >"$gho"
check "releaser-inputs writes GITHUB_OUTPUT" 0 releaser-inputs --version "$V" --release-tag "v$V" --dir "$dist" --github-output "$gho"
contains "releaser formula_includes key" "$gho" "formula_includes<<EOF_"
contains "releaser install key" "$gho" "install<<EOF_"
contains "releaser test key" "$gho" "test<<EOF_"
contains "releaser bottle root_url is the GA release" "$gho" "  root_url \"https://github.com/marcelocantos/den/releases/download/v$V\""
contains "releaser includes keep libarchive dep" "$gho" '  depends_on "libarchive"'
contains "releaser install" "$gho" 'bin.install "den"'

# The GA formula and the RC formula carry the same bottle shas.
grep 'sha256 cellar:' "$gho" | sed 's/^ *//' >"$_tmp/ga-shas"
grep 'sha256 cellar:' "$_tmp/den.rb" | sed 's/^ *//' >"$_tmp/rc-shas"
cmp -s "$_tmp/ga-shas" "$_tmp/rc-shas" && ok "RC and GA bottle shas match" || ko "RC and GA bottle shas match"

# --- tapper-config (GA, scripts/release-tap.sh) ------------------------------
base="$ROOT/tapper.yaml"
check "tapper-config renders" 0 tapper-config --version "$V" --release-tag "v$V" --dir "$dist" --base "$base" --out "$_tmp/tc/tapper.yaml"
tc="$_tmp/tc/tapper.yaml"
contains "tapper-config keeps the base config" "$tc" "repo: marcelocantos/den"
contains "tapper-config adds formula_includes" "$tc" "formula_includes: |"
contains "tapper-config bottle block" "$tc" "  bottle do"
contains "tapper-config root_url is the GA release" "$tc" "    root_url \"https://github.com/marcelocantos/den/releases/download/v$V\""
for t in $TAGS; do
    contains "tapper-config sha256 for $t" "$tc" "    sha256 cellar: \"/opt/homebrew/Cellar\", $t: \"$sha\""
done
contains "tapper-config keeps libarchive dep" "$tc" '    depends_on "libarchive"'
contains "tapper-config keeps service" "$tc" '    run [opt_bin/"den", "daemon", "run"]'
# The rendered includes are exactly what homebrew-releaser got from promote-rc.
python3 - "$tc" "$gho" >"$_tmp/out" 2>&1 <<'PY2'
import re, sys
tc = open(sys.argv[1]).read()
inc = tc.split("formula_includes: |\n", 1)[1]
inc = "".join(l[2:] if l.strip() else l for l in inc.splitlines(True)).rstrip()
gho = open(sys.argv[2]).read()
m = re.search(r"^formula_includes<<(EOF_\w+)\n(.*?)\n\1$", gho, re.S | re.M)
assert m and m.group(2) == inc, (inc, m and m.group(2))
PY2
[ $? -eq 0 ] && ok "tapper-config includes == releaser-inputs formula_includes" || ko "tapper-config includes == releaser-inputs formula_includes"
if command -v ruby >/dev/null 2>&1; then
    ruby -ryaml -e 'c = YAML.load_file(ARGV[0]); abort "bad" unless c["formula_includes"].start_with?("bottle do\n") && c["install"].include?("bin.install \"den\"")' "$tc" >"$_tmp/out" 2>&1 \
        && ok "tapper-config is valid YAML" || ko "tapper-config is valid YAML"
fi

{ cat "$base"; printf 'formula_includes: |\n  depends_on "x"\n'; } >"$_tmp/has-includes.yaml"
check "tapper-config rejects a base with formula_includes" 1 tapper-config --version "$V" --release-tag "v$V" --dir "$dist" --base "$_tmp/has-includes.yaml" --out "$_tmp/tc/x.yaml"
{ cat "$base"; printf 'formula_includes_file: inc.rb\n'; } >"$_tmp/has-includes-file.yaml"
check "tapper-config rejects a base with formula_includes_file" 1 tapper-config --version "$V" --release-tag "v$V" --dir "$dist" --base "$_tmp/has-includes-file.yaml" --out "$_tmp/tc/x.yaml"
sed 's/bin.install "den"/bin.install "den2"/' "$base" >"$_tmp/install-drift.yaml"
check "tapper-config rejects install drift" 1 tapper-config --version "$V" --release-tag "v$V" --dir "$dist" --base "$_tmp/install-drift.yaml" --out "$_tmp/tc/x.yaml"
sed 's/No packages installed/No packages/' "$base" >"$_tmp/test-drift.yaml"
check "tapper-config rejects test drift" 1 tapper-config --version "$V" --release-tag "v$V" --dir "$dist" --base "$_tmp/test-drift.yaml" --out "$_tmp/tc/x.yaml"
check "tapper-config rejects -rc version" 1 tapper-config --version "$V-rc.1" --release-tag "v$V-rc.1" --dir "$dist" --base "$base" --out "$_tmp/tc/x.yaml"
check "tapper-config fails without bottles" 1 tapper-config --version "$V" --release-tag "v$V" --dir "$_tmp/partial" --base "$base" --out "$_tmp/tc/x.yaml"

# --- workflow wiring --------------------------------------------------------
wf="$ROOT/.github/workflows"
contains "RC workflow builds bottles" "$wf/release-candidate.yml" "scripts/homebrew-bottles.py build"
contains "RC workflow verifies bottles" "$wf/release-candidate.yml" "scripts/homebrew-bottles.py verify"
contains "RC workflow renders den.rb" "$wf/release-candidate.yml" "scripts/homebrew-bottles.py formula"
contains "promote verifies RC bottles" "$wf/promote-rc.yml" "scripts/homebrew-bottles.py verify"
contains "promote uploads Homebrew-named aliases" "$wf/promote-rc.yml" "brew-assets/den-\${VERSION}-darwin-arm64.tar.gz"
grep -q 'HOMEBREW_TAP_TOKEN\|homebrew-releaser@' "$wf/promote-rc.yml" \
    && ko "promote no longer bumps the tap from CI (no tap token in Actions)" \
    || ok "promote no longer bumps the tap from CI (no tap token in Actions)"
contains "release-tap renders the per-release tapper config" "$ROOT/scripts/release-tap.sh" "scripts/homebrew-bottles.py tapper-config"

echo ""
echo "Results: ${_pass} passed, ${_fail} failed"
[ "$_fail" -eq 0 ]

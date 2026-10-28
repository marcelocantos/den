#!/usr/bin/env bash
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Publish Formula/den.rb to marcelocantos/homebrew-tap for an existing GA
# release, from the dev Mac (tapper runs homebrew-releaser in Docker and
# reads its token from the keychain; no Actions secret).
#
# Run after promote-rc has published the GA release, including its
# Homebrew-named aliases (den-<ver>-darwin-arm64 / linux-amd64 /
# linux-arm64 .tar.gz). The bottle block is rendered fresh for this
# release from the real assets, after the same bottle verification
# promote-rc does, so the formula never ships without bottles. After a
# real push, the published Formula/den.rb is fetched back and checked.
#
#   scripts/release-tap.sh [vX.Y.Z] [--dry-run]
#
# KEEP_WORK=1 keeps the scratch directory (assets + rendered config).
set -euo pipefail
cd "$(dirname "$0")/.."

TAP_REPO=marcelocantos/homebrew-tap
TAGS=(arm64_golden_gate arm64_tahoe arm64_sequoia arm64_sonoma)
RETRIES="${RELEASE_TAP_CHECK_RETRIES:-5}"
DELAY="${RELEASE_TAP_CHECK_DELAY:-3}"

TAG="" DRY=()
for arg in "$@"; do
	case "$arg" in
	--dry-run) DRY=(--dry-run) ;;
	v*) TAG="$arg" ;;
	*) echo "usage: $0 [vX.Y.Z] [--dry-run]" >&2; exit 2 ;;
	esac
done
TAG="${TAG:-$(git describe --tags --abbrev=0 --match 'v[0-9]*.[0-9]*.[0-9]*' --exclude '*-rc.*')}"
if ! [[ "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
	echo "release-tap: $TAG is not a GA tag (vX.Y.Z)" >&2
	exit 1
fi
VERSION="${TAG#v}"

command -v tapper >/dev/null || {
	echo "tapper not on PATH — brew install marcelocantos/tap/tapper" >&2
	exit 1
}

work="$(mktemp -d)"
trap '[ -n "${KEEP_WORK:-}" ] && echo "release-tap: kept $work" >&2 || rm -rf "$work"' EXIT

gh release download "$TAG" --repo marcelocantos/den --dir "$work/assets" \
	--pattern "den-${VERSION}-*.tar.gz" \
	--pattern "den-${VERSION}.*.bottle.tar.gz" \
	--pattern "den-${VERSION}-checksums.txt"

for alias in darwin-arm64 linux-amd64 linux-arm64; do
	[ -f "$work/assets/den-${VERSION}-${alias}.tar.gz" ] || {
		echo "release-tap: $TAG lacks den-${VERSION}-${alias}.tar.gz (promote-rc uploads it)" >&2
		exit 1
	}
done

(cd "$work/assets" && shasum -a 256 -c "den-${VERSION}-checksums.txt") || {
	echo "release-tap: $TAG assets do not match den-${VERSION}-checksums.txt" >&2
	exit 1
}
python3 scripts/homebrew-bottles.py verify \
	--version "$VERSION" \
	--tarball "$work/assets/den-${VERSION}-darwin-aarch64.tar.gz" \
	--dir "$work/assets" \
	--checksums "$work/assets/den-${VERSION}-checksums.txt"
python3 scripts/homebrew-bottles.py tapper-config \
	--version "$VERSION" --release-tag "$TAG" \
	--dir "$work/assets" --base tapper.yaml --out "$work/tapper.yaml"

tapper push --config "$work/tapper.yaml" --version "$TAG" ${DRY[@]+"${DRY[@]}"}

[ ${#DRY[@]} -eq 0 ] || exit 0

# Post-push check: the formula on the tap's default branch must be this
# release's, with its bottle block. Retried briefly in case the contents
# API lags the push.
check_formula() {
	local f="$1" t
	grep -qx '  bottle do' "$f" || { echo "missing 'bottle do' block"; return 1; }
	grep -qF "root_url \"https://github.com/marcelocantos/den/releases/download/${TAG}\"" "$f" ||
		{ echo "missing root_url .../download/${TAG}"; return 1; }
	for t in "${TAGS[@]}"; do
		grep -qE "^ +sha256 cellar: \"/opt/homebrew/Cellar\", ${t}: \"[0-9a-f]{64}\"\$" "$f" ||
			{ echo "missing bottle tag ${t}"; return 1; }
	done
	grep -qx "  version \"${VERSION}\"" "$f" || { echo "version is not ${VERSION}"; return 1; }
}

why=""
for ((i = 1; i <= RETRIES; i++)); do
	if gh api "repos/${TAP_REPO}/contents/Formula/den.rb" \
		-H 'Accept: application/vnd.github.raw' >"$work/den.rb" 2>"$work/fetch.err"; then
		if why=$(check_formula "$work/den.rb"); then
			echo "release-tap: ${TAP_REPO} Formula/den.rb is ${VERSION} with bottles (${TAGS[*]})"
			exit 0
		fi
	else
		why="could not fetch Formula/den.rb: $(head -1 "$work/fetch.err")"
	fi
	[ "$i" -lt "$RETRIES" ] && sleep "$DELAY"
done
echo "release-tap: post-push check FAILED for ${TAP_REPO} Formula/den.rb: ${why}" >&2
exit 1

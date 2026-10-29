#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Guard: den is not released or distributed through Homebrew. Fails if any
# workflow under .github/workflows, the Makefile or scripts/verify-release.sh
# (whose tap step uses a fixture tap from this repo) mentions tapper,
# release-tap, den's own bottles (homebrew-bottles, *.bottle.tar.gz, a
# `bottle do` block) or the marcelocantos tap.
#
# This only covers den's own release path. den still pours Homebrew bottles
# and reads taps as a package manager; that code lives in src/ and is not
# checked here.
#
#   scripts/check-no-brew-release.sh [repo-root]
#
# Exit 0 when clean, 1 with one "file:line: text" per hit otherwise.
set -u

root="${1:-$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)}"
pattern='tapper|release-tap|marcelocantos/(homebrew-)?tap|homebrew-bottles|\.bottle\.tar\.gz|bottle do([[:space:]]|$)'

files=""
for f in "$root"/.github/workflows/*.yml "$root"/.github/workflows/*.yaml "$root"/Makefile \
    "$root"/scripts/verify-release.sh; do
    [ -f "$f" ] && files="$files
$f"
done
if [ -z "$files" ]; then
    echo "check-no-brew-release: no workflows or Makefile under $root" >&2
    exit 2
fi

hits=$(printf '%s\n' "$files" | sed '/^$/d' | while IFS= read -r f; do
    grep -n -i -E "$pattern" "$f" | sed "s|^|${f#"$root"/}:|"
done)

if [ -n "$hits" ]; then
    echo "check-no-brew-release: den's Homebrew release path is retired; remove:" >&2
    printf '%s\n' "$hits" >&2
    exit 1
fi
echo "check-no-brew-release: OK"

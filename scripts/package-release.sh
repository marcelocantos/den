#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Package a built den into a release tarball:
#
#   den                          the binary (top level, as install.sh and
#                                `den self-update` expect)
#   share/den/known_hashes.json  the trust replica snapshot (🎯T42), which den
#                                resolves at <bin>/../share/den/ once installed
#                                or <dir>/share/den/ when run from the unpacked
#                                archive
#
# Usage: scripts/package-release.sh <build-dir> <archive.tar.gz>
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <build-dir> <archive.tar.gz>" >&2
    exit 2
fi
build_dir=$1
archive=$2
repo_root=$(cd "$(dirname "$0")/.." && pwd)

[ -x "$build_dir/den" ] || { echo "error: $build_dir/den not found" >&2; exit 1; }
replica="$repo_root/data/known_hashes.json"
[ -f "$replica" ] || { echo "error: $replica not found" >&2; exit 1; }

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/share/den"
cp "$build_dir/den" "$stage/den"
cp "$replica" "$stage/share/den/known_hashes.json"

case $archive in
/*) out=$archive ;;
*) out="$(pwd)/$archive" ;;
esac
tar czf "$out" -C "$stage" den share

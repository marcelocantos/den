#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Pick the tag that release notes for VERSION should diff against.
#
# Usage:
#   <tag list> | scripts/prev-release-tag.sh VERSION
#   e.g. git tag -l 'v*' | scripts/prev-release-tag.sh 0.14.0
#
# Reads one tag per line on stdin and prints the highest FINAL release tag
# (vX.Y.Z, no pre-release suffix) that is strictly below VERSION. Release
# candidates (-rc.N), other pre-release or malformed tags, VERSION itself,
# and anything above VERSION (e.g. a stray v1.0.0-rc.1) are ignored.
#
# VERSION may carry a leading "v" and/or a "-rc.N" suffix; only X.Y.Z is
# used. Prints nothing (and exits 0) when there is no earlier final tag.
# Exits 2 on a malformed VERSION.

set -u

if [ $# -ne 1 ]; then
    echo "usage: <tags> | $0 VERSION" >&2
    exit 2
fi

target=$(printf '%s' "$1" | sed -e 's/^v//' -e 's/-rc\.[0-9][0-9]*$//')
if ! printf '%s\n' "$target" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo "ERROR: '$1' is not a version of the form [v]X.Y.Z[-rc.N]" >&2
    exit 2
fi

tr -d '\r' | awk -v target="$target" '
function cmp(a, b,    x, y, i) {
    split(a, x, ".")
    split(b, y, ".")
    for (i = 1; i <= 3; i++) {
        if (x[i] + 0 < y[i] + 0) return -1
        if (x[i] + 0 > y[i] + 0) return 1
    }
    return 0
}
{
    sub(/^[ \t]+/, ""); sub(/[ \t]+$/, "")
    if ($0 !~ /^v[0-9]+\.[0-9]+\.[0-9]+$/) next
    v = substr($0, 2)
    if (cmp(v, target) >= 0) next
    if (best == "" || cmp(v, best) > 0) best = v
}
END { if (best != "") print "v" best }
'

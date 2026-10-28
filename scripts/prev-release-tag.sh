#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Pick the tag that release notes for VERSION should diff against.
#
# Usage:
#   <tag list> | scripts/prev-release-tag.sh VERSION
#   <tag list> | scripts/prev-release-tag.sh --all VERSION
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
#
# --all prints every release tag, final AND release candidate, strictly
# below VERSION (including its -rc.N, if any), newest first, one per line.
# Ordering is X.Y.Z, then -rc.N numerically, with vX.Y.Z above all of its
# own RCs: for v0.15.0-rc.3 that is v0.15.0-rc.2, v0.15.0-rc.1, v0.14.1, ...
# verify-release's self-update step walks this list for the newest earlier
# release whose binary can `self-update --version`.

set -u

all=0
if [ "${1:-}" = --all ]; then
    all=1
    shift
fi
if [ $# -ne 1 ]; then
    echo "usage: <tags> | $0 [--all] VERSION" >&2
    exit 2
fi

if ! printf '%s\n' "$1" | grep -Eq '^v?[0-9]+\.[0-9]+\.[0-9]+(-rc\.[0-9]+)?$'; then
    echo "ERROR: '$1' is not a version of the form [v]X.Y.Z[-rc.N]" >&2
    exit 2
fi
target=$(printf '%s' "$1" | sed -e 's/^v//' -e 's/-rc\.[0-9][0-9]*$//')

if [ "$all" = 1 ]; then
    # Sort key: X Y Z R, where R is the RC number or a huge value for a final
    # release (so vX.Y.Z sorts above its own RCs).
    tr -d '\r' | awk -v t="$1" '
    function key(tag,    v, rc, p) {
        sub(/^v/, "", tag)
        rc = 999999999
        if (tag ~ /-rc\.[0-9]+$/) {
            rc = substr(tag, index(tag, "-rc.") + 4) + 0
            tag = substr(tag, 1, index(tag, "-rc.") - 1)
        }
        split(tag, p, ".")
        return sprintf("%09d %09d %09d %09d", p[1], p[2], p[3], rc)
    }
    BEGIN { tk = key(t) }
    {
        sub(/^[ \t]+/, ""); sub(/[ \t]+$/, "")
        if ($0 !~ /^v[0-9]+\.[0-9]+\.[0-9]+(-rc\.[0-9]+)?$/) next
        k = key($0)
        if (k >= tk || ($0 in seen)) next
        seen[$0] = 1
        print k "\t" $0
    }' | LC_ALL=C sort -r | cut -f2
    exit 0
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

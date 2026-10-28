#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Look up a tag by its exact name via the GitHub API.
#
# Usage:
#   scripts/tag-ref.sh OWNER/REPO TAG
#   e.g. scripts/tag-ref.sh "$GITHUB_REPOSITORY" v0.14.0
#
# Exit status and output:
#   0  the tag exists; prints the commit SHA it points to (annotated tags
#      are peeled to their commit)
#   1  the tag does not exist (the API answered HTTP 404)
#   2  anything else: bad usage, an API or network error, or a response
#      that is not an exact match for refs/tags/TAG
#
# Uses the singular git/ref/tags/TAG endpoint. The plural git/refs/tags/TAG
# endpoint falls back to a prefix match when there is no exact ref, so
# v0.14.0 "exists" while only v0.14.0-rc.* do, and rc.1 can match rc.10.
# Only a 404 means "missing"; callers must treat exit 2 as a hard failure
# rather than as "missing" or "present".

set -u

if [ $# -ne 2 ] || [ -z "$1" ] || [ -z "$2" ]; then
    echo "usage: $0 OWNER/REPO TAG" >&2
    exit 2
fi

repo="$1"
tag="$2"

_err=$(mktemp "${TMPDIR:-/tmp}/tag-ref.XXXXXX") || exit 2
trap 'rm -f "$_err"' EXIT

# api PATH JQ: run gh api; on failure exit 1 for a 404 and 2 otherwise.
api() {
    if _out=$(gh api "$1" --jq "$2" 2>"$_err"); then
        return 0
    fi
    if grep -q 'HTTP 404' "$_err"; then
        return 1
    fi
    echo "ERROR: gh api $1 failed:" >&2
    sed 's/^/    /' "$_err" >&2
    exit 2
}

if ! api "repos/$repo/git/ref/tags/$tag" '"\(.ref) \(.object.type) \(.object.sha)"'; then
    exit 1
fi

# shellcheck disable=SC2086
set -- $_out
if [ $# -ne 3 ] || [ "$1" != "refs/tags/$tag" ]; then
    echo "ERROR: lookup of refs/tags/$tag returned an unexpected ref: '$_out'" >&2
    exit 2
fi
type="$2"
sha="$3"

# Peel annotated tags (possibly nested) down to the commit.
_depth=0
while [ "$type" = "tag" ]; do
    _depth=$((_depth + 1))
    if [ "$_depth" -gt 10 ]; then
        echo "ERROR: tag $tag: too many nested tag objects" >&2
        exit 2
    fi
    if ! api "repos/$repo/git/tags/$sha" '"\(.object.type) \(.object.sha)"'; then
        echo "ERROR: tag $tag: tag object $sha not found" >&2
        exit 2
    fi
    # shellcheck disable=SC2086
    set -- $_out
    type="${1:-}"
    sha="${2:-}"
done

if [ "$type" != "commit" ]; then
    echo "ERROR: tag $tag points to a $type, not a commit" >&2
    exit 2
fi
if ! printf '%s\n' "$sha" | grep -Eq '^[0-9a-f]{40}$'; then
    echo "ERROR: tag $tag: malformed object SHA '$sha'" >&2
    exit 2
fi

printf '%s\n' "$sha"

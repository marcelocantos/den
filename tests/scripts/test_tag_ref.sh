#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Regression tests for scripts/tag-ref.sh: tag lookups in the release
# workflows must match the exact tag name. GitHub's plural git/refs/tags/X
# endpoint falls back to a prefix match, which made promote-rc think
# v0.14.0 existed when only v0.14.0-rc.* did. Only an HTTP 404 may count
# as "missing"; any other failure must be an error.
#
# A fake `gh` on PATH serves tags from a fixture file and logs every call.
#
# Run:
#   sh tests/scripts/test_tag_ref.sh
# or via ctest (name: script_test_tag_ref).

set -u

_SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
SCRIPT="${_SELF_DIR}/../../scripts/tag-ref.sh"

_pass=0
_fail=0
_tmp=$(mktemp -d "${TMPDIR:-/tmp}/tag-ref-test.XXXXXX")
trap 'rm -rf "$_tmp"' EXIT

mkdir -p "$_tmp/bin"
# Fake gh. Fixtures:
#   $FAKE_REFS : lines "TAG TYPE SHA"         (refs/tags/TAG -> object)
#   $FAKE_OBJS : lines "SHA TYPE TARGET_SHA"  (annotated tag objects)
# Modes:
#   FAKE_FAIL="MSG"  every call fails with MSG on stderr (exit 1)
#   FAKE_PREFIX=1    ref lookups fall back to a prefix match, like the
#                    plural git/refs endpoint
# The --jq argument is ignored; output is what tag-ref.sh's filter yields.
cat >"$_tmp/bin/gh" <<'FAKE'
#!/bin/sh
echo "$*" >>"$FAKE_LOG"
[ "$1" = "api" ] || { echo "fake gh: unexpected: $*" >&2; exit 99; }
if [ -n "${FAKE_FAIL:-}" ]; then
    echo "$FAKE_FAIL" >&2
    exit 1
fi
path="$2"
notfound() {
    echo '{"message":"Not Found","status":"404"}'
    echo "gh: Not Found (HTTP 404)" >&2
    exit 1
}
case "$path" in
    repos/*/git/ref/tags/*)
        want="${path#repos/*/git/ref/tags/}"
        while read -r t ty sha; do
            if [ "$t" = "$want" ]; then
                echo "refs/tags/$t $ty $sha"; exit 0
            fi
        done <"$FAKE_REFS"
        if [ "${FAKE_PREFIX:-0}" = 1 ]; then
            while read -r t ty sha; do
                case "$t" in "$want"*)
                    echo "refs/tags/$t $ty $sha"; exit 0 ;;
                esac
            done <"$FAKE_REFS"
        fi
        notfound ;;
    repos/*/git/tags/*)
        want="${path#repos/*/git/tags/}"
        while read -r sha ty target; do
            if [ "$sha" = "$want" ]; then
                echo "$ty $target"; exit 0
            fi
        done <"$FAKE_OBJS"
        notfound ;;
    *)
        echo "fake gh: unexpected path $path" >&2; exit 99 ;;
esac
FAKE
chmod +x "$_tmp/bin/gh"

FAKE_REFS="$_tmp/refs"
FAKE_OBJS="$_tmp/objs"
FAKE_LOG="$_tmp/log"
export FAKE_REFS FAKE_OBJS FAKE_LOG
PATH="$_tmp/bin:$PATH"
export PATH

sha() { printf '%040d\n' "$1" | tr 0 "$2"; }
RC1=$(sha 0 1); RC2=$(sha 0 2); RC3=$(sha 0 3); RC10=$(sha 0 a)
GA=$(sha 0 b); TAGOBJ=$(sha 0 c); TAGOBJ2=$(sha 0 d); TREE=$(sha 0 e)

refs() { printf '%s\n' "$@" >"$FAKE_REFS"; }
: >"$FAKE_OBJS"

# expect NAME WANT_EXIT WANT_STDOUT TAG
expect() {
    _name="$1"; _want_rc="$2"; _want_out="$3"; _tag="$4"
    : >"$FAKE_LOG"
    _got=$(sh "$SCRIPT" owner/repo "$_tag" 2>"$_tmp/err")
    _rc=$?
    if [ "$_rc" -eq "$_want_rc" ] && [ "$_got" = "$_want_out" ]; then
        _pass=$((_pass + 1))
        echo "PASS: $_name"
    else
        _fail=$((_fail + 1))
        echo "FAIL: $_name (exit $_rc, want $_want_rc; got '$_got', want '$_want_out')"
        sed 's/^/    /' "$_tmp/err"
    fi
}

# --- rc.1 vs rc.10 ---
refs "v0.14.0-rc.1 commit $RC1" "v0.14.0-rc.10 commit $RC10"
expect "rc.1 resolves to rc.1, not rc.10"   0 "$RC1"  v0.14.0-rc.1
expect "rc.10 resolves to rc.10"            0 "$RC10" v0.14.0-rc.10
refs "v0.14.0-rc.10 commit $RC10"
expect "rc.1 missing while rc.10 exists"    1 ""      v0.14.0-rc.1

# --- GA vs its RCs (the promote-rc failure) ---
refs "v0.14.0-rc.1 commit $RC1" "v0.14.0-rc.2 commit $RC2" "v0.14.0-rc.3 commit $RC3"
expect "GA missing while only RCs exist"    1 ""      v0.14.0
expect "rc.3 resolves among RCs"            0 "$RC3"  v0.14.0-rc.3
refs "v0.14.0-rc.3 commit $RC3" "v0.14.0 commit $GA"
expect "GA present alongside RCs"           0 "$GA"   v0.14.0
refs
expect "no tags at all"                     1 ""      v0.14.0

# The lookup must use the singular endpoint.
refs "v0.14.0-rc.3 commit $RC3"
: >"$FAKE_LOG"
sh "$SCRIPT" owner/repo v0.14.0-rc.3 >/dev/null 2>&1
if grep -q '^api repos/owner/repo/git/ref/tags/v0.14.0-rc.3 ' "$FAKE_LOG" \
    && ! grep -q 'git/refs/' "$FAKE_LOG"; then
    _pass=$((_pass + 1)); echo "PASS: uses singular git/ref endpoint"
else
    _fail=$((_fail + 1)); echo "FAIL: uses singular git/ref endpoint"
    sed 's/^/    /' "$FAKE_LOG"
fi

# A prefix-matching answer (plural-endpoint behaviour) must be rejected,
# not taken as "present".
refs "v0.14.0-rc.1 commit $RC1"
FAKE_PREFIX=1; export FAKE_PREFIX
expect "prefix-match response is an error"  2 ""      v0.14.0
unset FAKE_PREFIX

# --- annotated tags are peeled to the commit ---
refs "v0.14.0-rc.3 tag $TAGOBJ"
printf '%s\n' "$TAGOBJ commit $RC3" >"$FAKE_OBJS"
expect "annotated tag peeled"               0 "$RC3"  v0.14.0-rc.3
refs "v0.14.0-rc.3 tag $TAGOBJ2"
printf '%s\n' "$TAGOBJ2 tag $TAGOBJ" "$TAGOBJ commit $RC3" >"$FAKE_OBJS"
expect "nested annotated tag peeled"        0 "$RC3"  v0.14.0-rc.3
refs "v0.14.0-rc.3 tag $TAGOBJ"
: >"$FAKE_OBJS"
expect "dangling tag object is an error"    2 ""      v0.14.0-rc.3
refs "v0.14.0-rc.3 tree $TREE"
expect "tag pointing at a tree is an error" 2 ""      v0.14.0-rc.3
refs "v0.14.0-rc.3 commit not-a-sha"
expect "malformed SHA is an error"          2 ""      v0.14.0-rc.3

# --- only 404 means missing; every other failure is an error ---
refs "v0.14.0-rc.3 commit $RC3"
for msg in "gh: Server Error (HTTP 500)" "gh: Bad credentials (HTTP 401)" \
           "gh: Resource not accessible by integration (HTTP 403)" \
           "gh: API rate limit exceeded (HTTP 429)" \
           "error connecting to api.github.com"; do
    FAKE_FAIL="$msg"; export FAKE_FAIL
    expect "error is not 'missing': $msg"   2 ""      v0.14.0
    expect "error is not 'present': $msg"   2 ""      v0.14.0-rc.3
done
unset FAKE_FAIL

# --- usage ---
for args in "" "owner/repo" "owner/repo v1 extra"; do
    # shellcheck disable=SC2086
    if sh "$SCRIPT" $args >/dev/null 2>&1; then _rc=0; else _rc=$?; fi
    if [ "$_rc" -eq 2 ]; then
        _pass=$((_pass + 1)); echo "PASS: usage error for '$args'"
    else
        _fail=$((_fail + 1)); echo "FAIL: usage error for '$args' (exit $_rc)"
    fi
done
if sh "$SCRIPT" owner/repo "" >/dev/null 2>&1; then _rc=0; else _rc=$?; fi
if [ "$_rc" -eq 2 ]; then
    _pass=$((_pass + 1)); echo "PASS: empty tag rejected"
else
    _fail=$((_fail + 1)); echo "FAIL: empty tag rejected (exit $_rc)"
fi

echo ""
echo "tag-ref: ${_pass} passed, ${_fail} failed"
[ "$_fail" -eq 0 ]

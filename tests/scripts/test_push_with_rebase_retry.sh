#!/bin/sh
# Regression test for scripts/push-with-rebase-retry.sh against a local bare
# remote (no network): a push rejected because the branch moved is rebased
# and retried; a real rebase conflict fails, aborts the rebase and pushes
# nothing; an up-to-date branch pushes first time.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SCRIPT="$ROOT/scripts/push-with-rebase-retry.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
export PUSH_RETRY_BACKOFF_SECONDS=0
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.invalid
export GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.invalid
fails=0

git init -q --bare -b master "$tmp/remote.git"
git clone -q "$tmp/remote.git" "$tmp/seed" 2>/dev/null
(cd "$tmp/seed" && echo base > base.txt && echo a > shared.txt && git add . \
    && git commit -qm base && git push -q origin HEAD:master)

# CI-like shallow clone, then the remote moves underneath it.
ci_clone() {
    rm -rf "$tmp/ci"
    git clone -q --depth 1 "file://$tmp/remote.git" "$tmp/ci"
}
remote_commit() { # <file> <content>
    (cd "$tmp/seed" && git pull -q --rebase origin master \
        && echo "$2" > "$1" && git add "$1" && git commit -qm "maint $1" \
        && git push -q origin HEAD:master)
}
check() { # <name> <cond-exit>
    if [ "$2" -eq 0 ]; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi
}

# 1. Up to date: first push wins.
ci_clone
(cd "$tmp/ci" && echo 1 > snap1.txt && git add . && git commit -qm snap1)
out=$(cd "$tmp/ci" && sh "$SCRIPT" master 2>&1) && rc=0 || rc=$?
echo "$out" | grep -q "attempt 1/5"; check "up to date: pushed on attempt 1" $(( rc + $? ))

# 2. Branch moved (different file): rebase + retry succeeds, both commits kept.
ci_clone
(cd "$tmp/ci" && echo 2 > snap2.txt && git add . && git commit -qm snap2)
remote_commit other.txt moved
out=$(cd "$tmp/ci" && sh "$SCRIPT" master 2>&1) && rc=0 || rc=$?
log=$(git --git-dir="$tmp/remote.git" log --format=%s master)
echo "$out" | grep -q "attempt 2/5" && echo "$log" | grep -q '^snap2$' \
    && echo "$log" | grep -q '^maint other.txt$'; check "moved branch: rebased, pushed on attempt 2" $(( rc + $? ))

# 3. Real conflict: fails, rebase aborted, nothing pushed.
ci_clone
(cd "$tmp/ci" && echo mine > shared.txt && git add . && git commit -qm conflicting)
remote_commit shared.txt theirs
before=$(git --git-dir="$tmp/remote.git" rev-parse master)
rc=0; (cd "$tmp/ci" && sh "$SCRIPT" master >"$tmp/out3" 2>&1) || rc=$?
after=$(git --git-dir="$tmp/remote.git" rev-parse master)
[ "$rc" -ne 0 ] && [ "$before" = "$after" ] && grep -q "conflicted; not pushing" "$tmp/out3" \
    && [ ! -d "$tmp/ci/.git/rebase-merge" ] && [ ! -d "$tmp/ci/.git/rebase-apply" ]
check "conflict: fails, aborts rebase, pushes nothing" $?

if [ "$fails" -ne 0 ]; then
    sed 's/^/     | /' "$tmp/out3"
    exit 1
fi
echo "all push-with-rebase-retry cases passed"

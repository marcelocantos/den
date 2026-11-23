#!/bin/sh
# Push the current HEAD to <branch> on origin, rebasing onto the remote and
# retrying when the push is rejected because the branch moved meanwhile
# (e.g. a maintainer push landed during a 45-minute benchmark run).
#
#   scripts/push-with-rebase-retry.sh <branch> [attempts]
#
# Up to <attempts> pushes (default 5; env PUSH_RETRY_BACKOFF_SECONDS sets the
# first backoff, default 2, doubled each retry). Between attempts:
# `git pull --rebase origin <branch>`. A rebase conflict is a real conflict:
# the rebase is aborted and the script fails without pushing.
set -eu

branch=${1:?usage: push-with-rebase-retry.sh <branch> [attempts]}
attempts=${2:-5}
backoff=${PUSH_RETRY_BACKOFF_SECONDS:-2}

i=1
while :; do
    if git push origin "HEAD:$branch"; then
        echo "pushed to $branch on attempt $i/$attempts"
        exit 0
    fi
    if [ "$i" -ge "$attempts" ]; then
        echo "::error::push to $branch still rejected after $attempts attempts"
        exit 1
    fi
    echo "push to $branch rejected (attempt $i/$attempts); rebasing onto origin/$branch"
    if ! git pull --rebase origin "$branch"; then
        git rebase --abort 2>/dev/null || true
        echo "::error::rebase onto origin/$branch conflicted; not pushing"
        exit 1
    fi
    sleep "$backoff"
    backoff=$((backoff * 2))
    i=$((i + 1))
done

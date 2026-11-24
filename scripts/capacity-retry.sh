#!/usr/bin/env bash
# Re-dispatch a scheduled workflow run that GitHub lost to macOS runner
# capacity. Shared by the *-capacity-retry.yml companions.
#
#   scripts/capacity-retry.sh <workflow-file> [-f key=value ...]
#
# Reads RUN_ID, RUN_ATTEMPT, HEAD_SHA and HEAD_BRANCH (the completed
# scheduled run) and REPO from the environment. The -f inputs are passed to
# every dispatch, after capacity_retry_sha/capacity_retry_run, which the
# workflow puts in its run-name: "... capacity-retry of <sha> (run <id>)".
#
# 1. Classify the run's non-successful jobs. A capacity cancel is a
#    macOS-labelled job, conclusion "cancelled", no steps, no runner and a
#    "not acquired by Runner" annotation; anything else is a real failure,
#    and then nothing is retried.
# 2. Dispatch retry 1 if no run titled "(run <id>)" exists yet.
# 3. Retries use github.token, which fires no workflow_run of their own, so
#    follow retry 1 to completion (RETRY_WATCH_MINUTES) and classify it the
#    same way. If it too lost only macOS jobs to capacity, wait
#    RETRY_DELAY_SECONDS and dispatch retry 2 if exactly one retry exists.
#    Never more than MAX_RETRIES (default 2) per scheduled run.
#
# Env knobs: MAX_RETRIES (2), RETRY_DELAY_SECONDS (1800), RETRY_WATCH_MINUTES
# (75), POLL_SECONDS (60), LIST_POLL_SECONDS (5), DRY_RUN ("true": classify
# and count, dispatch nothing). Tested by tests/scripts/test_capacity_retry.sh.
set -euo pipefail

workflow=${1:?usage: capacity-retry.sh <workflow-file> [-f key=value ...]}
shift
: "${REPO:?}" "${RUN_ID:?}" "${RUN_ATTEMPT:?}" "${HEAD_SHA:?}" "${HEAD_BRANCH:?}"
MAX_RETRIES=${MAX_RETRIES:-2}
RETRY_DELAY_SECONDS=${RETRY_DELAY_SECONDS:-1800}
RETRY_WATCH_MINUTES=${RETRY_WATCH_MINUTES:-75}
POLL_SECONDS=${POLL_SECONDS:-60}
LIST_POLL_SECONDS=${LIST_POLL_SECONDS:-5}
DRY_RUN=${DRY_RUN:-false}
short_sha=${HEAD_SHA:0:12}
summary=${GITHUB_STEP_SUMMARY:-/dev/stdout}

# classify RUN ATTEMPT: sets $bad_jobs (non-successful jobs), of which
# $capacity are capacity cancels and $real are anything else.
classify() {
    local run=$1 attempt=$2 jobs bad job id name conclusion macos nsteps runner not_acquired
    jobs=$(gh api --paginate \
        "repos/$REPO/actions/runs/$run/attempts/$attempt/jobs?per_page=100" \
        --jq '.jobs[]' | jq -s '.')
    # Skipped/neutral jobs are fine: dependents of a capacity-cancelled job,
    # or filtered out.
    bad=$(jq -c '[.[] | select(.conclusion != "success"
                               and .conclusion != "skipped"
                               and .conclusion != "neutral")]' <<<"$jobs")
    bad_jobs=$(jq length <<<"$bad")
    capacity=0
    real=0
    while read -r job; do
        [ -n "$job" ] || continue
        id=$(jq -r .id <<<"$job")
        name=$(jq -r .name <<<"$job")
        conclusion=$(jq -r .conclusion <<<"$job")
        macos=$(jq -r '[.labels[]? | ascii_downcase | startswith("macos")] | any' <<<"$job")
        nsteps=$(jq -r '.steps | length' <<<"$job")
        runner=$(jq -r '.runner_name // ""' <<<"$job")
        not_acquired=$(gh api "repos/$REPO/check-runs/$id/annotations" \
            --jq '[.[] | select(.message | test("not acquired by Runner"; "i"))] | length')
        if [ "$conclusion" = "cancelled" ] && [ "$macos" = "true" ] \
           && [ "$nsteps" -eq 0 ] && [ -z "$runner" ] && [ "$not_acquired" -gt 0 ]; then
            echo "capacity: $name (job $id)"
            capacity=$((capacity + 1))
        else
            echo "real:     $name (job $id): conclusion=$conclusion macos=$macos steps=$nsteps runner='$runner' not_acquired=$not_acquired"
            real=$((real + 1))
        fi
    done < <(jq -c '.[]' <<<"$bad")
}

# Capacity-retries of this scheduled run so far (any status), newest first,
# one URL per line.
retries() {
    gh api --paginate \
        "repos/$REPO/actions/workflows/$workflow/runs?event=workflow_dispatch&per_page=100" \
        --jq ".workflow_runs[] | select(.display_title | contains(\"(run $RUN_ID)\")) | .html_url"
}
count_lines() { grep -c . <<<"$1" || true; }

# dispatch N: dispatch retry N if exactly N-1 retries exist, then wait for
# it to be listed (so a duplicate trigger in this concurrency group counts
# it). Sets $url ("" if it did not appear); returns 1 if not dispatched.
dispatch() {
    local n=$1 existing count master_sha
    url=""
    existing=$(retries)
    count=$(count_lines "$existing")
    if [ "$count" -ne $((n - 1)) ]; then
        echo "::notice::Run $RUN_ID has $count capacity-retr(y/ies); retry $n of $MAX_RETRIES not dispatched:"
        echo "$existing"
        echo "Not retried: run $RUN_ID already has $count capacity-retr(y/ies)." >>"$summary"
        return 1
    fi
    master_sha=$(gh api "repos/$REPO/commits/$HEAD_BRANCH" --jq .sha)
    if [ "$master_sha" != "$HEAD_SHA" ]; then
        echo "::warning::$HEAD_BRANCH moved from $short_sha to ${master_sha:0:12}; the capacity-retry runs on the current $HEAD_BRANCH head."
    fi
    echo "Dispatching capacity-retry $n of $MAX_RETRIES of $workflow for run $RUN_ID."
    if [ "$DRY_RUN" = "true" ]; then
        echo "DRY_RUN: would dispatch $workflow on $HEAD_BRANCH (capacity_retry_sha=$short_sha, capacity_retry_run=$RUN_ID ${dispatch_inputs[*]-})"
        return 1
    fi
    gh workflow run "$workflow" --repo "$REPO" --ref "$HEAD_BRANCH" \
        -f capacity_retry_sha="$short_sha" \
        -f capacity_retry_run="$RUN_ID" \
        ${dispatch_inputs[@]+"${dispatch_inputs[@]}"}

    for _ in $(seq 1 18); do
        sleep "$LIST_POLL_SECONDS"
        existing=$(retries)
        if [ "$(count_lines "$existing")" -ge "$n" ]; then
            url=$(head -1 <<<"$existing")
            break
        fi
    done
    if [ -z "$url" ]; then
        echo "::warning::Dispatched, but capacity-retry $n for run $RUN_ID did not appear in time."
    else
        echo "::notice::capacity-retry $n of $short_sha (run $RUN_ID): $url"
    fi
    echo "capacity-retry $n of $short_sha (run $RUN_ID): ${url:-dispatched, run not yet listed}" >>"$summary"
}

dispatch_inputs=("$@")

classify "$RUN_ID" "$RUN_ATTEMPT"
if [ "$bad_jobs" -eq 0 ]; then
    echo "::notice::No failed jobs in run $RUN_ID attempt $RUN_ATTEMPT; nothing to retry."
    exit 0
fi
if [ "$real" -gt 0 ]; then
    echo "::notice::Run $RUN_ID has $real non-capacity failure(s); not retrying."
    echo "Not retried: $real non-capacity failure(s) in run $RUN_ID." >>"$summary"
    exit 0
fi
echo "Run $RUN_ID: $capacity macOS job(s) capacity-cancelled, no real failures."

dispatch 1 || exit 0
[ "$MAX_RETRIES" -ge 2 ] || exit 0
if [ -z "$url" ]; then
    echo "::warning::Cannot follow capacity-retry 1 for run $RUN_ID; no second retry."
    exit 0
fi

# Follow retry 1 and retry once more if it is lost to capacity too.
retry_id=${url##*/}
status=""
for _ in $(seq 1 $((RETRY_WATCH_MINUTES * 60 / POLL_SECONDS))); do
    sleep "$POLL_SECONDS"
    status=$(gh api "repos/$REPO/actions/runs/$retry_id" --jq .status)
    [ "$status" = "completed" ] && break
done
if [ "$status" != "completed" ]; then
    echo "::warning::capacity-retry 1 ($url) not finished after ${RETRY_WATCH_MINUTES} min; no second retry."
    exit 0
fi
conclusion=$(gh api "repos/$REPO/actions/runs/$retry_id" --jq .conclusion)
classify "$retry_id" 1
if [ "$bad_jobs" -eq 0 ]; then
    echo "::notice::capacity-retry 1 ($url) concluded $conclusion with no failed jobs."
    echo "capacity-retry 1: $conclusion." >>"$summary"
    exit 0
fi
if [ "$real" -gt 0 ]; then
    echo "::notice::capacity-retry 1 ($url) has $real non-capacity failure(s); no second retry."
    echo "capacity-retry 1: $real non-capacity failure(s); no second retry." >>"$summary"
    exit 0
fi
echo "capacity-retry 1 ($url) lost $capacity macOS job(s) to capacity; waiting ${RETRY_DELAY_SECONDS}s."
sleep "$RETRY_DELAY_SECONDS"
dispatch 2 || exit 0

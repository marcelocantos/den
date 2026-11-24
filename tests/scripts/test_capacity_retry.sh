#!/bin/sh
# Tests for scripts/capacity-retry.sh, the shared logic of the
# *-capacity-retry.yml companions, against a stub `gh` that serves canned
# jobs/annotations/runs JSON and records dispatches. Each case runs the
# script for both companions' calls (source-build-smoke.yml with
# `-f platform=macos`, bench.yml with no extra inputs) and checks the
# dispatches it makes.
#
# Run:
#   sh tests/scripts/test_capacity_retry.sh
# or via ctest (name: script_test_capacity_retry).
set -u

_SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT=$(CDPATH= cd -- "${_SELF_DIR}/../.." && pwd)
SCRIPT="${ROOT}/scripts/capacity-retry.sh"

for tool in bash jq; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "FAIL: $tool not found (capacity-retry.sh needs it)"
        exit 1
    fi
done

_pass=0
_fail=0
ok() { _pass=$((_pass + 1)); echo "PASS: $1"; }
bad() { _fail=$((_fail + 1)); echo "FAIL: $1"; }

SIM=$(mktemp -d)
trap 'rm -rf "$SIM"' EXIT
export SIM
mkdir "$SIM/bin"

# Stub gh. `gh workflow run WF ... -f k=v ...` logs "WF k=v ..." (sorted
# inputs) and lists a new run titled like the real run-name; `gh api URL
# [--paginate] [--jq EXPR]` filters a fixture through jq. The scheduled
# run is 777; its jobs are jobs-777.json, every other run's (the retries')
# jobs-retry.json.
cat >"$SIM/bin/gh" <<'STUB'
#!/bin/sh
if [ "$1" = workflow ]; then
    wf=$3; shift 3
    inputs=""
    while [ $# -gt 0 ]; do
        [ "$1" = -f ] && inputs="$inputs $2"
        shift
    done
    echo "$wf$(printf '%s\n' $inputs | sort | tr '\n' ' ' | sed 's/ $//; s/^/ /')" >>"$SIM/dispatches"
    sha=$(printf '%s\n' $inputs | sed -n 's/^capacity_retry_sha=//p')
    run=$(printf '%s\n' $inputs | sed -n 's/^capacity_retry_run=//p')
    n=$(( $(jq '.workflow_runs | length' "$SIM/runs.json") + 900 ))
    jq --arg t "X capacity-retry of $sha (run $run)" --arg u "https://github.com/o/r/actions/runs/$n" \
        '.workflow_runs = [{display_title: $t, html_url: $u}] + .workflow_runs' \
        "$SIM/runs.json" >"$SIM/runs.tmp" && mv "$SIM/runs.tmp" "$SIM/runs.json"
    exit 0
fi
shift
url=""; jqx="."
while [ $# -gt 0 ]; do
    case "$1" in
    --paginate) ;;
    --jq) shift; jqx=$1 ;;
    *) url=$1 ;;
    esac
    shift
done
case "$url" in
*/runs/777/attempts/*/jobs*) f=$SIM/jobs-777.json ;;
*/attempts/*/jobs*) f=$SIM/jobs-retry.json ;;
*/annotations) f=$SIM/ann.json ;;
*/workflows/*/runs*) f=$SIM/runs.json ;;
*/actions/runs/*) echo '{"status":"completed","conclusion":"failure"}' | jq -r "$jqx"; exit ;;
*/commits/*) echo "{\"sha\":\"$HEAD_SHA\"}" | jq -r "$jqx"; exit ;;
*) echo "stub gh: unexpected $url" >&2; exit 1 ;;
esac
jq -r "$jqx" "$f"
STUB
chmod +x "$SIM/bin/gh"

echo '[{"message":"The job was not acquired by Runner of type hosted even after multiple attempts"}]' >"$SIM/ann.json"
CAP='{"jobs":[{"id":1,"name":"Build (macos-15)","conclusion":"cancelled","labels":["macos-15"],"steps":[],"runner_name":null},
             {"id":2,"name":"Build (ubuntu-latest)","conclusion":"success","labels":["ubuntu-latest"],"steps":[{}],"runner_name":"r"}]}'
REAL='{"jobs":[{"id":1,"name":"Build (macos-15)","conclusion":"failure","labels":["macos-15"],"steps":[{}],"runner_name":"r1"}]}'
OK='{"jobs":[{"id":1,"name":"Build (macos-15)","conclusion":"success","labels":["macos-15"],"steps":[{}],"runner_name":"r1"}]}'

# check NAME SCHEDULED_JOBS RETRY_JOBS WANT_DISPATCHES [EXISTING_TITLE]:
# for each companion, run the script with the scheduled run's and the
# retry's jobs and an optional pre-existing retry title; it must exit 0
# and dispatch exactly WANT_DISPATCHES times, each with that companion's
# inputs.
check() {
    name=$1 sched=$2 retry=$3 want=$4 existing=${5:-}
    for wf in source-build-smoke.yml bench.yml; do
        if [ "$wf" = source-build-smoke.yml ]; then set -- -f platform=macos; else set --; fi
        echo "$sched" >"$SIM/jobs-777.json"
        echo "$retry" >"$SIM/jobs-retry.json"
        : >"$SIM/dispatches"
        if [ -n "$existing" ]; then
            jq -n --arg t "$existing" '{workflow_runs: [{display_title: $t, html_url: "https://github.com/o/r/actions/runs/111"}]}'
        else
            echo '{"workflow_runs": []}'
        fi >"$SIM/runs.json"
        out=$(PATH="$SIM/bin:$PATH" REPO=o/r RUN_ID=777 RUN_ATTEMPT=1 \
            HEAD_SHA=abcdef1234567890 HEAD_BRANCH=master GITHUB_STEP_SUMMARY=/dev/null \
            RETRY_DELAY_SECONDS=0 RETRY_WATCH_MINUTES=1 POLL_SECONDS=1 LIST_POLL_SECONDS=0 \
            "$SCRIPT" "$wf" "$@" 2>&1)
        rc=$?
        expect=""
        if [ "$wf" = source-build-smoke.yml ]; then
            line="$wf capacity_retry_run=777 capacity_retry_sha=abcdef123456 platform=macos"
        else
            line="$wf capacity_retry_run=777 capacity_retry_sha=abcdef123456"
        fi
        i=0
        while [ "$i" -lt "$want" ]; do expect="$expect$line
"; i=$((i + 1)); done
        got=$(cat "$SIM/dispatches")
        if [ "$rc" -eq 0 ] && [ "$got" = "$(printf '%s' "$expect")" ]; then
            ok "$wf: $name"
        else
            bad "$wf: $name (exit $rc, want $want dispatch(es), got:
$got)
$out"
        fi
    done
}

check "capacity miss, retry 1 lost to capacity: 2 retries" "$CAP" "$CAP" 2
check "capacity miss, retry 1 passes: 1 retry" "$CAP" "$OK" 1
check "capacity miss, retry 1 real failure: 1 retry" "$CAP" "$REAL" 1
check "already retried (manual re-run): no retry" "$CAP" "$CAP" 0 "X capacity-retry of abc (run 777)"
check "real failure: no retry" "$REAL" "$CAP" 0
check "another run's retry does not count: 2 retries" "$CAP" "$CAP" 2 "X capacity-retry of abc (run 778)"
check "no failed jobs: no retry" "$OK" "$CAP" 0

echo "${_pass} passed, ${_fail} failed"
[ "$_fail" -eq 0 ]

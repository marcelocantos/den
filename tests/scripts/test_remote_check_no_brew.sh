#!/bin/sh
# Guard for #159: scripts/remote-check.sh must not put a Homebrew prefix on
# CMake's search path, and must check the binary's links before testing it.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
f="$ROOT/scripts/remote-check.sh"
fails=0
if grep -n '/opt/homebrew\|brew --prefix' "$f"; then
    echo "FAIL remote-check.sh references a Homebrew prefix (above)"
    fails=1
else
    echo "ok   no Homebrew prefix in remote-check.sh"
fi
if grep -q '^scripts/check-links.sh build/den$' "$f"; then
    echo "ok   remote-check.sh runs check-links.sh on build/den"
else
    echo "FAIL remote-check.sh does not run scripts/check-links.sh build/den"
    fails=1
fi
bash -n "$f" && echo "ok   remote-check.sh parses" || fails=1
exit $fails

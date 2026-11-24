#!/bin/sh
# Guard for #158: no GitHub workflow or local action may install from
# Homebrew or point a build at a Homebrew prefix. Build tools come from
# .github/actions/toolchain (pinned official releases); libarchive is
# vendored. Comment lines are ignored, so workflows may still explain why.
#
# Fails on any non-comment line in .github/workflows/*.y*ml or
# .github/actions/**/action.y*ml matching `brew install`, `brew --prefix`
# or `/opt/homebrew`. Self-tests the scanner on fixtures first.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PATTERN='brew install|brew --prefix|/opt/homebrew'

# scan FILE...: print offending non-comment lines as file:line: text.
scan() {
    for f in "$@"; do
        [ -f "$f" ] || continue
        awk -v f="$f" -v pat="$PATTERN" '
            { line = $0; sub(/^[ \t]*/, "", line) }
            line ~ /^#/ { next }
            $0 ~ pat { printf "%s:%d: %s\n", f, NR, $0 }' "$f"
    done
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
fails=0

# Self-test: the scanner must catch each pattern and ignore comments.
cat > "$tmp/bad.yml" <<'YML'
      - run: brew install cmake ninja
      - run: cmake -DCMAKE_PREFIX_PATH=$(brew --prefix libarchive)
        env: { P: /opt/homebrew/bin }
YML
cat > "$tmp/ok.yml" <<'YML'
      # was: brew install cmake ninja (see /opt/homebrew)
      - uses: ./.github/actions/toolchain
YML
if [ "$(scan "$tmp/bad.yml" | wc -l | tr -d ' ')" = 3 ] && [ -z "$(scan "$tmp/ok.yml")" ]; then
    echo "ok   scanner self-test"
else
    echo "FAIL scanner self-test"; fails=1
fi

hits=$(scan "$ROOT"/.github/workflows/*.yml "$ROOT"/.github/workflows/*.yaml \
            "$ROOT"/.github/actions/*/action.yml "$ROOT"/.github/actions/*/action.yaml)
if [ -z "$hits" ]; then
    echo "ok   no brew install / brew --prefix / /opt/homebrew in workflows or actions"
else
    echo "FAIL Homebrew in workflows/actions (use .github/actions/toolchain):"
    echo "$hits" | sed 's/^/     /'
    fails=1
fi
exit $fails

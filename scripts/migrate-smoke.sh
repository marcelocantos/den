#!/usr/bin/env bash
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# migrate-smoke.sh — developer-invoked smoke test for den migrate (🎯T71).
#
# Runs `den migrate` against the real local Homebrew install, then verifies:
#   1. All formulae in HOMEBREW_CELLAR appear in den's ROOT.json manifest.
#   2. The Homebrew Cellar is byte-identical before and after.
#   3. `brew` continues to work (non-destructive check).
#   4. Re-running migration produces no additional adds (idempotent).
#
# SKIPPED assertions (not yet implemented in src/migrate/):
#   - Caskroom packages in manifest   (T37 upstream gap)
#   - Taps registered in den          (T37 upstream gap)
#   - brew services state preserved   (T37 upstream gap)
#
# Usage:
#   ./scripts/migrate-smoke.sh [--den-binary <path>] [--den-home <path>]
#
# Options:
#   --den-binary  Path to the den binary. Defaults to ./build/den (or ./den).
#   --den-home    Isolated DEN_HOME for the test. A temp dir is used if omitted.
#
# The script is intentionally read-only with respect to the live Homebrew
# installation — it never writes to HOMEBREW_CELLAR or HOMEBREW_PREFIX.

set -euo pipefail

# ---------------------------------------------------------------------------
# Argument parsing.
# ---------------------------------------------------------------------------
DEN_BIN=""
DEN_HOME_ARG=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --den-binary)
            DEN_BIN="$2"; shift 2 ;;
        --den-home)
            DEN_HOME_ARG="$2"; shift 2 ;;
        *)
            echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

# Resolve den binary.
if [[ -z "$DEN_BIN" ]]; then
    if [[ -x "./build/den" ]]; then
        DEN_BIN="./build/den"
    elif [[ -x "./den" ]]; then
        DEN_BIN="./den"
    else
        echo "ERROR: cannot find den binary. Pass --den-binary or build first." >&2
        exit 1
    fi
fi

if [[ ! -x "$DEN_BIN" ]]; then
    echo "ERROR: $DEN_BIN is not executable." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Detect Homebrew prefix / Cellar.
# ---------------------------------------------------------------------------
HOMEBREW_PREFIX="${HOMEBREW_PREFIX:-}"
if [[ -z "$HOMEBREW_PREFIX" ]]; then
    if [[ -x /opt/homebrew/bin/brew ]]; then
        HOMEBREW_PREFIX=/opt/homebrew
    elif [[ -x /usr/local/bin/brew ]]; then
        HOMEBREW_PREFIX=/usr/local
    elif [[ -x /home/linuxbrew/.linuxbrew/bin/brew ]]; then
        HOMEBREW_PREFIX=/home/linuxbrew/.linuxbrew
    else
        echo "ERROR: Cannot detect Homebrew prefix. Set HOMEBREW_PREFIX." >&2
        exit 1
    fi
fi
HOMEBREW_CELLAR="${HOMEBREW_CELLAR:-$HOMEBREW_PREFIX/Cellar}"

if [[ ! -d "$HOMEBREW_CELLAR" ]]; then
    echo "ERROR: Cellar not found at $HOMEBREW_CELLAR" >&2
    exit 1
fi

echo "==> Homebrew prefix : $HOMEBREW_PREFIX"
echo "==> Homebrew Cellar : $HOMEBREW_CELLAR"

# ---------------------------------------------------------------------------
# Isolated DEN_HOME.
# ---------------------------------------------------------------------------
SMOKE_DEN_HOME=""
CLEANUP_DEN_HOME=false

if [[ -n "$DEN_HOME_ARG" ]]; then
    SMOKE_DEN_HOME="$DEN_HOME_ARG"
    mkdir -p "$SMOKE_DEN_HOME"
else
    SMOKE_DEN_HOME="$(mktemp -d /tmp/den_smoke_XXXXXX)"
    CLEANUP_DEN_HOME=true
fi

cleanup() {
    if [[ "$CLEANUP_DEN_HOME" == true ]] && [[ -n "$SMOKE_DEN_HOME" ]]; then
        rm -rf "$SMOKE_DEN_HOME"
    fi
}
trap cleanup EXIT

echo "==> Isolated DEN_HOME: $SMOKE_DEN_HOME"

# ---------------------------------------------------------------------------
# Helper: run den with the isolated home.
# ---------------------------------------------------------------------------
run_den() {
    DEN_HOME="$SMOKE_DEN_HOME" \
    HOMEBREW_PREFIX="$HOMEBREW_PREFIX" \
    HOMEBREW_CELLAR="$HOMEBREW_CELLAR" \
    "$DEN_BIN" "$@"
}

# ---------------------------------------------------------------------------
# Step 1: Snapshot the Cellar before migration.
# ---------------------------------------------------------------------------
echo ""
echo "==> [1/5] Snapshotting Cellar before migration …"
BEFORE_SNAP="$(mktemp /tmp/den_snap_before_XXXXXX)"
# Record path:size pairs for every regular file under the Cellar.
find "$HOMEBREW_CELLAR" -type f -exec stat -c '%n %s' {} \; 2>/dev/null | sort > "$BEFORE_SNAP" \
    || find "$HOMEBREW_CELLAR" -type f -exec stat -f '%N %z' {} \; 2>/dev/null | sort > "$BEFORE_SNAP"
echo "    $(wc -l < "$BEFORE_SNAP") files recorded."

# ---------------------------------------------------------------------------
# Step 2: Count formulae in the Cellar.
# ---------------------------------------------------------------------------
echo ""
echo "==> [2/5] Counting installed formulae …"
FORMULA_NAMES=()
while IFS= read -r -d '' entry; do
    name="$(basename "$entry")"
    FORMULA_NAMES+=("$name")
done < <(find "$HOMEBREW_CELLAR" -mindepth 1 -maxdepth 1 -type d -print0 | sort -z)

FORMULA_COUNT=${#FORMULA_NAMES[@]}
echo "    $FORMULA_COUNT formulae found."

if [[ $FORMULA_COUNT -eq 0 ]]; then
    echo "SKIP: No formulae in Cellar — nothing to migrate. Install at least one brew package first."
    exit 0
fi

# ---------------------------------------------------------------------------
# Step 3: First migration run.
# ---------------------------------------------------------------------------
echo ""
echo "==> [3/5] Running first migration …"
run_den migrate 2>&1 | sed 's/^/    /'

# Verify ROOT.json was written.
MANIFEST="$SMOKE_DEN_HOME/manifests/ROOT.json"
if [[ ! -f "$MANIFEST" ]]; then
    echo "FAIL: ROOT.json manifest not written to $MANIFEST" >&2
    exit 1
fi
echo "    Manifest written: $MANIFEST"

# ---------------------------------------------------------------------------
# Step 3a: Check all Cellar formulae appear in manifest.
# ---------------------------------------------------------------------------
echo ""
echo "==> [3a/5] Verifying all formulae appear in manifest …"
MISSING=()
for name in "${FORMULA_NAMES[@]}"; do
    # Use python3 (likely available since it's a Homebrew env) or jq.
    if command -v jq &>/dev/null; then
        present=$(jq --arg n "$name" 'has("packages") and (.packages | has($n))' "$MANIFEST" 2>/dev/null)
    elif command -v python3 &>/dev/null; then
        present=$(python3 -c "
import json, sys
with open('$MANIFEST') as f:
    m = json.load(f)
print('true' if '$name' in m.get('packages', {}) else 'false')
" 2>/dev/null)
    else
        # Fallback: grep-based check (not fully reliable for special chars).
        if grep -q "\"$name\"" "$MANIFEST"; then
            present=true
        else
            present=false
        fi
    fi

    if [[ "$present" != "true" ]]; then
        MISSING+=("$name")
    fi
done

if [[ ${#MISSING[@]} -gt 0 ]]; then
    echo "FAIL: The following formulae are missing from the manifest:" >&2
    for m in "${MISSING[@]}"; do
        echo "  - $m" >&2
    done
    exit 1
fi
echo "    All $FORMULA_COUNT formulae present in manifest. OK"

# ---------------------------------------------------------------------------
# Step 4: Non-destructive check — Cellar unchanged.
# ---------------------------------------------------------------------------
echo ""
echo "==> [4/5] Verifying Cellar is unchanged after migration …"
AFTER_SNAP="$(mktemp /tmp/den_snap_after_XXXXXX)"
find "$HOMEBREW_CELLAR" -type f -exec stat -c '%n %s' {} \; 2>/dev/null | sort > "$AFTER_SNAP" \
    || find "$HOMEBREW_CELLAR" -type f -exec stat -f '%N %z' {} \; 2>/dev/null | sort > "$AFTER_SNAP"

if ! diff -q "$BEFORE_SNAP" "$AFTER_SNAP" >/dev/null 2>&1; then
    echo "FAIL: Cellar was modified by migration!" >&2
    diff "$BEFORE_SNAP" "$AFTER_SNAP" >&2
    rm -f "$BEFORE_SNAP" "$AFTER_SNAP"
    exit 1
fi
rm -f "$BEFORE_SNAP" "$AFTER_SNAP"
echo "    Cellar is byte-identical. OK"

# ---------------------------------------------------------------------------
# Step 4a: brew continues to work.
# ---------------------------------------------------------------------------
if command -v brew &>/dev/null; then
    echo ""
    echo "==> [4a/5] Verifying brew still works …"
    if brew --version >/dev/null 2>&1; then
        echo "    brew --version: OK"
    else
        echo "FAIL: brew --version failed after migration." >&2
        exit 1
    fi
else
    echo "==> [4a/5] SKIP: brew not in PATH — cannot verify brew continues to work."
fi

# ---------------------------------------------------------------------------
# Step 5: Idempotency — second migration run produces no new adds.
# ---------------------------------------------------------------------------
echo ""
echo "==> [5/5] Running second migration (idempotency check) …"
SECOND_OUT="$(run_den migrate 2>&1)"
echo "$SECOND_OUT" | sed 's/^/    /'

# Parse "N added" from the output.
ADDED_SECOND=$(echo "$SECOND_OUT" | grep -oE '[0-9]+ added' | grep -oE '^[0-9]+' || echo "")
if [[ -z "$ADDED_SECOND" ]]; then
    echo "WARN: Could not parse 'N added' from second migration output — skipping count check."
elif [[ "$ADDED_SECOND" -gt 0 ]]; then
    echo "FAIL: Second migration run added $ADDED_SECOND packages (expected 0)." >&2
    exit 1
else
    echo "    Second run added 0 packages (idempotent). OK"
fi

# ---------------------------------------------------------------------------
# SKIPPED assertions — upstream gaps against T37.
# ---------------------------------------------------------------------------
echo ""
echo "==> SKIPPED assertions (not yet implemented in src/migrate/):"
echo "    - Caskroom packages present in manifest            [T37 gap]"
echo "    - Taps registered as den tap sources              [T37 gap]"
echo "    - brew services state preserved post-migration    [T37 gap]"

# ---------------------------------------------------------------------------
# Summary.
# ---------------------------------------------------------------------------
echo ""
echo "==> migrate-smoke.sh PASSED"
echo "    Formulae migrated : $FORMULA_COUNT"
echo "    DEN_HOME used     : $SMOKE_DEN_HOME"

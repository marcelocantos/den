#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Assert that a Homebrew install of den was poured from a bottle rather
# than installed from the release tarball (or built from source).
#
# Usage: check-poured.sh PREFIX [FORMULA]
#
#   PREFIX   the keg or opt prefix (`brew --prefix den`); its
#            INSTALL_RECEIPT.json must say "poured_from_bottle": true.
#   FORMULA  optional formula name (e.g. marcelocantos/tap/den); when
#            given, `$BREW info --json=v2 FORMULA` must also report the
#            installed keg of that version as poured_from_bottle.
#
# brew writes poured_from_bottle=true into the receipt only on the pour
# path (FormulaInstaller#pour); `brew install` of a bottle-less formula
# writes false. BREW defaults to `brew`. Needs python3 for JSON parsing.
#
# Exit: 0 poured, 1 not poured (or unreadable), 2 usage.

set -u

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
    echo "usage: $0 PREFIX [FORMULA]" >&2
    exit 2
fi
prefix=$1
formula=${2:-}
BREW=${BREW:-brew}

receipt="$prefix/INSTALL_RECEIPT.json"
if [ ! -f "$receipt" ]; then
    echo "not poured: no receipt at $receipt" >&2
    exit 1
fi

# The keg directory name is the installed pkg_version.
keg=$(CDPATH= cd -P -- "$prefix" 2>/dev/null && pwd) || keg=$prefix
version=$(basename "$keg")

python3 - "$receipt" <<'PY' || exit 1
import json, sys
path = sys.argv[1]
try:
    with open(path) as f:
        tab = json.load(f)
except (OSError, ValueError) as e:
    sys.exit(f"not poured: cannot parse {path}: {e}")
poured = tab.get("poured_from_bottle")
if poured is not True:
    sys.exit(f"not poured: {path} has poured_from_bottle={json.dumps(poured)}"
             f" (built_as_bottle={json.dumps(tab.get('built_as_bottle'))})")
print(f"receipt: poured_from_bottle=true ({path})")
PY

[ -n "$formula" ] || exit 0

info=$("$BREW" info --json=v2 "$formula" 2>/dev/null) || {
    echo "not poured: '$BREW info --json=v2 $formula' failed" >&2
    exit 1
}
printf '%s' "$info" | python3 -c '
import json, sys
want = sys.argv[1]
try:
    data = json.load(sys.stdin)
    installed = data["formulae"][0].get("installed") or []
except (ValueError, KeyError, IndexError) as e:
    sys.exit(f"not poured: unexpected brew info JSON: {e}")
match = [i for i in installed if i.get("version") == want]
if not match:
    have = ", ".join(str(i.get("version")) for i in installed) or "none"
    sys.exit(f"not poured: brew info lists no installed {want} (have: {have})")
poured = match[0].get("poured_from_bottle")
if poured is not True:
    sys.exit(f"not poured: brew info reports poured_from_bottle="
             f"{json.dumps(poured)} for {want}")
print(f"brew info: {want} poured_from_bottle=true")
' "$version"

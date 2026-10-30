#!/usr/bin/env python3
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# index-disabled.py INDEX NAME: is NAME disabled upstream in den's package
# index (DEN_HOME/cache/index.json, as `den update` writes it)?
#
#   exit 0  disabled; prints "'NAME' is disabled upstream[: REASON][ (disabled
#           on DATE)][; use REPLACEMENT instead]" on stdout
#   exit 1  not disabled, or not in the index (den reports that itself)
#   exit 2  usage error, or INDEX missing or unreadable
#
# verify-release uses this to refuse a fixture package that upstream has
# disabled with a message that names it, instead of failing further on.

import json
import sys


def main(argv):
    if len(argv) != 3:
        print("usage: index-disabled.py INDEX NAME", file=sys.stderr)
        return 2
    path, name = argv[1], argv[2]
    try:
        with open(path, encoding="utf-8") as f:
            index = json.load(f)
        packages = index["packages"]
    except (OSError, ValueError, KeyError, TypeError) as e:
        print(f"index-disabled.py: cannot read {path}: {e}", file=sys.stderr)
        return 2
    for pkg in packages:
        if not isinstance(pkg, dict) or pkg.get("name") != name:
            continue
        if pkg.get("disabled") is not True:
            return 1
        msg = f"'{name}' is disabled upstream"
        if pkg.get("disable_reason"):
            msg += f": {pkg['disable_reason']}"
        if pkg.get("disable_date"):
            msg += f" (disabled on {pkg['disable_date']})"
        if pkg.get("disable_replacement"):
            msg += f"; use {pkg['disable_replacement']} instead"
        print(msg)
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))

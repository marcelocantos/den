#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Guard against shipping a release whose version disagrees with the source.
#
# Usage:
#   scripts/check-version.sh [--cmakelists FILE] [--skip-cmake] [--binary PATH] VERSION
#
# VERSION may carry a leading "v" and/or a "-rc.N" suffix (e.g. 0.14.0,
# v0.14.0, v0.14.0-rc.2); both are stripped before comparison, so the
# result is always compared as X.Y.Z.
#
# Checks (all that apply must pass):
#   * project(den VERSION X.Y.Z) in CMakeLists.txt equals VERSION
#     (skipped with --skip-cmake).
#   * `PATH --version` prints exactly VERSION (only when --binary is given).
#
# Exits 0 on match, 1 on mismatch, 2 on usage error.

set -u

_root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cmakelists="${_root}/CMakeLists.txt"
skip_cmake=0
binary=""
version=""

usage() {
    echo "usage: $0 [--cmakelists FILE] [--skip-cmake] [--binary PATH] VERSION" >&2
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
    --cmakelists)
        [ $# -ge 2 ] || usage
        cmakelists="$2"
        shift 2
        ;;
    --skip-cmake)
        skip_cmake=1
        shift
        ;;
    --binary)
        [ $# -ge 2 ] || usage
        binary="$2"
        shift 2
        ;;
    -*) usage ;;
    *)
        [ -z "$version" ] || usage
        version="$1"
        shift
        ;;
    esac
done
[ -n "$version" ] || usage

want=$(printf '%s' "$version" | sed -e 's/^v//' -e 's/-rc\.[0-9][0-9]*$//')
if ! printf '%s\n' "$want" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo "ERROR: '$version' is not a version of the form [v]X.Y.Z[-rc.N]" >&2
    exit 2
fi

if [ "$skip_cmake" -eq 0 ]; then
    if [ ! -f "$cmakelists" ]; then
        echo "ERROR: $cmakelists not found" >&2
        exit 2
    fi
    cmake_version=$(sed -n -E 's/^[[:space:]]*project\([[:space:]]*den[[:space:]]+VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$cmakelists" | head -1)
    if [ -z "$cmake_version" ]; then
        echo "ERROR: could not find project(den VERSION X.Y.Z) in $cmakelists" >&2
        exit 2
    fi
    if [ "$cmake_version" != "$want" ]; then
        echo "ERROR: requested version $want does not match CMake project version $cmake_version" >&2
        echo "       Bump project(den VERSION ...) in CMakeLists.txt before releasing." >&2
        exit 1
    fi
    echo "OK: CMake project version is $cmake_version"
fi

if [ -n "$binary" ]; then
    if [ ! -x "$binary" ]; then
        echo "ERROR: binary $binary not found or not executable" >&2
        exit 1
    fi
    if ! got=$("$binary" --version 2>&1); then
        echo "ERROR: '$binary --version' failed: $got" >&2
        exit 1
    fi
    got=$(printf '%s' "$got" | tr -d '\r' | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    if [ "$got" != "$want" ]; then
        echo "ERROR: '$binary --version' reports '$got', expected $want" >&2
        exit 1
    fi
    echo "OK: $binary --version reports $got"
fi

exit 0

#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Fail if a den binary links anything a stock OS install doesn't provide.
# Release binaries must be self-contained: no /opt/homebrew, /usr/local or
# other package-manager libraries in the load commands.
#
#   macOS: every `otool -L` entry must live under /usr/lib/ or /System/.
#   Linux: every DT_NEEDED entry (`readelf -d`) must be one of
#          libc, libm, libpthread, libdl, ld-linux*, libcurl.so.4.
#
# Usage: scripts/check-links.sh [--os darwin|linux] BINARY
#
# OTOOL / READELF override the tools (used by the regression test to feed
# canned output). --os defaults to `uname -s`.

set -u

usage() {
    echo "usage: $0 [--os darwin|linux] BINARY" >&2
    exit 2
}

os=""
while [ $# -gt 0 ]; do
    case "$1" in
        --os)
            [ $# -ge 2 ] || usage
            os="$2"
            shift 2
            ;;
        -h | --help) usage ;;
        --*) usage ;;
        *) break ;;
    esac
done
[ $# -eq 1 ] || usage
bin="$1"

if [ -z "$os" ]; then
    case "$(uname -s)" in
        Darwin) os=darwin ;;
        Linux) os=linux ;;
        *)
            echo "check-links: unsupported OS $(uname -s)" >&2
            exit 2
            ;;
    esac
fi

if [ ! -f "$bin" ]; then
    echo "check-links: $bin: no such file" >&2
    exit 2
fi

bad=0
count=0

case "$os" in
    darwin)
        OTOOL="${OTOOL:-otool}"
        if ! out=$("$OTOOL" -L "$bin"); then
            echo "check-links: $OTOOL -L $bin failed" >&2
            exit 2
        fi
        # First line is "<path>:"; each dependency line is
        # "<tab><install name> (compatibility version ...)".
        libs=$(printf '%s\n' "$out" | sed -n '2,$p' | sed -e 's/^[[:space:]]*//' -e 's/ (.*$//' | sed '/^$/d')
        echo "check-links: $bin (darwin)"
        while IFS= read -r lib; do
            [ -n "$lib" ] || continue
            count=$((count + 1))
            case "$lib" in
                /usr/lib/* | /System/*) echo "  ok   $lib" ;;
                *)
                    echo "  BAD  $lib"
                    bad=$((bad + 1))
                    ;;
            esac
        done <<LIBS
$libs
LIBS
        ;;
    linux)
        READELF="${READELF:-readelf}"
        if ! out=$("$READELF" -d "$bin"); then
            echo "check-links: $READELF -d $bin failed" >&2
            exit 2
        fi
        # "0x... (NEEDED)  Shared library: [libfoo.so.1]"
        libs=$(printf '%s\n' "$out" | sed -n 's/.*(NEEDED).*\[\(.*\)\].*/\1/p')
        echo "check-links: $bin (linux)"
        while IFS= read -r lib; do
            [ -n "$lib" ] || continue
            count=$((count + 1))
            case "$lib" in
                libc.so.* | libm.so.* | libpthread.so.* | libdl.so.* | ld-linux*.so.* | libcurl.so.4)
                    echo "  ok   $lib"
                    ;;
                *)
                    echo "  BAD  $lib"
                    bad=$((bad + 1))
                    ;;
            esac
        done <<LIBS
$libs
LIBS
        ;;
    *)
        echo "check-links: unknown --os '$os' (want darwin or linux)" >&2
        exit 2
        ;;
esac

if [ "$count" -eq 0 ]; then
    echo "check-links: no dynamic dependencies parsed from $bin; refusing to pass" >&2
    exit 1
fi
if [ "$bad" -gt 0 ]; then
    echo "check-links: FAIL: $bad disallowed dynamic dependenc$([ "$bad" -eq 1 ] && echo y || echo ies)" >&2
    exit 1
fi
echo "check-links: OK ($count dependencies, all from the base OS)"

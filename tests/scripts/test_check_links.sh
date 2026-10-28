#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Tests for scripts/check-links.sh: release binaries may only link libraries
# shipped with the base OS. Fake otool/readelf feed canned output so both
# platforms' rules are exercised on any host.
#
# Run:
#   sh tests/scripts/test_check_links.sh
# or via ctest (name: script_test_check_links).

set -u

_SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT="${_SELF_DIR}/../.."
SCRIPT="${ROOT}/scripts/check-links.sh"

_pass=0
_fail=0
_tmp=$(mktemp -d "${TMPDIR:-/tmp}/check-links-test.XXXXXX")
trap 'rm -rf "$_tmp"' EXIT

: >"$_tmp/den"

# Fake tools print $_tmp/canned regardless of arguments.
cat >"$_tmp/otool" <<TOOL
#!/bin/sh
cat "$_tmp/canned"
TOOL
cp "$_tmp/otool" "$_tmp/readelf"
cat >"$_tmp/broken" <<'TOOL'
#!/bin/sh
exit 1
TOOL
chmod +x "$_tmp/otool" "$_tmp/readelf" "$_tmp/broken"

# expect NAME WANT_EXIT OS  (canned output on stdin)
expect() {
    cat >"$_tmp/canned"
    OTOOL="$_tmp/otool" READELF="$_tmp/readelf" \
        sh "$SCRIPT" --os "$3" "$_tmp/den" >"$_tmp/out" 2>&1
    _got=$?
    if [ "$_got" -eq "$2" ]; then
        _pass=$((_pass + 1))
        echo "PASS: $1"
    else
        _fail=$((_fail + 1))
        echo "FAIL: $1 (exit $_got, want $2)"
        sed 's/^/    /' "$_tmp/out"
    fi
}

# --- macOS ---

expect "darwin: system-only links pass" 0 darwin <<EOF2
$_tmp/den:
	/usr/lib/libcurl.4.dylib (compatibility version 7.0.0, current version 9.0.0)
	/usr/lib/libz.1.dylib (compatibility version 1.0.0, current version 1.2.12)
	/usr/lib/libbz2.1.0.dylib (compatibility version 1.0.0, current version 1.0.8)
	/usr/lib/libiconv.2.dylib (compatibility version 7.0.0, current version 7.0.0)
	/usr/lib/libexpat.1.dylib (compatibility version 7.0.0, current version 8.0.0)
	/usr/lib/libc++.1.dylib (compatibility version 1.0.0, current version 1800.101.0)
	/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1351.0.0)
	/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation (compatibility version 150.0.0, current version 2503.1.255)
EOF2

expect "darwin: homebrew libarchive fails (v0.14.0 regression)" 1 darwin <<EOF2
$_tmp/den:
	/usr/lib/libcurl.4.dylib (compatibility version 7.0.0, current version 9.0.0)
	/opt/homebrew/opt/libarchive/lib/libarchive.13.dylib (compatibility version 23.0.0, current version 23.4.0)
	/usr/lib/libc++.1.dylib (compatibility version 1.0.0, current version 1800.101.0)
	/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1351.0.0)
EOF2

expect "darwin: /usr/local fails" 1 darwin <<EOF2
$_tmp/den:
	/usr/local/lib/libzstd.1.dylib (compatibility version 1.0.0, current version 1.5.7)
	/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1351.0.0)
EOF2

expect "darwin: @rpath fails" 1 darwin <<EOF2
$_tmp/den:
	@rpath/liblzma.5.dylib (compatibility version 13.0.0, current version 13.8.0)
	/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1351.0.0)
EOF2

expect "darwin: no dependencies parsed fails" 1 darwin <<EOF2
$_tmp/den: is not an object file
EOF2

# --- Linux ---

expect "linux: glibc + libcurl passes (x86_64)" 0 linux <<'EOF2'

Dynamic section at offset 0x4b1d50 contains 29 entries:
  Tag        Type                         Name/Value
 0x0000000000000001 (NEEDED)             Shared library: [libcurl.so.4]
 0x0000000000000001 (NEEDED)             Shared library: [libm.so.6]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
 0x0000000000000001 (NEEDED)             Shared library: [ld-linux-x86-64.so.2]
 0x000000000000000c (INIT)               0x4a000
EOF2

expect "linux: glibc + libcurl passes (aarch64)" 0 linux <<'EOF2'
 0x0000000000000001 (NEEDED)             Shared library: [libcurl.so.4]
 0x0000000000000001 (NEEDED)             Shared library: [libpthread.so.0]
 0x0000000000000001 (NEEDED)             Shared library: [libdl.so.2]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
 0x0000000000000001 (NEEDED)             Shared library: [ld-linux-aarch64.so.1]
EOF2

expect "linux: dynamic libarchive fails (v0.14.0 regression)" 1 linux <<'EOF2'
 0x0000000000000001 (NEEDED)             Shared library: [libcurl.so.4]
 0x0000000000000001 (NEEDED)             Shared library: [libarchive.so.13]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
EOF2

expect "linux: dynamic libstdc++ fails" 1 linux <<'EOF2'
 0x0000000000000001 (NEEDED)             Shared library: [libcurl.so.4]
 0x0000000000000001 (NEEDED)             Shared library: [libstdc++.so.6]
 0x0000000000000001 (NEEDED)             Shared library: [libgcc_s.so.1]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
EOF2

expect "linux: other libcurl soname fails" 1 linux <<'EOF2'
 0x0000000000000001 (NEEDED)             Shared library: [libcurl-gnutls.so.4]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
EOF2

expect "linux: static binary (no NEEDED) fails closed" 1 linux <<'EOF2'

There is no dynamic section in this file.
EOF2

# --- Tool failure / usage ---

: >"$_tmp/canned"
OTOOL="$_tmp/broken" sh "$SCRIPT" --os darwin "$_tmp/den" >"$_tmp/out" 2>&1
if [ $? -eq 2 ]; then
    _pass=$((_pass + 1))
    echo "PASS: failing otool is an error"
else
    _fail=$((_fail + 1))
    echo "FAIL: failing otool is an error"
fi

sh "$SCRIPT" --os darwin "$_tmp/missing" >"$_tmp/out" 2>&1
if [ $? -eq 2 ]; then
    _pass=$((_pass + 1))
    echo "PASS: missing binary is an error"
else
    _fail=$((_fail + 1))
    echo "FAIL: missing binary is an error"
fi

echo ""
echo "check-links: ${_pass} passed, ${_fail} failed"
[ "$_fail" -eq 0 ]

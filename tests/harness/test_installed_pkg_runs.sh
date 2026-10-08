#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Regression tests for tests/harness/installed_pkg_runs.sh.
#
# Proves that wrong-arch / non-executable / loader-error binaries FAIL the
# installed_pkg_runs check, while a real host-native binary still passes.
#
# Run:
#   sh tests/harness/test_installed_pkg_runs.sh
# or via ctest (name: harness_installed_pkg_runs).

set -u

_SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
# shellcheck source=installed_pkg_runs.sh
. "${_SELF_DIR}/installed_pkg_runs.sh"

_pass=0
_fail=0

assert_true() {
    _name="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        echo "PASS: ${_name}"
        _pass=$((_pass + 1))
    else
        echo "FAIL: ${_name}"
        _fail=$((_fail + 1))
    fi
}

assert_false() {
    _name="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        echo "FAIL: ${_name} (expected failure, got success)"
        _fail=$((_fail + 1))
    else
        echo "PASS: ${_name}"
        _pass=$((_pass + 1))
    fi
}

assert_check_fails() {
    _name="$1"
    _bin="$2"
    _msg=$(check_installed_binary "${_bin}" 2>&1) && _rc=$? || _rc=$?
    if [ "${_rc}" -ne 0 ]; then
        echo "PASS: ${_name} (rc=${_rc}: ${_msg})"
        _pass=$((_pass + 1))
    else
        echo "FAIL: ${_name} (expected failure, got: ${_msg})"
        _fail=$((_fail + 1))
    fi
}

assert_check_passes() {
    _name="$1"
    _bin="$2"
    _msg=$(check_installed_binary "${_bin}" 2>&1) && _rc=$? || _rc=$?
    if [ "${_rc}" -eq 0 ]; then
        echo "PASS: ${_name} (version: ${_msg})"
        _pass=$((_pass + 1))
    else
        echo "FAIL: ${_name} (expected pass, got rc=${_rc}: ${_msg})"
        _fail=$((_fail + 1))
    fi
}

# ---------------------------------------------------------------------------
# version_output_looks_real
# ---------------------------------------------------------------------------
assert_true  "version_real_jq"            version_output_looks_real "jq-1.7.1-apple"
assert_true  "version_real_dotted"        version_output_looks_real "foo 1.2.3"
assert_true  "version_real_vpref"         version_output_looks_real "tool version 2.0"
assert_true  "version_real_hello"         version_output_looks_real "hello 2.12.3"
assert_true  "version_real_v_semver"      version_output_looks_real "v1.23.3"
assert_true  "version_real_go_style"      version_output_looks_real "go version go1.22.5 darwin/arm64"
assert_false "version_empty"              version_output_looks_real ""
assert_false "version_whitespace"         version_output_looks_real "   "
assert_false "version_exec_format"        version_output_looks_real "Exec format error"
assert_false "version_bad_cpu"            version_output_looks_real "Bad CPU type in executable"
assert_false "version_wrong_arch"         version_output_looks_real "wrong architecture"
assert_false "version_no_digit"           version_output_looks_real "not a version at all"
assert_false "version_bare_integer"       version_output_looks_real "42"
assert_false "version_error_one"          version_output_looks_real "error 1"
assert_false "version_v_major_only"       version_output_looks_real "v2"
assert_false "version_single_digit_word"  version_output_looks_real "ready build 7"

# ---------------------------------------------------------------------------
# archs_match_host
# ---------------------------------------------------------------------------
_host=$(host_arch_normalized)
assert_true  "archs_host_self"            archs_match_host "${_host}"
assert_false "archs_empty"                archs_match_host ""
case "${_host}" in
    arm64)
        assert_true  "archs_arm64e_on_arm64" archs_match_host "arm64e"
        assert_true  "archs_universal_ok"    archs_match_host "x86_64 arm64"
        assert_false "archs_x86_only_on_arm" archs_match_host "x86_64"
        ;;
    x86_64)
        assert_true  "archs_x86_on_x86"      archs_match_host "x86_64"
        assert_true  "archs_universal_ok"    archs_match_host "x86_64 arm64"
        assert_false "archs_arm_only_on_x86" archs_match_host "arm64"
        ;;
esac

# ---------------------------------------------------------------------------
# Fixture sandbox
# ---------------------------------------------------------------------------
_TMP=$(mktemp -d "${TMPDIR:-/tmp}/den-harness-ipkg.XXXXXX")
trap 'rm -rf "${_TMP}"' EXIT INT HUP TERM

# Script that mimics a loader failure (the old false-pass case): non-zero
# exit + "Exec format error" text.
_fake_execfmt="${_TMP}/fake_execfmt"
cat > "${_fake_execfmt}" << 'EOF'
#!/bin/sh
echo "Exec format error" >&2
exit 126
EOF
chmod +x "${_fake_execfmt}"
assert_check_fails "full_check_exec_format_error" "${_fake_execfmt}"

# Script that exits 0 but prints no digit — not a real version.
_fake_nodigit="${_TMP}/fake_nodigit"
cat > "${_fake_nodigit}" << 'EOF'
#!/bin/sh
echo "ready"
exit 0
EOF
chmod +x "${_fake_nodigit}"
assert_check_fails "full_check_no_version_digit" "${_fake_nodigit}"

# Exit 0 with a digit but no dotted version token — must FAIL.
_fake_bare_int="${_TMP}/fake_bare_int"
cat > "${_fake_bare_int}" << 'EOF'
#!/bin/sh
echo "error 1"
exit 0
EOF
chmod +x "${_fake_bare_int}"
assert_check_fails "full_check_digit_but_no_dotted_version" "${_fake_bare_int}"

# Exit 0 with empty output — must FAIL.
_fake_empty="${_TMP}/fake_empty"
cat > "${_fake_empty}" << 'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "${_fake_empty}"
assert_check_fails "full_check_empty_version_output" "${_fake_empty}"

# Script that exits non-zero with a plausible-looking version string.
_fake_badrc="${_TMP}/fake_badrc"
cat > "${_fake_badrc}" << 'EOF'
#!/bin/sh
echo "tool 1.2.3"
exit 1
EOF
chmod +x "${_fake_badrc}"
assert_check_fails "full_check_nonzero_exit" "${_fake_badrc}"

# Non-executable file.
_fake_nox="${_TMP}/fake_nox"
echo 'tool 1.2.3' > "${_fake_nox}"
chmod a-x "${_fake_nox}"
assert_check_fails "full_check_not_executable" "${_fake_nox}"

# Good script double: exit 0 + real version. Scripts are arch-exempt.
_fake_good="${_TMP}/fake_good"
cat > "${_fake_good}" << 'EOF'
#!/bin/sh
echo "harness-fixture 1.0.0"
exit 0
EOF
chmod +x "${_fake_good}"
assert_check_passes "full_check_good_script" "${_fake_good}"

# ---------------------------------------------------------------------------
# Wrong-arch native binary (where we can produce one)
# ---------------------------------------------------------------------------
_wrong_arch_built=0
case "$(uname -s)" in
    Darwin)
        # Prefer thinning a universal system binary to a foreign slice.
        for _cand in /usr/bin/jq /usr/bin/true /bin/echo; do
            if [ -x "${_cand}" ] && have_cmd lipo; then
                _archs=$(lipo -archs "${_cand}" 2>/dev/null || true)
                _foreign=""
                case "${_host}" in
                    arm64)  _foreign=x86_64 ;;
                    x86_64) _foreign=arm64 ;;
                esac
                if [ -n "${_foreign}" ] && printf '%s' " ${_archs} " | grep -q " ${_foreign} "; then
                    _thin="${_TMP}/thin_foreign"
                    if lipo "${_cand}" -thin "${_foreign}" -output "${_thin}" 2>/dev/null; then
                        chmod +x "${_thin}"
                        assert_false "arch_match_foreign_slice" binary_arch_matches_host "${_thin}"
                        assert_check_fails "full_check_wrong_arch_binary" "${_thin}"
                        _wrong_arch_built=1
                        break
                    fi
                fi
            fi
        done
        # Fallback: compile a tiny wrong-arch binary if a cross compiler works.
        if [ "${_wrong_arch_built}" -eq 0 ] && have_cmd cc; then
            _src="${_TMP}/v.c"
            cat > "${_src}" << 'EOF'
#include <stdio.h>
int main(void) { puts("cross-fixture 1.0.0"); return 0; }
EOF
            _foreign_flag=""
            case "${_host}" in
                arm64)  _foreign_flag="-arch x86_64" ;;
                x86_64) _foreign_flag="-arch arm64" ;;
            esac
            _cross="${_TMP}/cross_bin"
            # Intentionally unquoted flag expansion for -arch.
            # shellcheck disable=SC2086
            if cc ${_foreign_flag} -o "${_cross}" "${_src}" 2>/dev/null; then
                assert_false "arch_match_cross_compiled" binary_arch_matches_host "${_cross}"
                assert_check_fails "full_check_cross_compiled" "${_cross}"
                _wrong_arch_built=1
            fi
        fi
        ;;
    Linux)
        if have_cmd cc; then
            _src="${_TMP}/v.c"
            cat > "${_src}" << 'EOF'
#include <stdio.h>
int main(void) { puts("native-fixture 1.0.0"); return 0; }
EOF
            _native="${_TMP}/native_bin"
            if cc -o "${_native}" "${_src}" 2>/dev/null; then
                assert_true "arch_match_native_elf" binary_arch_matches_host "${_native}"
                assert_check_passes "full_check_native_elf" "${_native}"
            fi
        fi
        # Minimal foreign ELF: write a tiny x86-64 ELF header blob if host is arm64
        # (or vice versa) so file(1) reports the wrong machine. We only need the
        # arch check to reject it; it does not have to be runnable.
        if have_cmd file && have_cmd python3; then
            _foreign_elf="${_TMP}/foreign.elf"
            case "${_host}" in
                arm64)
                    # Minimal ELF64 little-endian, e_machine = EM_X86_64 (62).
                    python3 - "${_foreign_elf}" << 'PY'
import sys, struct
path = sys.argv[1]
# EI + e_type=ET_EXEC(2), e_machine=EM_X86_64(62), rest zero-padded.
hdr = bytearray(64)
hdr[0:4] = b'\x7fELF'
hdr[4] = 2  # ELFCLASS64
hdr[5] = 1  # ELFDATA2LSB
hdr[6] = 1  # EV_CURRENT
struct.pack_into('<HHI', hdr, 16, 2, 62, 1)  # type, machine, version
open(path, 'wb').write(hdr)
PY
                    ;;
                x86_64)
                    # e_machine = EM_AARCH64 (183).
                    python3 - "${_foreign_elf}" << 'PY'
import sys, struct
path = sys.argv[1]
hdr = bytearray(64)
hdr[0:4] = b'\x7fELF'
hdr[4] = 2
hdr[5] = 1
hdr[6] = 1
struct.pack_into('<HHI', hdr, 16, 2, 183, 1)
open(path, 'wb').write(hdr)
PY
                    ;;
            esac
            if [ -f "${_foreign_elf}" ]; then
                chmod +x "${_foreign_elf}"
                # file(1) should classify it as ELF of the foreign machine.
                if file -b "${_foreign_elf}" | grep -qi ELF; then
                    assert_false "arch_match_foreign_elf" binary_arch_matches_host "${_foreign_elf}"
                    _wrong_arch_built=1
                fi
            fi
        fi
        ;;
esac

if [ "${_wrong_arch_built}" -eq 0 ]; then
    echo "SKIP: wrong_arch_fixture (could not build a foreign-arch binary on this host)"
fi

# ---------------------------------------------------------------------------
# Real host-native binary still passes (when we can find one with --version)
# ---------------------------------------------------------------------------
_real_ok=0
for _cand in \
    "$(command -v jq 2>/dev/null || true)" \
    "$(command -v python3 2>/dev/null || true)" \
    "$(command -v curl 2>/dev/null || true)" \
    /usr/bin/jq \
    /usr/bin/curl
do
    if [ -n "${_cand}" ] && [ -x "${_cand}" ] && binary_arch_matches_host "${_cand}"; then
        _vout=$("${_cand}" --version 2>&1) && _vrc=$? || _vrc=$?
        if [ "${_vrc}" -ne 0 ]; then
            _vout=$("${_cand}" -V 2>&1) && _vrc=$? || _vrc=$?
        fi
        if [ "${_vrc}" -eq 0 ] && version_output_looks_real "${_vout}"; then
            assert_check_passes "full_check_real_binary_${_cand##*/}" "${_cand}"
            _real_ok=1
            break
        fi
    fi
done

if [ "${_real_ok}" -eq 0 ]; then
    # Synthesize a native binary with a --version if a C compiler is available.
    if have_cmd cc; then
        _src="${_TMP}/real.c"
        cat > "${_src}" << 'EOF'
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        puts("native-fixture 1.2.3");
        return 0;
    }
    return 0;
}
EOF
        _native="${_TMP}/native_version_bin"
        if cc -o "${_native}" "${_src}" 2>/dev/null; then
            assert_check_passes "full_check_compiled_native" "${_native}"
            _real_ok=1
        fi
    fi
fi

if [ "${_real_ok}" -eq 0 ]; then
    echo "FAIL: full_check_real_binary (no suitable native --version binary found)"
    _fail=$((_fail + 1))
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "test_installed_pkg_runs: ${_pass} passed, ${_fail} failed"
if [ "${_fail}" -ne 0 ]; then
    exit 1
fi
exit 0

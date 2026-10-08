#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Helpers for the harness `installed_pkg_runs` step.
# Sourced by smoke.sh and by test_installed_pkg_runs.sh.
#
# A poured bottle must:
#   1. exit 0 from --version / -V,
#   2. print something that looks like a real version string (not a loader error),
#   3. be a native binary whose architecture matches the host (scripts exempt).
#
# Previously the step passed on any non-empty output, so "Exec format error"
# from a wrong-arch bottle counted as success. Rosetta can also make an
# x86_64 bottle appear to "run" on arm64 — the arch check catches that.

# True if a command is on PATH. Defined here so this file is standalone when
# sourced by the regression test; smoke.sh may already define have_cmd.
if ! command -v have_cmd >/dev/null 2>&1; then
    have_cmd() {
        command -v "$1" >/dev/null 2>&1
    }
fi

# Normalize `uname -m` to the labels used by lipo/file (arm64 / x86_64).
host_arch_normalized() {
    _m=$(uname -m)
    case "${_m}" in
        aarch64|arm64|arm64e) printf 'arm64\n' ;;
        x86_64|amd64)         printf 'x86_64\n' ;;
        i386|i686)            printf 'x86\n' ;;
        *)                    printf '%s\n' "${_m}" ;;
    esac
}

# Return 0 if space-separated arch list includes a slice runnable natively
# on this host. arm64e counts as arm64.
archs_match_host() {
    _archs="$1"
    _host=$(host_arch_normalized)
    _a=""
    for _a in ${_archs}; do
        case "${_a}" in
            arm64|arm64e)
                [ "${_host}" = "arm64" ] && return 0
                ;;
            x86_64)
                [ "${_host}" = "x86_64" ] && return 0
                ;;
            i386|x86)
                [ "${_host}" = "x86" ] && return 0
                ;;
            aarch64)
                [ "${_host}" = "arm64" ] && return 0
                ;;
        esac
    done
    return 1
}

# Return 0 if BIN is not a native Mach-O/ELF, or if it is and matches host.
# Non-native artifacts (shell scripts, etc.) return 0 — there is no bottle
# arch to validate. Unreadable/missing BIN returns 1.
binary_arch_matches_host() {
    _bin="$1"
    if [ ! -e "${_bin}" ]; then
        return 1
    fi

    # Prefer lipo on macOS — handles universal binaries cleanly.
    if have_cmd lipo; then
        _archs=$(lipo -archs "${_bin}" 2>/dev/null || true)
        if [ -n "${_archs}" ]; then
            archs_match_host "${_archs}"
            return $?
        fi
    fi

    if have_cmd file; then
        # file(1) may emit multi-line output for fat binaries; flatten.
        _desc=$(file -b "${_bin}" 2>/dev/null | tr '\n' ' ' || true)
        case "${_desc}" in
            *Mach-O*)
                _archs=""
                # Collect every arch token file mentions.
                for _tok in ${_desc}; do
                    case "${_tok}" in
                        arm64e) _archs="${_archs} arm64e" ;;
                        arm64)  _archs="${_archs} arm64" ;;
                        x86_64) _archs="${_archs} x86_64" ;;
                        i386)   _archs="${_archs} i386" ;;
                    esac
                done
                if [ -z "${_archs}" ]; then
                    return 1
                fi
                archs_match_host "${_archs}"
                return $?
                ;;
            *ELF*)
                _host=$(host_arch_normalized)
                case "${_host}" in
                    arm64)
                        printf '%s' "${_desc}" | grep -Eqi 'aarch64|ARM aarch64|arm64'
                        return $?
                        ;;
                    x86_64)
                        printf '%s' "${_desc}" | grep -Eqi 'x86-64|x86_64|Intel 80386|8086'
                        return $?
                        ;;
                    *)
                        # Unknown host — don't block on arch.
                        return 0
                        ;;
                esac
                ;;
            *)
                # Script or other non-native artifact.
                return 0
                ;;
        esac
    fi

    # No lipo/file available: cannot validate; don't fail closed on that alone.
    return 0
}

# Return 0 if OUT looks like a real --version string rather than a loader or
# shell error that previously false-passed the harness.
version_output_looks_real() {
    _v="$1"
    # Trim to a few lines; ignore pure whitespace.
    _v=$(printf '%s' "${_v}" | head -n 8)
    if ! printf '%s' "${_v}" | grep -Eq '[^[:space:]]'; then
        return 1
    fi
    # Loader / dyld / kernel exec failures and similar.
    if printf '%s' "${_v}" | grep -Eiq \
        'exec format error|bad cpu type|cannot execute|wrong architecture|incompatible architecture|invalid application|not a valid mach-o|killed:|segmentation fault|illegal instruction|no such file or directory|permission denied|cannot open shared object'; then
        return 1
    fi
    # Real version output embeds at least one digit (jq-1.7.1, "1.2.3", "v2", …).
    printf '%s' "${_v}" | grep -Eq '[0-9]'
    return $?
}

# Invoke BIN with --version, falling back to -V. Prints version stdout+stderr
# to stdout; returns the exit code of the successful form (or of -V if both
# fail).
run_bin_version() {
    _bin="$1"
    _out=$("${_bin}" --version 2>&1)
    _rc=$?
    if [ "${_rc}" -ne 0 ]; then
        _out=$("${_bin}" -V 2>&1)
        _rc=$?
    fi
    printf '%s\n' "${_out}"
    return "${_rc}"
}

# Full check used by smoke.sh and the regression test.
# Usage: check_installed_binary /path/to/bin
# Prints a one-line reason on failure to stdout; returns 0 on success.
check_installed_binary() {
    _bin="$1"

    if [ ! -e "${_bin}" ]; then
        printf 'binary does not exist: %s\n' "${_bin}"
        return 1
    fi
    if [ ! -x "${_bin}" ]; then
        printf 'binary is not executable: %s\n' "${_bin}"
        return 1
    fi

    if ! binary_arch_matches_host "${_bin}"; then
        printf 'architecture does not match host (%s): %s\n' "$(uname -m)" "${_bin}"
        return 1
    fi

    # Capture output and rc without tripping `set -e` callers.
    _out=$(run_bin_version "${_bin}" 2>&1) && _rc=$? || _rc=$?

    if [ "${_rc}" -ne 0 ]; then
        printf 'version invocation exited %s: %s\n' "${_rc}" "${_out}"
        return 1
    fi
    if ! version_output_looks_real "${_out}"; then
        printf 'version output is not a real version string: %s\n' "${_out}"
        return 1
    fi

    # Echo the version for callers that want to log it.
    printf '%s\n' "${_out}"
    return 0
}

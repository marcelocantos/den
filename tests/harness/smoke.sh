#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Shared end-to-end smoke set for the den release-candidate harness.
# Runs on Linux (Docker) and macOS (SSH) without modification.
#
# Usage:
#   DEN_BIN=/path/to/den DEN_HOME=/tmp/den-harness ./smoke.sh
#
# Required env vars:
#   DEN_BIN   — path to den binary, or bare command name if on PATH
#   DEN_HOME  — writable sandbox; must not be $HOME/.den or /opt/homebrew
#
# Optional:
#   TEST_PKG  — package to exercise (default: jq)

set -eu

# ---------------------------------------------------------------------------
# Guard rails
# ---------------------------------------------------------------------------

if [ -z "${DEN_BIN:-}" ]; then
    echo "FAIL: setup: DEN_BIN is not set" >&2
    exit 1
fi

if [ -z "${DEN_HOME:-}" ]; then
    echo "FAIL: setup: DEN_HOME is not set" >&2
    exit 1
fi

_home_real="${HOME:-}"
if [ "${DEN_HOME}" = "${_home_real}/.den" ] || \
   [ "${DEN_HOME}" = "/opt/homebrew" ]; then
    echo "FAIL: setup: DEN_HOME '${DEN_HOME}' is a protected path — refusing to run" >&2
    exit 1
fi

TEST_PKG="${TEST_PKG:-jq}"

# Export DEN_HOME so den picks it up as its home directory.
export DEN_HOME

# ---------------------------------------------------------------------------
# Counters and helpers
# ---------------------------------------------------------------------------

_pass=0
_fail=0
_skip=0

pass() {
    _name="$1"
    echo "PASS: ${_name}"
    _pass=$((_pass + 1))
}

fail() {
    _name="$1"
    _reason="$2"
    echo "FAIL: ${_name}: ${_reason}"
    _fail=$((_fail + 1))
}

skip() {
    _name="$1"
    _reason="$2"
    echo "SKIP: ${_name}: ${_reason}"
    _skip=$((_skip + 1))
}

# Capture stdout of a den command; stderr goes to our stderr.
capture_den() {
    "${DEN_BIN}" "$@" 2>&1
}

# ---------------------------------------------------------------------------
# Step 1: version
# ---------------------------------------------------------------------------
step_version() {
    _out=$(capture_den --version 2>&1) || true
    if [ -n "${_out}" ]; then
        echo "  version output: ${_out}" >&2
        pass "version"
    else
        fail "version" "den --version produced no output"
    fi
}

# ---------------------------------------------------------------------------
# Step 2: help
# ---------------------------------------------------------------------------
step_help() {
    _out=$(capture_den --help 2>&1) || true
    _mentions_install=$(echo "${_out}" | grep -c "install" || true)
    _mentions_doctor=$(echo "${_out}" | grep -c "doctor" || true)
    if [ "${_mentions_install}" -gt 0 ] && [ "${_mentions_doctor}" -gt 0 ]; then
        pass "help"
    else
        fail "help" "den --help does not mention both 'install' and 'doctor' (install=${_mentions_install}, doctor=${_mentions_doctor})"
    fi
}

# ---------------------------------------------------------------------------
# Step 3: doctor
# ---------------------------------------------------------------------------
step_doctor() {
    # Doctor may report problems (non-zero exit) on a fresh sandbox — that is
    # expected.  We only fail this step if den crashes or produces no output at all.
    _out=$(capture_den doctor 2>&1) || true
    echo "  doctor output:" >&2
    echo "${_out}" | while IFS= read -r _line; do
        echo "    ${_line}" >&2
    done
    if [ -n "${_out}" ]; then
        pass "doctor"
    else
        fail "doctor" "den doctor produced no output (possible crash)"
    fi
}

# ---------------------------------------------------------------------------
# Step 4: info
# ---------------------------------------------------------------------------
step_info() {
    _out=$(capture_den info "${TEST_PKG}" 2>&1)
    _rc=$?
    echo "${_out}" | while IFS= read -r _line; do
        echo "  [info] ${_line}" >&2
    done
    if [ "${_rc}" -eq 0 ] && echo "${_out}" | grep -qi "name"; then
        pass "info"
    else
        fail "info" "den info ${TEST_PKG} failed or returned no name field (rc=${_rc})"
    fi
}

# ---------------------------------------------------------------------------
# Step 5: install
# ---------------------------------------------------------------------------
step_install() {
    _out=$(capture_den install "${TEST_PKG}" 2>&1)
    _rc=$?
    echo "${_out}" | while IFS= read -r _line; do
        echo "  [install] ${_line}" >&2
    done
    if [ "${_rc}" -eq 0 ]; then
        pass "install"
    else
        fail "install" "den install ${TEST_PKG} exited ${_rc}"
    fi
}

# ---------------------------------------------------------------------------
# Step 6: installed_pkg_runs
# ---------------------------------------------------------------------------
step_installed_pkg_runs() {
    # Attempt to find the installed binary.  Try two strategies:
    #   1. Scan $DEN_HOME/envs/*/bin/ for the package binary.
    #   2. If TEST_PKG is jq, call it as jq --version (PATH may not include den envs).
    _bin=""

    # Strategy 1: glob for the binary in any den env.
    for _candidate in "${DEN_HOME}/envs/"*/bin/"${TEST_PKG}"; do
        if [ -x "${_candidate}" ]; then
            _bin="${_candidate}"
            break
        fi
    done

    if [ -z "${_bin}" ]; then
        fail "installed_pkg_runs" "could not locate ${TEST_PKG} binary under \${DEN_HOME}/envs/*/bin/"
        return
    fi

    echo "  binary: ${_bin}" >&2
    _out=$("${_bin}" --version 2>&1) || _out=$("${_bin}" -V 2>&1) || true
    if [ -n "${_out}" ]; then
        echo "  version: ${_out}" >&2
        pass "installed_pkg_runs"
    else
        fail "installed_pkg_runs" "${_bin} --version produced no output"
    fi
}

# ---------------------------------------------------------------------------
# Step 7: env_create
# ---------------------------------------------------------------------------
step_env_create() {
    # Remove any leftover env from a previous run to ensure idempotency.
    capture_den env remove "harness-test" >/dev/null 2>&1 || true

    _out=$(capture_den env create "harness-test" 2>&1)
    _rc=$?
    echo "${_out}" | while IFS= read -r _line; do
        echo "  [env create] ${_line}" >&2
    done
    if [ "${_rc}" -eq 0 ]; then
        pass "env_create"
    else
        fail "env_create" "den env create harness-test exited ${_rc}"
    fi
}

# ---------------------------------------------------------------------------
# Step 8: env_list
# ---------------------------------------------------------------------------
step_env_list() {
    _out=$(capture_den env list 2>&1)
    _rc=$?
    echo "${_out}" | while IFS= read -r _line; do
        echo "  [env list] ${_line}" >&2
    done
    if [ "${_rc}" -eq 0 ] && echo "${_out}" | grep -q "harness-test"; then
        pass "env_list"
    else
        fail "env_list" "den env list did not include 'harness-test' (rc=${_rc})"
    fi
}

# ---------------------------------------------------------------------------
# Step 9: env_use
# ---------------------------------------------------------------------------
step_env_use() {
    _out=$(capture_den env use "harness-test" 2>&1)
    _rc=$?
    echo "${_out}" | while IFS= read -r _line; do
        echo "  [env use] ${_line}" >&2
    done
    if [ "${_rc}" -eq 0 ]; then
        pass "env_use"
    else
        fail "env_use" "den env use harness-test exited ${_rc}"
    fi
}

# ---------------------------------------------------------------------------
# Step 10: env_destroy
# ---------------------------------------------------------------------------
step_env_destroy() {
    # The subcommand is 'env remove' (not 'env destroy' — SKIP note below).
    # TODO(T73): den uses 'env remove'; consider adding an 'env destroy' alias for UX.
    _out=$(capture_den env remove "harness-test" 2>&1)
    _rc=$?
    echo "${_out}" | while IFS= read -r _line; do
        echo "  [env remove] ${_line}" >&2
    done
    if [ "${_rc}" -eq 0 ]; then
        pass "env_destroy"
    else
        fail "env_destroy" "den env remove harness-test exited ${_rc}"
    fi
}

# ---------------------------------------------------------------------------
# Step 11: uninstall
# ---------------------------------------------------------------------------
step_uninstall() {
    _out=$(capture_den uninstall "${TEST_PKG}" 2>&1)
    _rc=$?
    echo "${_out}" | while IFS= read -r _line; do
        echo "  [uninstall] ${_line}" >&2
    done
    if [ "${_rc}" -eq 0 ]; then
        pass "uninstall"
    else
        fail "uninstall" "den uninstall ${TEST_PKG} exited ${_rc}"
    fi
}

# ---------------------------------------------------------------------------
# Step 12: state_clean_post_uninstall
# ---------------------------------------------------------------------------
step_state_clean_post_uninstall() {
    # Check that no env's bin/ directory still holds the package binary.
    _found=0
    for _candidate in "${DEN_HOME}/envs/"*/bin/"${TEST_PKG}"; do
        if [ -e "${_candidate}" ]; then
            _found=1
            echo "  still present: ${_candidate}" >&2
        fi
    done
    if [ "${_found}" -eq 0 ]; then
        pass "state_clean_post_uninstall"
    else
        fail "state_clean_post_uninstall" "${TEST_PKG} binary still exists under \${DEN_HOME}/envs/ after uninstall"
    fi
}

# ---------------------------------------------------------------------------
# Step 13: selfupdate_check
# ---------------------------------------------------------------------------
step_selfupdate_check() {
    # 'den self-update' applies the update immediately — there is no --check or
    # --dry-run flag in the current implementation.  Skip until a safe check
    # mode is added.
    # TODO(T73): add 'den self-update --check' (dry-run mode) so harness can
    #            probe for updates without replacing the binary under test.
    skip "selfupdate_check" "den self-update has no --check/--dry-run mode (would replace running binary)"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

echo "=== den smoke set ===" >&2
echo "  DEN_BIN:  ${DEN_BIN}" >&2
echo "  DEN_HOME: ${DEN_HOME}" >&2
echo "  TEST_PKG: ${TEST_PKG}" >&2
echo "" >&2

step_version
step_help
step_doctor
step_info
step_install
step_installed_pkg_runs
step_env_create
step_env_list
step_env_use
step_env_destroy
step_uninstall
step_state_clean_post_uninstall
step_selfupdate_check

echo "" >&2
echo "=== results: ${_pass} passed, ${_fail} failed, ${_skip} skipped ===" >&2

if [ "${_fail}" -gt 0 ]; then
    exit 1
fi
exit 0

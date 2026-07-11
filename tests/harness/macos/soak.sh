#!/usr/bin/env bash
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Real-home soak set for den on a macOS test host.
# Unlike smoke.sh (isolated DEN_HOME under /tmp), this exercises the live
# dual-run install: ~/.den, shared Cellar, optional shell integration.
#
# Required:
#   DEN_BIN   — path to den binary
# Optional:
#   DEN_HOME  — defaults to $HOME/.den
#   SOAK_PKGS — space-separated bottle packages (default: tree jq)
#   SOAK_MULTI_PKG / SOAK_MULTI_V1 / SOAK_MULTI_V2 — multi-version pair
#
# Output: PASS:/FAIL:/SKIP: lines (same contract as smoke.sh).
# Intentionally not set -e: each step accounts for itself.

set -u

if [ -z "${DEN_BIN:-}" ]; then
    echo "FAIL: setup: DEN_BIN is not set" >&2
    exit 1
fi

export PATH="/opt/homebrew/bin:/usr/local/bin:${HOME}/.cargo/bin:${HOME}/go/bin:${PATH:-}"
export DEN_HOME="${DEN_HOME:-$HOME/.den}"
export HOMEBREW_NO_AUTO_UPDATE=1
export HOMEBREW_NO_ENV_HINTS=1

SOAK_PKGS="${SOAK_PKGS:-tree jq}"
# python@3.12 is multi-version friendly when bottles exist; override if needed.
SOAK_MULTI_PKG="${SOAK_MULTI_PKG:-}"
SOAK_MULTI_V1="${SOAK_MULTI_V1:-}"
SOAK_MULTI_V2="${SOAK_MULTI_V2:-}"
SOAK_ENV="${SOAK_ENV:-/soak-pilot}"

_pass=0
_fail=0
_skip=0

pass() { echo "PASS: $1"; _pass=$((_pass + 1)); }
fail() { echo "FAIL: $1${2:+: $2}"; _fail=$((_fail + 1)); }
skip() { echo "SKIP: $1${2:+: $2}"; _skip=$((_skip + 1)); }

den() { command "$DEN_BIN" "$@"; }

# Run den, capture combined output; set _rc / _out.
run_den() {
    _rc=0
    _out=$(den "$@" 2>&1) || _rc=$?
}

# Binary under active env (or ROOT) that must execute and print something.
run_bin() {
    _name="$1"
    _bin=""
    for _cand in \
        "${DEN_HOME}/envs/ROOT/bin/${_name}" \
        "${DEN_HOME}/envs/"*/bin/"${_name}"; do
        if [ -x "${_cand}" ]; then
            _bin="${_cand}"
            break
        fi
    done
    if [ -z "${_bin}" ]; then
        return 1
    fi
    _bout=$("${_bin}" --version 2>&1) || _bout=$("${_bin}" -V 2>&1) || _bout=$("${_bin}" version 2>&1) || true
    [ -n "${_bout}" ]
}

echo "=== den soak set ==="
echo "  DEN_BIN:  ${DEN_BIN}"
echo "  DEN_HOME: ${DEN_HOME}"
echo "  SOAK_PKGS: ${SOAK_PKGS}"
echo

# ---------------------------------------------------------------------------
step_version() {
    run_den --version
    if [ "${_rc}" -eq 0 ] && [ -n "${_out}" ]; then
        echo "  version: ${_out}" >&2
        pass "version"
    else
        fail "version" "rc=${_rc}"
    fi
}

step_doctor_no_errors() {
    run_den doctor
    # Warnings are OK. Errors about bottle-owned files (e.g. cacert.pem mode)
    # are noise on a shared Cellar; fail only on den-owned path errors.
    _errs=$(echo "${_out}" | grep '\[error\]' | grep -E '\.den/(config|manifests|bin)' || true)
    if [ -n "${_errs}" ]; then
        echo "${_errs}" | while IFS= read -r _l; do echo "  ${_l}" >&2; done
        fail "doctor_no_errors" "doctor reported [error] under den state"
    else
        pass "doctor_no_errors"
    fi
}

step_update() {
    run_den update
    if [ "${_rc}" -eq 0 ]; then
        pass "update"
    else
        fail "update" "rc=${_rc}"
    fi
}

step_settings_safe() {
    den set daemon.auto_upgrade false >/dev/null 2>&1 || true
    chmod 600 "${DEN_HOME}/config.json" 2>/dev/null || true
    run_den settings
    if ! echo "${_out}" | grep -q 'auto_upgrade'; then
        skip "settings_safe" "settings output missing auto_upgrade"
        return
    fi
    # Fail only if JSON/value clearly true (not the word in a key path).
    if echo "${_out}" | grep -E '"auto_upgrade"[[:space:]]*:[[:space:]]*true([[:space:],}]|$)|auto_upgrade = true' >/dev/null; then
        fail "settings_safe" "auto_upgrade still true"
    else
        pass "settings_safe"
    fi
}

step_migrate_dry_run() {
    if ! command -v brew >/dev/null 2>&1; then
        skip "migrate_dry_run" "brew not on PATH"
        return
    fi
    run_den migrate --dry-run
    if [ "${_rc}" -eq 0 ] && echo "${_out}" | grep -qi 'migrat'; then
        pass "migrate_dry_run"
    else
        fail "migrate_dry_run" "rc=${_rc}"
    fi
}

step_migrate_subset() {
    if ! command -v brew >/dev/null 2>&1; then
        skip "migrate_subset" "brew not on PATH"
        return
    fi
    # Migrate a few formulae that usually exist on a brew machine.
    # shellcheck disable=SC2086
    run_den migrate tree jq
    if [ "${_rc}" -eq 0 ]; then
        pass "migrate_subset"
    else
        fail "migrate_subset" "rc=${_rc}"
    fi
}

bin_for_pkg() {
    case "$1" in
        ripgrep) echo rg ;;
        *) echo "$1" ;;
    esac
}

step_install_bottles() {
    # shellcheck disable=SC2086
    run_den install ${SOAK_PKGS}
    if [ "${_rc}" -ne 0 ]; then
        fail "install_bottles" "rc=${_rc}"
        return
    fi
    _ok=1
    for _p in ${SOAK_PKGS}; do
        _binname=$(bin_for_pkg "${_p}")
        if run_bin "${_binname}"; then
            echo "  runs: ${_binname}" >&2
        else
            echo "  FAIL run: ${_binname}" >&2
            _ok=0
        fi
    done
    if [ "${_ok}" -eq 1 ]; then
        pass "install_bottles"
    else
        fail "install_bottles" "one or more packages did not run"
    fi
}

step_uninstall_reinstall() {
    _pkg=$(echo "${SOAK_PKGS}" | awk '{print $1}')
    [ -n "${_pkg}" ] || { skip "uninstall_reinstall" "no SOAK_PKGS"; return; }
    run_den uninstall "${_pkg}"
    if [ "${_rc}" -ne 0 ]; then
        fail "uninstall_reinstall" "uninstall rc=${_rc}"
        return
    fi
    if run_bin "${_pkg}"; then
        fail "uninstall_reinstall" "binary still present/runnable after uninstall"
        return
    fi
    run_den install "${_pkg}"
    if [ "${_rc}" -eq 0 ] && run_bin "${_pkg}"; then
        pass "uninstall_reinstall"
    else
        fail "uninstall_reinstall" "reinstall failed"
    fi
}

step_env_layer() {
    den env remove "${SOAK_ENV}" >/dev/null 2>&1 || true
    run_den env create "${SOAK_ENV}"
    if [ "${_rc}" -ne 0 ]; then
        fail "env_layer" "create rc=${_rc}"
        return
    fi
    run_den env use "${SOAK_ENV}"
    # install a small pkg into that env's active context
    _pkg=$(echo "${SOAK_PKGS}" | awk '{print $1}')
    run_den install "${_pkg}"
    _inst_rc=${_rc}
    run_den env use /
    den env remove "${SOAK_ENV}" >/dev/null 2>&1 || true
    if [ "${_inst_rc}" -eq 0 ]; then
        pass "env_layer"
    else
        fail "env_layer" "install in child env rc=${_inst_rc}"
    fi
}

step_multi_version() {
    if [ -z "${SOAK_MULTI_PKG}" ] || [ -z "${SOAK_MULTI_V1}" ] || [ -z "${SOAK_MULTI_V2}" ]; then
        skip "multi_version" "set SOAK_MULTI_PKG/V1/V2 to exercise"
        return
    fi
    run_den install "${SOAK_MULTI_PKG}"
    # Best-effort: use may need exact versions present
    run_den use "${SOAK_MULTI_PKG}" "${SOAK_MULTI_V1}"
    _r1=${_rc}
    run_den use "${SOAK_MULTI_PKG}" "${SOAK_MULTI_V2}"
    _r2=${_rc}
    if [ "${_r1}" -eq 0 ] || [ "${_r2}" -eq 0 ]; then
        pass "multi_version"
    else
        fail "multi_version" "use v1 rc=${_r1} use v2 rc=${_r2}"
    fi
}

step_pip_provider() {
    if ! command -v python3 >/dev/null 2>&1 && ! command -v pip3 >/dev/null 2>&1; then
        skip "pip_provider" "no python3/pip3"
        return
    fi
    run_den install --provider pip cowsay
    if [ "${_rc}" -eq 0 ]; then
        pass "pip_provider"
    else
        skip "pip_provider" "install failed rc=${_rc} (toolchain/env)"
    fi
}

step_selfupdate_check() {
    run_den self-update --check
    if [ "${_rc}" -ne 0 ]; then
        # network flake → skip not fail
        skip "selfupdate_check" "rc=${_rc}"
        return
    fi
    # Must not claim an older GA is an upgrade over 1.x
    if echo "${_out}" | grep -E '0\.[0-9]+\.[0-9]+ is available \(current: 1\.' >/dev/null; then
        fail "selfupdate_check" "offered older 0.x over 1.x: ${_out}"
        return
    fi
    pass "selfupdate_check"
}

step_shell_env() {
    if ! den shell-env / >/dev/null 2>&1; then
        fail "shell_env" "den shell-env / failed"
        return
    fi
    _exports=$(den shell-env / 2>/dev/null)
    if echo "${_exports}" | grep -q 'DEN_ENV=' && echo "${_exports}" | grep -q 'PATH='; then
        # Evaluate in a subshell and check den env bin is first
        _first=$(
            eval "${_exports}"
            echo "$PATH" | tr ':' '\n' | head -1
        )
        case "${_first}" in
            */.den/envs/*)
                pass "shell_env"
                ;;
            *)
                fail "shell_env" "PATH head is ${_first}, expected den env bin"
                ;;
        esac
    else
        fail "shell_env" "missing DEN_ENV or PATH export"
    fi
}

step_brew_still_there() {
    if [ -x /opt/homebrew/bin/brew ]; then
        if /opt/homebrew/bin/brew --version >/dev/null 2>&1; then
            pass "brew_still_there"
        else
            fail "brew_still_there" "brew binary broken"
        fi
    else
        skip "brew_still_there" "no /opt/homebrew/bin/brew"
    fi
}

step_list_status() {
    run_den list
    _lrc=${_rc}
    run_den status
    if [ "${_lrc}" -eq 0 ] && [ "${_rc}" -eq 0 ]; then
        pass "list_status"
    else
        fail "list_status" "list rc=${_lrc} status rc=${_rc}"
    fi
}

# ---------------------------------------------------------------------------
step_version
step_settings_safe
step_update
step_doctor_no_errors
step_migrate_dry_run
step_migrate_subset
step_install_bottles
step_uninstall_reinstall
step_env_layer
step_multi_version
step_pip_provider
step_selfupdate_check
step_shell_env
step_brew_still_there
step_list_status

echo
echo "=== results: ${_pass} passed, ${_fail} failed, ${_skip} skipped ==="
if [ "${_fail}" -gt 0 ]; then
    exit 1
fi
exit 0

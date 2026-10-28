#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Remote-side sandbox helper for the macOS harness (run.sh). It is copied to
# (or piped into) the test host and keeps everything a harness run writes
# inside the per-run sandbox directory.
#
# Usage:
#   sandbox.sh run DIR -- CMD [ARGS...]
#       Exec CMD with toolchain caches, temp files and Go telemetry redirected
#       into DIR (see sandbox_env below). Creates the directories it needs.
#   sandbox.sh cleanup DIR
#       chmod -R u+w DIR, rm -rf DIR, then verify DIR is really gone. Exits
#       non-zero with a message on stderr if anything is left behind.
#
# DIR must be HARNESS_SANDBOX_ROOT (default /tmp/den-harness) or a path below
# it, made only of [A-Za-z0-9._/-] and without "..". `cleanup` refuses
# anything else, so a bad argument can never become `rm -rf $HOME`.
#
# Why each variable is set (all observed leaking out of a sandboxed run):
#   TMPDIR               mktemp, xcrun's xcrun_db, Node's default compile cache
#   NODE_COMPILE_CACHE   Node's on-disk compile cache (explicit, belt and braces)
#   npm_config_cache     ~/.npm/_cacache, ~/.npm/_logs
#   npm_config_update_notifier=false   ~/.npm/_update-notifier-last-checked
#   UV_CACHE_DIR         ~/.cache/uv
#   PIP_CACHE_DIR        pip's cache (pip provider falls back to pip if no uv)
#   XDG_CACHE_HOME, XDG_DATA_HOME, XDG_STATE_HOME   other XDG-aware tools
#   GOCACHE              ~/Library/Caches/go-build
#   GOMODCACHE, GOPATH   direct go use (den's go provider sets its own, also
#                        inside DIR)
#   CARGO_HOME           direct cargo use (den's cargo provider sets its own)
#   RUSTUP_HOME          pinned to the host's real value so rustup's cargo
#                        proxy still finds its toolchains (read only)
#
# Go telemetry: `go` writes counters to os.UserConfigDir()/go/telemetry
# (~/Library/Application Support/go/telemetry on macOS). GOTELEMETRY and
# GOTELEMETRYDIR are read-only `go env` values and cannot redirect or disable
# this from the environment, and `go telemetry off` would itself write to the
# real directory. So `run` puts a `go` shim first on PATH that execs the real
# go with HOME=DIR/.harness-env/home, which moves the telemetry dir into the sandbox. GOENV
# is pinned to the host's real go env file so `go env -w` settings still apply.
#
# Not redirectable from the environment: macOS's per-user cache dir
# (DARWIN_USER_CACHE_DIR, e.g. com.apple.DeveloperTools written by xcrun).

set -eu

HARNESS_SANDBOX_ROOT="${HARNESS_SANDBOX_ROOT:-/tmp/den-harness}"

sandbox_die() {
    printf 'sandbox.sh: error: %s\n' "$*" >&2
    exit 1
}

# sandbox_check_path DIR — DIR must be the sandbox root or below it.
sandbox_check_path() {
    _p="$1"
    case "$_p" in
        "") sandbox_die "empty sandbox path" ;;
        *[!A-Za-z0-9._/-]*) sandbox_die "unsafe characters in sandbox path: $_p" ;;
        *..*) sandbox_die "'..' not allowed in sandbox path: $_p" ;;
    esac
    case "$_p" in
        "$HARNESS_SANDBOX_ROOT" | "$HARNESS_SANDBOX_ROOT"/?*) ;;
        *) sandbox_die "refusing path outside $HARNESS_SANDBOX_ROOT: $_p" ;;
    esac
}

# sandbox_go_env_file — the host's default GOENV, computed without running go
# (running go here would write a telemetry counter outside the sandbox).
sandbox_go_env_file() {
    if [ -n "${GOENV:-}" ]; then
        printf '%s\n' "$GOENV"
    elif [ "$(uname -s)" = Darwin ]; then
        printf '%s\n' "$HOME/Library/Application Support/go/env"
    else
        printf '%s\n' "${XDG_CONFIG_HOME:-$HOME/.config}/go/env"
    fi
}

# sandbox_env DIR — export the redirections and install the go shim.
sandbox_env() {
    _d="$1"
    sandbox_check_path "$_d"

    _real_go=$(command -v go 2>/dev/null || true)
    _go_env=$(sandbox_go_env_file)
    _rustup_home="${RUSTUP_HOME:-$HOME/.rustup}"

    # Everything lives under DIR/.harness-env so it stays out of den's own
    # DEN_HOME layout (DIR is also DEN_HOME); den's providers still point Go
    # and Cargo at DIR/cache themselves when den runs them.
    _e="$_d/.harness-env"
    mkdir -p "$_e/tmp" "$_e/home" "$_e/shims" "$_e/cache/node-compile-cache" \
        "$_e/cache/npm" "$_e/cache/uv" "$_e/cache/pip" "$_e/cache/xdg" \
        "$_e/share/xdg" "$_e/state/xdg" "$_e/cache/go/build" \
        "$_e/cache/go/modcache" "$_e/cache/go/gopath" "$_e/cache/cargo"

    export TMPDIR="$_e/tmp"
    export NODE_COMPILE_CACHE="$_e/cache/node-compile-cache"
    export npm_config_cache="$_e/cache/npm"
    export npm_config_update_notifier=false
    export UV_CACHE_DIR="$_e/cache/uv"
    export PIP_CACHE_DIR="$_e/cache/pip"
    export XDG_CACHE_HOME="$_e/cache/xdg"
    export XDG_DATA_HOME="$_e/share/xdg"
    export XDG_STATE_HOME="$_e/state/xdg"
    export GOCACHE="$_e/cache/go/build"
    export GOMODCACHE="$_e/cache/go/modcache"
    export GOPATH="$_e/cache/go/gopath"
    export CARGO_HOME="$_e/cache/cargo"
    export RUSTUP_HOME="$_rustup_home"

    if [ -n "$_real_go" ]; then
        # _d is restricted to [A-Za-z0-9._/-]; the other two values are
        # host paths, so quote them for the shim's single-quoted literals.
        _q_go=$(printf '%s' "$_real_go" | sed "s/'/'\\\\''/g")
        _q_env=$(printf '%s' "$_go_env" | sed "s/'/'\\\\''/g")
        cat > "$_e/shims/go" <<SHIM
#!/bin/sh
# Generated by tests/harness/macos/sandbox.sh: keep go telemetry in the sandbox.
HOME='$_e/home' GOENV='$_q_env' exec '$_q_go' "\$@"
SHIM
        chmod +x "$_e/shims/go"
        export PATH="$_e/shims:$PATH"
    fi
}

# sandbox_cleanup DIR — make DIR writable, remove it, and prove it is gone.
sandbox_cleanup() {
    _d="$1"
    sandbox_check_path "$_d"
    if [ ! -e "$_d" ] && [ ! -L "$_d" ]; then
        return 0
    fi
    # Go's module cache (and some npm/uv entries) are read-only; plain rm -rf
    # fails on them, so restore owner write permission first.
    chmod -R u+w "$_d" 2>/dev/null || true
    _rm_rc=0
    rm -rf "$_d" || _rm_rc=$?
    if [ -e "$_d" ] || [ -L "$_d" ]; then
        printf 'sandbox.sh: error: failed to remove %s (rm exit %d); left behind:\n' \
            "$_d" "$_rm_rc" >&2
        find "$_d" -maxdepth 3 2>/dev/null | head -n 20 >&2 || true
        return 1
    fi
    if [ "$_rm_rc" -ne 0 ]; then
        printf 'sandbox.sh: error: rm -rf %s exited %d\n' "$_d" "$_rm_rc" >&2
        return 1
    fi
}

sandbox_main() {
    [ $# -ge 2 ] || sandbox_die "usage: sandbox.sh run DIR -- CMD... | sandbox.sh cleanup DIR"
    _cmd="$1"
    _dir="$2"
    shift 2
    case "$_cmd" in
        run)
            [ "${1:-}" = "--" ] || sandbox_die "run: expected -- before the command"
            shift
            [ $# -ge 1 ] || sandbox_die "run: no command given"
            sandbox_env "$_dir"
            exec "$@"
            ;;
        cleanup)
            [ $# -eq 0 ] || sandbox_die "cleanup: unexpected arguments: $*"
            sandbox_cleanup "$_dir"
            ;;
        *)
            sandbox_die "unknown subcommand: $_cmd"
            ;;
    esac
}

# Run main unless sourced for tests (SANDBOX_SH_NO_MAIN=1).
if [ "${SANDBOX_SH_NO_MAIN:-0}" != 1 ]; then
    sandbox_main "$@"
fi

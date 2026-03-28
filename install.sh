#!/bin/sh
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# den installer — https://github.com/marcelocantos/den
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/marcelocantos/den/master/install.sh | sh
#
# Environment variables:
#   DEN_VERSION          Pin a specific version (default: latest)
#   DEN_INSTALL_DIR      Override install directory (default: ~/.den)
#   DEN_NO_MODIFY_PATH   Set to 1 to skip shell profile modification
#
set -eu

main() {
    check_cmd curl || check_cmd wget || err "need curl or wget"
    need_cmd tar
    need_cmd uname

    # --- Platform detection ---
    _os="$(uname -s)"
    _arch="$(uname -m)"

    case "$_os" in
        Linux)  os=linux ;;
        Darwin) os=darwin ;;
        *)      err "unsupported OS: $_os" ;;
    esac

    case "$_arch" in
        x86_64|amd64)  arch=x86_64 ;;
        aarch64|arm64) arch=aarch64 ;;
        *)             err "unsupported architecture: $_arch" ;;
    esac

    # --- Configuration ---
    den_dir="${DEN_INSTALL_DIR:-$HOME/.den}"
    bin_dir="$den_dir/bin"
    version="${DEN_VERSION:-}"

    # --- Resolve version ---
    if [ -z "$version" ]; then
        say "finding latest release..."
        version="$(latest_version)"
        if [ -z "$version" ]; then
            err "could not determine latest version"
        fi
    fi

    # Validate version format (vN.N.N or N.N.N).
    case "$version" in
        v[0-9]*.[0-9]*.[0-9]*) ;;
        [0-9]*.[0-9]*.[0-9]*)  ;;
        *) err "invalid version format: $version (expected vN.N.N)" ;;
    esac

    say "installing den $version for $os/$arch"

    # --- Check for existing install ---
    if [ -x "$bin_dir/den" ]; then
        existing="$("$bin_dir/den" --version 2>/dev/null | awk '{print $2}')" || existing=""
        if [ "$existing" = "$version" ]; then
            say "den $version is already installed."
            exit 0
        fi
        say "upgrading den $existing -> $version"
    fi

    # --- Download ---
    tmpdir="$(mktemp -d)"
    trap 'rm -rf "$tmpdir"' EXIT

    archive_name="den-${version}-${os}-${arch}.tar.gz"
    checksum_name="den-${version}-checksums.txt"
    base_url="https://github.com/marcelocantos/den/releases/download/v${version}"

    say "downloading $archive_name..."
    download "$base_url/$archive_name" "$tmpdir/$archive_name"
    download "$base_url/$checksum_name" "$tmpdir/$checksum_name"

    # --- Verify checksum ---
    say "verifying checksum..."
    verify_checksum "$tmpdir/$archive_name" "$tmpdir/$checksum_name" "$archive_name"

    # --- Extract and install ---
    mkdir -p "$bin_dir"
    tar xzf "$tmpdir/$archive_name" -C "$tmpdir"

    if [ -f "$tmpdir/den" ]; then
        mv "$tmpdir/den" "$bin_dir/den"
    elif [ -f "$tmpdir/den-${version}-${os}-${arch}/den" ]; then
        mv "$tmpdir/den-${version}-${os}-${arch}/den" "$bin_dir/den"
    else
        err "binary not found in archive"
    fi

    chmod +x "$bin_dir/den"

    # Clear macOS quarantine flag.
    if [ "$os" = "darwin" ]; then
        xattr -d com.apple.quarantine "$bin_dir/den" 2>/dev/null || true
    fi

    # --- Verify it works ---
    installed_version="$("$bin_dir/den" --version 2>/dev/null)" || err "installed binary failed to run"
    say "$installed_version"

    # --- Shell integration ---
    if [ "${DEN_NO_MODIFY_PATH:-}" = "1" ]; then
        say "skipping shell profile modification (DEN_NO_MODIFY_PATH=1)"
    else
        add_shell_integration "$bin_dir"
    fi

    # --- Done ---
    say ""
    say "den installed to $bin_dir/den"
    say ""
    say "restart your shell, then get started:"
    say "  den migrate              # import existing Homebrew packages"
    say "  den daemon install       # enable background maintenance"
    say "  den --help               # see all commands"
}

latest_version() {
    # Use the GitHub releases redirect to avoid API rate limits.
    if check_cmd curl; then
        curl -fsSL -o /dev/null -w '%{redirect_url}' \
            "https://github.com/marcelocantos/den/releases/latest" 2>/dev/null \
            | sed 's|.*/v||'
    elif check_cmd wget; then
        wget --spider -S -O /dev/null \
            "https://github.com/marcelocantos/den/releases/latest" 2>&1 \
            | grep -i 'Location:' | sed 's|.*/v||' | tr -d '[:space:]'
    fi
}

download() {
    _url="$1"
    _dest="$2"
    if check_cmd curl; then
        curl -fsSL "$_url" -o "$_dest"
    elif check_cmd wget; then
        wget -qO "$_dest" "$_url"
    else
        err "need curl or wget"
    fi
}

verify_checksum() {
    _file="$1"
    _checksum_file="$2"
    _archive_name="$3"

    _expected="$(grep -F "$_archive_name" "$_checksum_file" | awk '{print $1}')"
    if [ -z "$_expected" ]; then
        err "checksum not found for $_archive_name in checksum file"
    fi

    if check_cmd shasum; then
        _actual="$(shasum -a 256 "$_file" | awk '{print $1}')"
    elif check_cmd sha256sum; then
        _actual="$(sha256sum "$_file" | awk '{print $1}')"
    else
        say "warning: cannot verify checksum (no shasum or sha256sum)"
        return
    fi

    if [ "$_actual" != "$_expected" ]; then
        err "checksum mismatch: expected $_expected, got $_actual"
    fi
}

add_shell_integration() {
    _bin_dir="$1"
    _init_line="eval \"\$(\\\"$_bin_dir/den\\\" init)\""

    # Detect shell and profile.
    _shell="$(basename "${SHELL:-/bin/sh}")"
    case "$_shell" in
        zsh)  _profile="$HOME/.zshrc" ;;
        bash)
            if [ -f "$HOME/.bash_profile" ]; then
                _profile="$HOME/.bash_profile"
            else
                _profile="$HOME/.bashrc"
            fi
            ;;
        fish) _profile="$HOME/.config/fish/config.fish" ;;
        *)    _profile="$HOME/.profile" ;;
    esac

    # Check if already present.
    if [ -f "$_profile" ] && grep -qF 'den init' "$_profile"; then
        say "shell integration already in $_profile"
        return
    fi

    # Append shell-appropriate init line.
    if [ "$_shell" = "fish" ]; then
        printf '\n# den — universal development environment manager\n%s/den init --shell fish | source\n' "$_bin_dir" >> "$_profile"
    else
        printf '\n# den — universal development environment manager\neval "$("%s/den" init)"\n' "$_bin_dir" >> "$_profile"
    fi
    say "added shell integration to $_profile"
}

say() {
    printf 'den: %s\n' "$1"
}

err() {
    say "error: $1" >&2
    exit 1
}

check_cmd() {
    command -v "$1" >/dev/null 2>&1
}

need_cmd() {
    if ! check_cmd "$1"; then
        err "need '$1' (command not found)"
    fi
}

main "$@"

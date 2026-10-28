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
#   DEN_VERSION          Pin a release: 0.14.0, v0.14.0, 0.15.0-rc.1 or
#                        v0.15.0-rc.1 (default: latest GA)
#   DEN_INSTALL_DIR      Override install directory (default: $DEN_HOME, else ~/.den)
#   DEN_HOME             den's home, as den itself reads it (default: ~/.den)
#   DEN_PREFIX           den's package prefix, as den itself reads it
#                        (default: $DEN_HOME/prefix). Created if missing; a
#                        prefix you cannot create (e.g. /opt/homebrew) needs
#                        sudo, which is offered interactively, never run
#                        silently.
#   DEN_INSTALL_INTERACTIVE  1 to prompt, 0 to never prompt (default: prompt
#                        when a terminal is available)
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
    den_home="${DEN_HOME:-$HOME/.den}"
    den_dir="${DEN_INSTALL_DIR:-$den_home}"
    bin_dir="$den_dir/bin"
    # Same resolution order den uses when DEN_PREFIX is set or defaulted (a
    # store.prefix in config.json is den's own setting and not consulted).
    prefix="${DEN_PREFIX:-$den_home/prefix}"
    version="${DEN_VERSION:-}"

    # A sha256 tool is mandatory: never install unverified bytes.
    sha256_tool >/dev/null || err "need 'shasum' or 'sha256sum' to verify the download"

    # --- Resolve version ---
    if [ -z "$version" ]; then
        say "finding latest release..."
        version="$(latest_version)"
        if [ -z "$version" ]; then
            err "could not determine latest version"
        fi
    fi

    # Accept N.N.N or N.N.N-rc.N, each with an optional leading v.
    version="${version#v}"
    case "$version" in
        *[!0-9.a-z-]*) err "invalid version format: $version (expected N.N.N or N.N.N-rc.N)" ;;
    esac
    _base="${version%%-*}"
    _rest="${version#"$_base"}"
    is_valid_base "$_base" || err "invalid version format: $version (expected N.N.N or N.N.N-rc.N)"
    case "$_rest" in
        "") ;;
        -rc.*)
            _rc="${_rest#-rc.}"
            case "$_rc" in
                ""|*[!0-9]*) err "invalid version format: $version (expected N.N.N or N.N.N-rc.N)" ;;
            esac
            ;;
        *) err "invalid version format: $version (expected N.N.N or N.N.N-rc.N)" ;;
    esac
    tag="v$version"
    # Release candidates ship GA-named assets (den-<base>-...) and the binary
    # reports the base version.
    asset_version="$_base"

    say "installing den $tag for $os/$arch"

    # --- Check for existing install ---
    # `den --version` prints the bare version (e.g. 0.14.0), which an RC
    # shares with its GA, so the installed tag is recorded beside it.
    tag_file="$den_dir/share/den/installed-tag"
    if [ -x "$bin_dir/den" ]; then
        existing="$("$bin_dir/den" --version 2>/dev/null)" || existing=""
        existing_tag=""
        if [ -f "$tag_file" ]; then
            existing_tag="$(cat "$tag_file" 2>/dev/null)" || existing_tag=""
        fi
        if [ "$existing" = "$asset_version" ]; then
            if [ "$existing_tag" = "$tag" ] || { [ -z "$existing_tag" ] && [ "$tag" = "v$asset_version" ]; }; then
                ensure_prefix "$prefix"
                say "den $tag is already installed."
                exit 0
            fi
        fi
        say "replacing den ${existing_tag:-${existing:-unknown}} -> $tag"
    fi

    # --- Prefix ---
    ensure_prefix "$prefix"

    # --- Download ---
    tmpdir="$(mktemp -d)"
    trap 'rm -rf "$tmpdir"' EXIT

    archive_name="den-${asset_version}-${os}-${arch}.tar.gz"
    checksum_name="den-${asset_version}-checksums.txt"
    base_url="https://github.com/marcelocantos/den/releases/download/${tag}"

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
        pkg_root="$tmpdir"
    elif [ -f "$tmpdir/den-${asset_version}-${os}-${arch}/den" ]; then
        pkg_root="$tmpdir/den-${asset_version}-${os}-${arch}"
    else
        err "binary not found in archive"
    fi
    mv "$pkg_root/den" "$bin_dir/den"

    # Trust replica snapshot (🎯T42): den resolves it at
    # <bin>/../share/den/known_hashes.json. Older archives don't carry it;
    # den then falls back to the copy compiled into the binary.
    if [ -f "$pkg_root/share/den/known_hashes.json" ]; then
        mkdir -p "$den_dir/share/den"
        cp "$pkg_root/share/den/known_hashes.json" "$den_dir/share/den/known_hashes.json"
    fi

    chmod +x "$bin_dir/den"
    mkdir -p "$den_dir/share/den"
    printf '%s\n' "$tag" > "$tag_file"

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
    if [ -n "${DEN_PREFIX:-}" ] && [ "$DEN_PREFIX" != "$den_home/prefix" ]; then
        say ""
        say "den reads its prefix from DEN_PREFIX; keep it set in your shell profile:"
        say "  export DEN_PREFIX=\"$DEN_PREFIX\""
    fi
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

    _expected="$(awk -v n="$_archive_name" '$2 == n || $2 == "*" n {print $1; exit}' "$_checksum_file")"
    if [ -z "$_expected" ]; then
        err "checksum not found for $_archive_name in checksum file"
    fi

    _tool="$(sha256_tool)" || err "need 'shasum' or 'sha256sum' to verify the download"
    case "$_tool" in
        shasum) _actual="$(shasum -a 256 "$_file" | awk '{print $1}')" ;;
        *)      _actual="$(sha256sum "$_file" | awk '{print $1}')" ;;
    esac

    if [ "$_actual" != "$_expected" ]; then
        err "checksum mismatch: expected $_expected, got $_actual"
    fi
}

sha256_tool() {
    if check_cmd shasum; then
        echo shasum
    elif check_cmd sha256sum; then
        echo sha256sum
    else
        return 1
    fi
}

is_valid_base() {
    # N.N.N, digits only.
    case "$1" in
        *[!0-9.]*|.*|*.|*..*) return 1 ;;
    esac
    _IFS_save="$IFS"
    IFS=.
    # shellcheck disable=SC2086
    set -- $1
    IFS="$_IFS_save"
    [ $# -eq 3 ]
}

is_interactive() {
    case "${DEN_INSTALL_INTERACTIVE:-}" in
        1) return 0 ;;
        0) return 1 ;;
    esac
    # `curl ... | sh` has a pipe on stdin, but the terminal is still there.
    (: <"${DEN_INSTALL_TTY:-/dev/tty}") 2>/dev/null
}

ensure_prefix() {
    _prefix="$1"
    if [ -d "$_prefix" ] && [ -w "$_prefix" ]; then
        return
    fi
    if [ ! -e "$_prefix" ]; then
        # Find the nearest existing ancestor; if we can write there, no
        # privileges are needed.
        _anc="$(dirname "$_prefix")"
        while [ ! -e "$_anc" ]; do
            _anc="$(dirname "$_anc")"
        done
        if [ -d "$_anc" ] && [ -w "$_anc" ]; then
            mkdir -p "$_prefix" || err "could not create prefix $_prefix"
            say "created prefix $_prefix"
            return
        fi
    fi

    _user="$(id -un)"
    if [ "$os" = "darwin" ]; then
        _group="admin"
    else
        _group="$(id -gn)"
    fi
    _cmd="sudo mkdir -p $_prefix && sudo chown $_user:$_group $_prefix"

    say "den's prefix $_prefix does not exist or is not writable by $_user."
    say "den installs packages there without sudo, so it must be created once and"
    say "owned by you. The command is:"
    say "  $_cmd"
    if ! is_interactive; then
        err "run the command above (or set DEN_PREFIX to a directory you own), then re-run the installer"
    fi
    printf 'den: run it now with sudo? [Y/n] ' >&2
    _answer=""
    read -r _answer < "${DEN_INSTALL_TTY:-/dev/tty}" || _answer=n
    case "$_answer" in
        ""|y|Y|yes|YES|Yes) ;;
        *) err "not creating $_prefix; run the command above (or set DEN_PREFIX), then re-run the installer" ;;
    esac
    sudo mkdir -p "$_prefix" || err "sudo mkdir -p $_prefix failed"
    sudo chown "$_user:$_group" "$_prefix" || err "sudo chown $_user:$_group $_prefix failed"
    [ -d "$_prefix" ] && [ -w "$_prefix" ] || err "prefix $_prefix is still not writable"
    say "created prefix $_prefix"
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

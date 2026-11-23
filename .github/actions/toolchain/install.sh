#!/usr/bin/env bash
# The *_VERSION / *_SHA256_* variables come from versions.env.
# shellcheck disable=SC2154
# Install pinned, SHA-256-verified official release builds of the requested
# tools (cmake, ninja, hyperfine) into <dest> and put them on PATH.
# Versions and hashes come from versions.env next to this script.
#
#   install.sh [--dest DIR] TOOL...
#
# On GitHub Actions the bin directories are appended to $GITHUB_PATH; dest
# defaults to $RUNNER_TEMP/toolchain. Supported: macOS (arm64/x86_64) and
# Linux x86_64/aarch64. Anything else, an unknown tool or a hash mismatch
# fails the step.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=/dev/null
. "$here/versions.env"

dest="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/toolchain"
if [[ "${1:-}" == --dest ]]; then dest=$2; shift 2; fi
[[ $# -gt 0 ]] || { echo "usage: install.sh [--dest DIR] cmake|ninja|hyperfine..." >&2; exit 2; }

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64)   plat=macos;  arch=arm64 ;;
    Darwin-x86_64)  plat=macos;  arch=x86_64 ;;
    Linux-x86_64)   plat=linux;  arch=x86_64 ;;
    Linux-aarch64|Linux-arm64) plat=linux; arch=aarch64 ;;
    *) echo "::error::toolchain: unsupported platform $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

# fetch URL SHA256 FILE: download with retries, verify, or fail.
fetch() {
    local url=$1 want=$2 out=$3 got
    [[ -n "$want" ]] || { echo "::error::toolchain: no pinned hash for $url" >&2; exit 1; }
    curl -fsSL --retry 5 --retry-delay 2 --retry-all-errors -o "$out" "$url"
    got=$(sha256 "$out")
    if [[ "$got" != "$want" ]]; then
        echo "::error::toolchain: SHA-256 mismatch for $url: expected $want, got $got" >&2
        rm -f "$out"
        exit 1
    fi
    echo "verified $(basename "$out") sha256=$got"
}

mkdir -p "$dest/dl"
bins=()
for tool in "$@"; do
    case "$tool" in
    cmake)
        v=$CMAKE_VERSION
        if [[ $plat == macos ]]; then
            name="cmake-$v-macos-universal"; sha=$CMAKE_SHA256_macos_universal
            bin="$dest/$name/CMake.app/Contents/bin"
        else
            name="cmake-$v-linux-$arch"; key="CMAKE_SHA256_linux_$arch"; sha=${!key:-}
            bin="$dest/$name/bin"
        fi
        fetch "https://github.com/Kitware/CMake/releases/download/v$v/$name.tar.gz" "$sha" "$dest/dl/$name.tar.gz"
        tar -xzf "$dest/dl/$name.tar.gz" -C "$dest"
        ;;
    ninja)
        v=$NINJA_VERSION
        if [[ $plat == macos ]]; then
            asset=ninja-mac.zip; sha=$NINJA_SHA256_macos_universal
        elif [[ $arch == x86_64 ]]; then
            asset=ninja-linux.zip; sha=$NINJA_SHA256_linux_x86_64
        else
            asset=ninja-linux-aarch64.zip; sha=$NINJA_SHA256_linux_aarch64
        fi
        fetch "https://github.com/ninja-build/ninja/releases/download/v$v/$asset" "$sha" "$dest/dl/ninja-$v-$asset"
        bin="$dest/ninja-$v/bin"
        mkdir -p "$bin"
        unzip -oq "$dest/dl/ninja-$v-$asset" -d "$bin"
        chmod +x "$bin/ninja"
        ;;
    hyperfine)
        v=$HYPERFINE_VERSION
        case "$plat-$arch" in
            macos-arm64)  triple=aarch64-apple-darwin;      sha=$HYPERFINE_SHA256_macos_arm64 ;;
            linux-x86_64) triple=x86_64-unknown-linux-gnu;  sha=$HYPERFINE_SHA256_linux_x86_64 ;;
            linux-aarch64) triple=aarch64-unknown-linux-gnu; sha=$HYPERFINE_SHA256_linux_aarch64 ;;
            *) echo "::error::toolchain: no pinned hyperfine for $plat-$arch" >&2; exit 1 ;;
        esac
        name="hyperfine-v$v-$triple"
        fetch "https://github.com/sharkdp/hyperfine/releases/download/v$v/$name.tar.gz" "$sha" "$dest/dl/$name.tar.gz"
        tar -xzf "$dest/dl/$name.tar.gz" -C "$dest"
        bin="$dest/$name"
        ;;
    *)
        echo "::error::toolchain: unknown tool '$tool'" >&2; exit 2 ;;
    esac
    bins+=("$bin")
done

for b in "${bins[@]}"; do
    [[ -n "${GITHUB_PATH:-}" ]] && echo "$b" >> "$GITHUB_PATH"
    echo "bin: $b"
done
# Report what was installed, from the installed binaries themselves.
for tool in "$@"; do
    for b in "${bins[@]}"; do
        if [[ -x "$b/$tool" ]]; then "$b/$tool" --version | head -n 1; break; fi
    done
done

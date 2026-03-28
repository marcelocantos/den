// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use crate::config::Config;
use crate::manifest;

pub(super) fn print_shell_init(config: &Config, shell: &str) {
    // Shell-escape the path to prevent injection via DEN_HOME.
    let den_home = config
        .den_home
        .display()
        .to_string()
        .replace('\\', "\\\\")
        .replace('"', "\\\"")
        .replace('$', "\\$")
        .replace('`', "\\`")
        .replace('!', "\\!");
    let root_slug = manifest::env_slug("/");
    match shell {
        "zsh" | "bash" => {
            print!(
                r#"# den shell integration
export DEN_HOME="{den_home}"
export DEN_ENV="/"

_den_env="{den_home}/envs/{root_slug}"

# Add den binary and root environment to PATH (before Homebrew).
export PATH="{den_home}/bin:$_den_env/bin:$PATH"

# Build environment — headers, libraries, pkg-config.
export LIBRARY_PATH="$_den_env/lib${{LIBRARY_PATH:+:$LIBRARY_PATH}}"
export CPATH="$_den_env/include${{CPATH:+:$CPATH}}"
export PKG_CONFIG_PATH="$_den_env/lib/pkgconfig:$_den_env/share/pkgconfig${{PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}}"
export CMAKE_PREFIX_PATH="$_den_env${{CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}}"
export MANPATH="$_den_env/share/man${{MANPATH:+:$MANPATH}}:"
export INFOPATH="$_den_env/share/info${{INFOPATH:+:$INFOPATH}}"

unset _den_env

# Shell function wrapping `den env use` so it can modify the current shell.
den() {{
    if [ "$1" = "env" ] && [ "$2" = "use" ] && [ -n "$3" ]; then
        local _den_output
        _den_output="$(command den env use "$3" 2>&1)"
        local _den_rc=$?
        if [ $_den_rc -eq 0 ]; then
            eval "$_den_output"
        else
            echo "$_den_output" >&2
            return $_den_rc
        fi
    else
        command den "$@"
    fi
}}
"#
            );
        }
        "fish" => {
            print!(
                r#"# den shell integration
set -gx DEN_HOME "{den_home}"
set -gx DEN_ENV "/"
set -l _den_env "{den_home}/envs/{root_slug}"

# Add den binary and root environment to PATH.
fish_add_path --prepend "{den_home}/bin" "$_den_env/bin"

# Build environment — headers, libraries, pkg-config.
set -gx LIBRARY_PATH "$_den_env/lib" $LIBRARY_PATH
set -gx CPATH "$_den_env/include" $CPATH
set -gx PKG_CONFIG_PATH "$_den_env/lib/pkgconfig" "$_den_env/share/pkgconfig" $PKG_CONFIG_PATH
set -gx CMAKE_PREFIX_PATH "$_den_env" $CMAKE_PREFIX_PATH
set -gx MANPATH "$_den_env/share/man" $MANPATH
set -gx INFOPATH "$_den_env/share/info" $INFOPATH

# Wrapper function for `den env use`.
function den
    if test (count $argv) -ge 3; and test "$argv[1]" = "env"; and test "$argv[2]" = "use"
        set -l output (command den env use $argv[3] 2>&1)
        set -l rc $status
        if test $rc -eq 0
            eval $output
        else
            echo $output >&2
            return $rc
        end
    else
        command den $argv
    end
end
"#
            );
        }
        _ => {
            tracing::warn!("unsupported shell: {shell}. Use zsh, bash, or fish.");
        }
    }
}

pub(super) fn print_env_switch_commands(config: &Config, env_slug: &str) {
    let den_home = &config.den_home;
    let new_env = den_home.join("envs").join(env_slug);
    let env_path = manifest::slug_to_path(env_slug);
    let escape_for_shell = |s: &str| -> String {
        s.replace('\\', "\\\\")
            .replace('"', "\\\"")
            .replace('$', "\\$")
            .replace('`', "\\`")
            .replace('!', "\\!")
    };
    let dh = escape_for_shell(&den_home.display().to_string());
    let ne = escape_for_shell(&new_env.display().to_string());

    // Swap PATH, build env vars, and DEN_ENV in one eval.
    println!(
        r#"export PATH="$(echo "$PATH" | sed "s|{dh}/envs/[^:]*bin:||g")"
export PATH="{ne}/bin:$PATH"
export LIBRARY_PATH="$(echo "${{LIBRARY_PATH:-}}" | sed "s|{dh}/envs/[^:]*/lib:*||g")"
export LIBRARY_PATH="{ne}/lib${{LIBRARY_PATH:+:$LIBRARY_PATH}}"
export CPATH="$(echo "${{CPATH:-}}" | sed "s|{dh}/envs/[^:]*/include:*||g")"
export CPATH="{ne}/include${{CPATH:+:$CPATH}}"
export PKG_CONFIG_PATH="$(echo "${{PKG_CONFIG_PATH:-}}" | sed "s|{dh}/envs/[^:]*/lib/pkgconfig:*||g;s|{dh}/envs/[^:]*/share/pkgconfig:*||g")"
export PKG_CONFIG_PATH="{ne}/lib/pkgconfig:{ne}/share/pkgconfig${{PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}}"
export CMAKE_PREFIX_PATH="$(echo "${{CMAKE_PREFIX_PATH:-}}" | sed "s|{dh}/envs/[^:]*:*||g")"
export CMAKE_PREFIX_PATH="{ne}${{CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}}"
export MANPATH="$(echo "${{MANPATH:-}}" | sed "s|{dh}/envs/[^:]*/share/man:*||g")"
export MANPATH="{ne}/share/man${{MANPATH:+:$MANPATH}}:"
export INFOPATH="$(echo "${{INFOPATH:-}}" | sed "s|{dh}/envs/[^:]*/share/info:*||g")"
export INFOPATH="{ne}/share/info${{INFOPATH:+:$INFOPATH}}"
export DEN_ENV="{env_path}""#,
    );
}

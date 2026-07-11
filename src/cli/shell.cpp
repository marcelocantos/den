// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "shell.h"

#include "../env/environment.h"
#include "../env/manifest.h"

#include <spdlog/spdlog.h>

#include <iostream>
#include <stdexcept>
#include <string>

namespace den {

namespace {

// ---------------------------------------------------------------------------
// Shell-escape a path for use inside a double-quoted string.
// Characters that need escaping: \ " $ ` !
// ---------------------------------------------------------------------------
std::string shell_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '$':
            out += "\\$";
            break;
        case '`':
            out += "\\`";
            break;
        case '!':
            out += "\\!";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

// Quote a path for POSIX shells: wrap in double-quotes after escaping.
std::string dquote(const fs::path& p) {
    return "\"" + shell_escape(p.string()) + "\"";
}

// Quote a path for fish: wrap in single-quotes (no escaping needed for
// common paths; backslash and single-quote are escaped explicitly).
std::string fish_quote(const fs::path& p) {
    std::string s = p.string();
    std::string out;
    out.reserve(s.size() + 2);
    out += '\'';
    for (char c : s) {
        if (c == '\'')
            out += "\\'";
        else if (c == '\\')
            out += "\\\\";
        else
            out += c;
    }
    out += '\'';
    return out;
}

// Build the colon-separated PATH value:
//   <env_bin>:<den_bin>:<original $PATH>
std::string build_path_posix(const fs::path& env_bin, const fs::path& den_bin) {
    return shell_escape(env_bin.string()) + ":" + shell_escape(den_bin.string()) + ":$PATH";
}

std::string build_path_fish(const fs::path& env_bin, const fs::path& den_bin) {
    return fish_quote(env_bin) + " " + fish_quote(den_bin) + " $PATH";
}

// ---------------------------------------------------------------------------
// POSIX (bash/zsh) init script
// ---------------------------------------------------------------------------
void emit_posix_init(const Config& config, const std::string& shell) {
    const std::string den_bin = dquote(config.den_home / "bin");

    // Ensure den's own binary is on PATH before defining the wrapper
    // function. shell-env will later prepend the active environment's bin/
    // as well, but we need den itself to be findable first.
    std::cout << "# den shell integration (" << shell
              << ")\n"
                 "export PATH="
              << den_bin
              << ":\"$PATH\"\n"
                 "\n"
                 "den() {\n"
                 "  if [ \"$1\" = \"env\" ] && [ \"$2\" = \"use\" ]; then\n"
                 "    command den env use \"${3:-/}\"\n"
                 "    eval \"$(command den shell-env \"${3:-/}\")\"\n"
                 "  else\n"
                 "    command den \"$@\"\n"
                 "  fi\n"
                 "}\n"
                 "\n"
                 "# Activate the active den environment on shell startup.\n"
                 "if [ -z \"$DEN_ENV\" ]; then\n"
                 "  eval \"$(command den shell-env 2>/dev/null || true)\"\n"
                 "fi\n";
}

// ---------------------------------------------------------------------------
// Fish init script
// ---------------------------------------------------------------------------
void emit_fish_init(const Config& config) {
    std::cout
        << "# den shell integration (fish)\n"
           "set -gx PATH "
        << fish_quote(config.den_home / "bin")
        << " $PATH\n"
           "\n"
           "function den\n"
           "    if test (count $argv) -ge 2 -a \"$argv[1]\" = \"env\" -a \"$argv[2]\" = \"use\"\n"
           "        command den env use (count $argv -ge 3 && echo $argv[3] || echo /)\n"
           "        eval (command den shell-env (count $argv -ge 3 && echo $argv[3] || echo /))\n"
           "    else\n"
           "        command den $argv\n"
           "    end\n"
           "end\n"
           "\n"
           "# Activate the active den environment on shell startup.\n"
           "if not set -q DEN_ENV\n"
           "    eval (command den shell-env 2>/dev/null; or true)\n"
           "end\n";
}

// ---------------------------------------------------------------------------
// POSIX env-switch output
// ---------------------------------------------------------------------------
void emit_posix_env(const Config& config, const std::string& env_slug) {
    fs::path env_dir = config.den_home / "envs" / env_slug;
    fs::path env_bin = env_dir / "bin";
    fs::path env_lib = env_dir / "lib";
    fs::path env_inc = env_dir / "include";
    fs::path env_pc = env_dir / "lib" / "pkgconfig";
    fs::path env_cmake = env_dir;
    fs::path env_man = env_dir / "share" / "man";
    fs::path env_info = env_dir / "share" / "info";
    fs::path den_bin = config.den_home / "bin";

    auto e = [](const fs::path& p) { return shell_escape(p.string()); };

    std::cout << "export DEN_HOME=" << dquote(config.den_home) << "\n"
              << "export DEN_ENV=" << dquote(env_slug) << "\n"
              << "export PATH=\"" << build_path_posix(env_bin, den_bin) << "\"\n"
              << "export LIBRARY_PATH=\"" << e(env_lib) << ":${LIBRARY_PATH:-}\"\n"
#ifdef __APPLE__
              // Bottles often load via absolute /opt/homebrew/opt paths; when
              // den pours a newer keg than brew's opt link, fall back to the
              // env's lib/ (and the Cellar) so dyld can still resolve.
              << "export DYLD_FALLBACK_LIBRARY_PATH=\"" << e(env_lib)
              << ":/opt/homebrew/lib:${DYLD_FALLBACK_LIBRARY_PATH:-}\"\n"
#endif
              << "export CPATH=\"" << e(env_inc) << ":${CPATH:-}\"\n"
              << "export PKG_CONFIG_PATH=\"" << e(env_pc) << ":${PKG_CONFIG_PATH:-}\"\n"
              << "export CMAKE_PREFIX_PATH=\"" << e(env_cmake) << ":${CMAKE_PREFIX_PATH:-}\"\n"
              << "export MANPATH=\"" << e(env_man) << ":${MANPATH:-}\"\n"
              << "export INFOPATH=\"" << e(env_info) << ":${INFOPATH:-}\"\n";
}

// ---------------------------------------------------------------------------
// Fish env-switch output
// ---------------------------------------------------------------------------
void emit_fish_env(const Config& config, const std::string& env_slug) {
    fs::path env_dir = config.den_home / "envs" / env_slug;
    fs::path env_bin = env_dir / "bin";
    fs::path env_lib = env_dir / "lib";
    fs::path env_inc = env_dir / "include";
    fs::path env_pc = env_dir / "lib" / "pkgconfig";
    fs::path env_cmake = env_dir;
    fs::path env_man = env_dir / "share" / "man";
    fs::path env_info = env_dir / "share" / "info";
    fs::path den_bin = config.den_home / "bin";

    std::cout << "set -gx DEN_HOME " << fish_quote(config.den_home) << "\n"
              << "set -gx DEN_ENV " << fish_quote(env_slug) << "\n"
              << "set -gx PATH " << build_path_fish(env_bin, den_bin) << "\n"
              << "set -gx LIBRARY_PATH " << fish_quote(env_lib) << " $LIBRARY_PATH\n"
              << "set -gx CPATH " << fish_quote(env_inc) << " $CPATH\n"
              << "set -gx PKG_CONFIG_PATH " << fish_quote(env_pc) << " $PKG_CONFIG_PATH\n"
              << "set -gx CMAKE_PREFIX_PATH " << fish_quote(env_cmake) << " $CMAKE_PREFIX_PATH\n"
              << "set -gx MANPATH " << fish_quote(env_man) << " $MANPATH\n"
              << "set -gx INFOPATH " << fish_quote(env_info) << " $INFOPATH\n";
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void print_shell_init(const Config& config, const std::string& shell) {
    SPDLOG_DEBUG("emitting shell init for {}", shell);
    if (shell == "zsh" || shell == "bash") {
        emit_posix_init(config, shell);
    } else if (shell == "fish") {
        emit_fish_init(config);
    } else {
        throw std::invalid_argument("unsupported shell: " + shell +
                                    " (supported: zsh, bash, fish)");
    }
}

void print_env_switch(const Config& config, const std::string& env_arg) {
    // Resolve env path → filesystem slug under ~/.den/envs/.
    //   (empty)|default → active env (usually "/")
    //   /path            → env_slug(path)
    //   ROOT             → root env
    //   bare name        → env_slug("/" + name)
    std::string path;
    if (env_arg.empty() || env_arg == "default") {
        path = active_env_path(config.den_home);
    } else if (env_arg == "ROOT" || env_arg == "/") {
        path = "/";
    } else if (!env_arg.empty() && env_arg[0] == '/') {
        path = env_arg;
    } else {
        path = "/" + env_arg;
    }
    const std::string slug = env_slug(path);

    // Detect active shell from DEN_SHELL env var (set by shell-init wrapper),
    // falling back to SHELL.
    const char* shell_env = std::getenv("DEN_SHELL");
    if (!shell_env)
        shell_env = std::getenv("SHELL");
    std::string shell = shell_env ? shell_env : "";

    // Strip path prefix, e.g. "/bin/zsh" → "zsh".
    auto slash = shell.rfind('/');
    if (slash != std::string::npos)
        shell = shell.substr(slash + 1);

    SPDLOG_DEBUG("emitting env switch for path={} slug={} shell={}", path, slug, shell);

    if (shell == "fish") {
        emit_fish_env(config, slug);
    } else {
        // Default to POSIX-compatible output for zsh/bash and unknown shells.
        emit_posix_env(config, slug);
    }
}

} // namespace den

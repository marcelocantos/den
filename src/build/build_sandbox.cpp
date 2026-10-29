// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "build_sandbox.h"

#include "../core/error.h"

#include <cstdlib>
#include <pwd.h>
#include <system_error>
#include <unistd.h>

#ifdef __APPLE__
#include <dlfcn.h>
#endif

namespace den {

namespace {

constexpr const char* kSandboxExec = "/usr/bin/sandbox-exec";

std::string sh_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s)
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

// The real path of `p`, resolving symlinks in as much of it as exists.
fs::path real_path(const fs::path& p) {
    std::error_code ec;
    auto r = fs::weakly_canonical(fs::absolute(p), ec);
    return ec ? fs::absolute(p).lexically_normal() : r;
}

void add_unique(std::vector<fs::path>& v, const fs::path& p) {
    for (const auto& e : v)
        if (e == p)
            return;
    v.push_back(p);
}

std::string subpaths(const std::vector<fs::path>& paths) {
    std::string out;
    for (const auto& p : paths)
        out += " (subpath " + seatbelt_quote(p.string()) + ")";
    return out;
}

fs::path home_dir() {
    if (const char* h = std::getenv("HOME"); h && *h)
        return h;
    if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir)
        return pw->pw_dir;
    return {};
}

// A fixed system root, rebased under DEN_BUILD_SANDBOX_SYSTEM_ROOT when set.
fs::path system_root(const fs::path& p) {
    const char* base = std::getenv("DEN_BUILD_SANDBOX_SYSTEM_ROOT");
    if (!base || !*base)
        return p;
    return fs::path(base) / p.relative_path();
}

} // namespace

std::vector<fs::path> system_deny_roots(const Config& config) {
    std::vector<fs::path> roots;
    add_unique(roots, real_path(system_root("/opt/homebrew")));
    add_unique(roots, real_path(system_root("/opt/homebrew/Cellar")));
    add_unique(roots, real_path(system_root("/usr/local")));
    if (!config.homebrew_prefix.empty())
        add_unique(roots, real_path(config.homebrew_prefix));
    if (!config.homebrew_cellar.empty())
        add_unique(roots, real_path(config.homebrew_cellar));
    if (const auto home = home_dir(); !home.empty())
        add_unique(roots, real_path(system_root(home / "Library")));
    return roots;
}

BuildSandbox build_sandbox_for(const Config& config, const std::string& name,
                               const std::string& version) {
    BuildSandbox sb;
    add_unique(sb.deny_write, real_path(config.prefix));
    add_unique(sb.deny_write, real_path(config.store));
    add_unique(sb.deny_write, real_path(config.den_home));
    for (const auto& r : system_deny_roots(config))
        add_unique(sb.deny_write, r);

    const std::string id = name + "-" + version;
    const auto build = config.cache / "build";
    sb.temp_dir = real_path(config.cache / "tmp" / id);
    add_unique(sb.allow_write, real_path(build / id));
    add_unique(sb.allow_write, real_path(build / (id + ".destdir")));
    add_unique(sb.allow_write, sb.temp_dir);
    // Xcode tools keep caches under ~/Library/Developer; Homebrew's sandbox
    // allows it for every build (allow_write_xcode).
    if (const auto home = home_dir(); !home.empty())
        add_unique(sb.allow_write, real_path(system_root(home / "Library" / "Developer")));
    return sb;
}

std::string seatbelt_quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string seatbelt_profile(const BuildSandbox& sb) {
    // Later rules take precedence, so the allowed trees carve exceptions out
    // of the denied ones. file-write* covers creating, writing, renaming,
    // unlinking, chmod and xattrs; setuid/setgid bits are refused
    // everywhere, as Homebrew does.
    std::string p = "(version 1)\n"
                    "(debug deny)\n"
                    "(allow default)\n";
    if (!sb.deny_write.empty())
        p += "(deny file-write*" + subpaths(sb.deny_write) + ")\n";
    if (!sb.allow_write.empty())
        p += "(allow file-write*" + subpaths(sb.allow_write) + ")\n";
    p += "(deny file-write-setugid)\n";
    return p;
}

bool build_sandbox_supported() {
#ifdef __APPLE__
    return ::access(kSandboxExec, X_OK) == 0;
#else
    return false;
#endif
}

bool inside_macos_sandbox() {
#ifdef __APPLE__
    // libSystem's sandbox_check(pid, NULL, 0) is 1 for a sandboxed process.
    using check_fn = int (*)(pid_t, const char*, int);
    auto check = reinterpret_cast<check_fn>(::dlsym(RTLD_DEFAULT, "sandbox_check"));
    return check && check(::getpid(), nullptr, 0) == 1;
#else
    return false;
#endif
}

void require_build_sandbox() {
    if (build_sandbox_supported() && inside_macos_sandbox())
        throw UserError("den is running inside a macOS sandbox, so it cannot sandbox "
                        "source builds (sandbox-exec does not nest); run den outside the "
                        "sandbox to build from source");
}

std::string sandboxed_shell_command(const BuildSandbox& sb, const std::string& cmd) {
    if (!build_sandbox_supported())
        return cmd;
    return std::string(kSandboxExec) + " -p " + sh_quote(seatbelt_profile(sb)) + " /bin/sh -c " +
           sh_quote(cmd);
}

std::map<std::string, std::string> build_cache_env(const BuildSandbox& sb) {
    const auto& t = sb.temp_dir;
    return {
        {"GOCACHE", (t / "go-build").string()},
        {"GOMODCACHE", (t / "go" / "pkg" / "mod").string()},
        {"GOPATH", (t / "go").string()},
        {"PIP_CACHE_DIR", (t / "pip").string()},
        {"npm_config_cache", (t / "npm").string()},
        {"CARGO_HOME", (t / "cargo").string()},
    };
}

} // namespace den

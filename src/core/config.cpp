// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "config.h"

#include "../platform/platform.h"
#include "../settings/settings.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <system_error>

namespace den {

namespace {

fs::path home_dir() {
    if (const char* home = std::getenv("HOME")) {
        return fs::path(home);
    }
    return fs::path("/tmp");
}

fs::path env_or(const char* var, fs::path fallback) {
    if (const char* val = std::getenv(var)) {
        return fs::path(val);
    }
    return fallback;
}

fs::path absolute_or_self(const fs::path& p) {
    std::error_code ec;
    auto abs = fs::absolute(p, ec);
    return ec ? p : abs.lexically_normal();
}

// A den home "predates isolation" when one of its manifests lists a
// Homebrew-provider package that Homebrew's Cellar has a rack for (any
// version: brew may have upgraded the keg underneath den). Before the store
// was isolated, den poured straight into Homebrew's Cellar and its
// environments symlink there, so switching such a home to a private prefix
// would orphan every installed package. Only consulted when the private
// prefix has no Cellar yet, so a home that has ever installed in isolation
// never matches. Read-only: never writes.
bool legacy_shared_cellar(const fs::path& den_home, const fs::path& brew_cellar) {
    std::error_code ec;
    const auto manifests = den_home / "manifests";
    if (!fs::is_directory(manifests, ec) || !fs::is_directory(brew_cellar, ec)) {
        return false;
    }
    for (const auto& env : fs::directory_iterator(manifests, ec)) {
        const auto mpath = env.path() / "manifest.json";
        if (!fs::is_regular_file(mpath, ec)) {
            continue;
        }
        std::ifstream f(mpath);
        auto j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
        if (!j.is_object() || !j.contains("packages") || !j["packages"].is_object()) {
            continue;
        }
        const auto& pkgs = j["packages"];
        if (!pkgs.contains("homebrew") || !pkgs["homebrew"].is_object()) {
            continue;
        }
        for (const auto& [name, ver] : pkgs["homebrew"].items()) {
            (void)ver;
            if (fs::is_directory(brew_cellar / name, ec)) {
                return true;
            }
        }
    }
    return false;
}

// `den migrate-prefix` pours into <den_home>/prefix before it switches
// store.prefix, so that Cellar exists while den must still use the old
// prefix (and after a rollback, which keeps it). Its journal names the
// target and phase; until the switch ("switched"/"done") the private
// Cellar does not count. Read-only.
bool migrate_prefix_pending(const fs::path& den_home, const fs::path& private_prefix) {
    std::error_code ec;
    const auto path = den_home / "migrate-prefix.json";
    if (!fs::is_regular_file(path, ec)) {
        return false;
    }
    std::ifstream f(path);
    auto j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || !j.contains("to") || !j["to"].is_string()) {
        return false;
    }
    const auto phase = j.value("phase", std::string());
    if (phase == "switched" || phase == "done") {
        return false;
    }
    const fs::path to = j["to"].get<std::string>();
    if (to.lexically_normal() == private_prefix.lexically_normal()) {
        return true;
    }
    return fs::equivalent(to, private_prefix, ec);
}

} // namespace

const char* to_string(PrefixSource s) {
    switch (s) {
    case PrefixSource::EnvDenPrefix:
        return "DEN_PREFIX";
    case PrefixSource::Settings:
        return "config.json store.prefix";
    case PrefixSource::LegacyShared:
        return "legacy shared Homebrew Cellar (pre-isolation den home)";
    case PrefixSource::DenHome:
        return "DEN_HOME/prefix (default)";
    }
    return "unknown";
}

void Config::set_prefix(const fs::path& p, PrefixSource source) {
    prefix = p;
    store = p / "Cellar";
    prefix_source = source;
}

bool Config::shares_homebrew_cellar() const {
    std::error_code ec;
    if (fs::equivalent(store, homebrew_cellar, ec)) {
        return true;
    }
    return store.lexically_normal() == homebrew_cellar.lexically_normal();
}

Config Config::detect() {
    Config c;
    c.arch = detect_arch();
    c.macos_version = detect_macos_version();

    c.den_home = env_or("DEN_HOME", home_dir() / ".den");
    c.cache = c.den_home / "cache";

    // Where Homebrew lives. Read-only input for migrate / diagnostics.
#ifdef __APPLE__
    c.homebrew_prefix =
        env_or("HOMEBREW_PREFIX", c.arch == Arch::Arm64 ? "/opt/homebrew" : "/usr/local");
#else
    c.homebrew_prefix = env_or("HOMEBREW_PREFIX", "/home/linuxbrew/.linuxbrew");
#endif
    c.homebrew_cellar = env_or("HOMEBREW_CELLAR", c.homebrew_prefix / "Cellar");
    c.homebrew_caskroom = env_or("HOMEBREW_CASKROOM", c.homebrew_prefix / "Caskroom");
    c.homebrew_taps = c.homebrew_prefix / "Library" / "Taps";

    // Where den installs. See config.h for the precedence order.
    const fs::path private_prefix = c.den_home / "prefix";
    std::error_code ec;
    if (const char* dp = std::getenv("DEN_PREFIX"); dp && *dp) {
        c.set_prefix(absolute_or_self(dp), PrefixSource::EnvDenPrefix);
    } else if (auto s = read_settings(c.den_home); s.store.prefix && !s.store.prefix->empty()) {
        c.set_prefix(absolute_or_self(*s.store.prefix), PrefixSource::Settings);
    } else if (fs::exists(private_prefix / "Cellar", ec) &&
               !migrate_prefix_pending(c.den_home, private_prefix)) {
        c.set_prefix(private_prefix, PrefixSource::DenHome);
    } else if (legacy_shared_cellar(c.den_home, c.homebrew_cellar)) {
        c.set_prefix(c.homebrew_cellar.parent_path(), PrefixSource::LegacyShared);
    } else {
        c.set_prefix(private_prefix, PrefixSource::DenHome);
    }

#ifdef __APPLE__
    c.appdir = env_or("DEN_APPDIR", "/Applications");
#else
    c.appdir = env_or("DEN_APPDIR", c.den_home / "Applications");
#endif

    return c;
}

} // namespace den

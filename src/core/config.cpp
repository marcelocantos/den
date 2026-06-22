// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "config.h"

#include "../platform/platform.h"

#include <cstdlib>

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

} // namespace

Config Config::detect() {
    Config c;
    c.arch = detect_arch();
    c.macos_version = detect_macos_version();

    c.den_home = env_or("DEN_HOME", home_dir() / ".den");
    c.cache = c.den_home / "cache";

    // Homebrew paths — den shares the Cellar with Homebrew.
#ifdef __APPLE__
    c.homebrew_prefix =
        env_or("HOMEBREW_PREFIX", c.arch == Arch::Arm64 ? "/opt/homebrew" : "/usr/local");
#else
    c.homebrew_prefix = env_or("HOMEBREW_PREFIX", "/home/linuxbrew/.linuxbrew");
#endif
    c.homebrew_cellar = env_or("HOMEBREW_CELLAR", c.homebrew_prefix / "Cellar");
    c.homebrew_caskroom = env_or("HOMEBREW_CASKROOM", c.homebrew_prefix / "Caskroom");
    c.homebrew_taps = c.homebrew_prefix / "Library" / "Taps";
    c.store = c.homebrew_cellar; // Shared Cellar — bottles pour at expected prefix

    return c;
}

} // namespace den

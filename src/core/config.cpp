// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "config.h"

#include <cstdlib>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/utsname.h>
#endif

namespace den {

namespace {

Arch detect_arch() {
#if defined(__aarch64__) || defined(__arm64__)
    return Arch::Arm64;
#elif defined(__x86_64__) || defined(__amd64__)
    return Arch::X86_64;
#else
    #error "Unsupported architecture"
#endif
}

std::optional<MacOsVersion> detect_macos_version() {
#ifdef __APPLE__
    struct utsname uts;
    if (uname(&uts) == 0) {
        // Darwin kernel version maps: Darwin 24.x = macOS 15.x, etc.
        int darwin_major = 0;
        if (sscanf(uts.release, "%d", &darwin_major) == 1 && darwin_major >= 20) {
            return MacOsVersion{darwin_major - 9, 0};
        }
    }
#endif
    return std::nullopt;
}

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
    c.store = c.den_home / "store";
    c.cache = c.den_home / "cache";

    // Homebrew paths (read-only, for migration).
#ifdef __APPLE__
    c.homebrew_prefix = env_or("HOMEBREW_PREFIX",
        c.arch == Arch::Arm64 ? "/opt/homebrew" : "/usr/local");
#else
    c.homebrew_prefix = env_or("HOMEBREW_PREFIX", "/home/linuxbrew/.linuxbrew");
#endif
    c.homebrew_cellar = env_or("HOMEBREW_CELLAR", c.homebrew_prefix / "Cellar");

    return c;
}

} // namespace den

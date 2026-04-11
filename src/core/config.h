// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "types.h"

#include <filesystem>
#include <optional>

namespace den {

namespace fs = std::filesystem;

struct Config {
    // Den's own paths.
    fs::path den_home; // ~/.den/
    fs::path store;    // /opt/homebrew/Cellar (shared with Homebrew)
    fs::path cache;    // ~/.den/cache/

    // Homebrew paths.
    fs::path homebrew_prefix; // /opt/homebrew
    fs::path homebrew_cellar; // /opt/homebrew/Cellar (same as store)

    // Platform.
    Arch arch;
    std::optional<MacOsVersion> macos_version;

    // Detect configuration from environment.
    static Config detect();
};

} // namespace den

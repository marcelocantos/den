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
    fs::path store;    // ~/.den/store/  (replaces Cellar)
    fs::path cache;    // ~/.den/cache/

    // Homebrew paths (read-only, used for migration).
    fs::path homebrew_prefix; // /opt/homebrew
    fs::path homebrew_cellar; // /opt/homebrew/Cellar

    // Platform.
    Arch arch;
    std::optional<MacOsVersion> macos_version;

    // Detect configuration from environment.
    static Config detect();
};

} // namespace den

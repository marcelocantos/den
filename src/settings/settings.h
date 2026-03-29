// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace den {

namespace fs = std::filesystem;

struct DaemonSettings {
    bool auto_download = true;
    bool auto_upgrade  = false;
    std::optional<std::string> upgrade_window;   // e.g. "02:00-04:00"
    std::optional<uint64_t>    interval_secs;    // override polling interval
};

struct SearchSettings {
    std::optional<std::string> provider; // e.g. "formulae.brew.sh"
};

struct Settings {
    DaemonSettings daemon;
    SearchSettings search;
};

// Read settings from <den_home>/config.json.  Returns defaults if the
// file is absent or a field is missing.
Settings read_settings(const fs::path& den_home);

// Atomically write settings to <den_home>/config.json.
void write_settings(const fs::path& den_home, const Settings& s);

// Read a single dotted key, e.g. "daemon.auto_upgrade".
// Returns the JSON representation of the value as a string.
// Throws std::invalid_argument if the key does not exist.
std::string get_setting(const fs::path& den_home, const std::string& key);

// Write a single dotted key.  The type is inferred from the existing
// value (bool, number, string).  New keys are always stored as strings.
// Throws std::invalid_argument if a parent path is not an object.
void set_setting(const fs::path& den_home, const std::string& key,
                 const std::string& value);

// Return a pretty-printed JSON representation of all settings.
std::string display_all_settings(const fs::path& den_home);

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace den {

namespace fs = std::filesystem;

struct DaemonSettings {
    bool auto_download = true;
    bool auto_upgrade = false;
    std::optional<std::string> upgrade_window; // e.g. "02:00-04:00"
    std::optional<uint64_t> interval_secs;     // override polling interval
};

struct SearchSettings {
    std::optional<std::string> provider; // e.g. "formulae.brew.sh"
};

// Registry of third-party Homebrew taps (🎯T67). Keys are tap names in
// "user/repo" form; values are the clone source (a git URL or a local path).
// Persisted under the "taps" object in config.json.
struct TapSettings {
    std::map<std::string, std::string> taps;
};

// Where den installs (🎯 store isolation). Unset means the default,
// <den_home>/prefix. `den migrate` pins this to Homebrew's prefix because
// migrated kegs stay in Homebrew's Cellar. Overridden by DEN_PREFIX.
struct StoreSettings {
    std::optional<std::string> prefix;
};

struct Settings {
    DaemonSettings daemon;
    StoreSettings store;
    SearchSettings search;
    TapSettings taps;
    /// Casks recorded by `den migrate` (token -> version). These stay in the
    /// Homebrew Caskroom; they are not Cellar packages and are not linked
    /// into environments.
    std::map<std::string, std::string> casks;
};

/// Mode config.json is written with (and the mode `den doctor` expects).
inline constexpr unsigned kConfigFileMode = 0600;

// Read settings from <den_home>/config.json.  Returns defaults if the
// file is absent or a field is missing.
Settings read_settings(const fs::path& den_home);

// Atomically write settings to <den_home>/config.json.
void write_settings(const fs::path& den_home, const Settings& s);

/// Thrown by get_setting when the key is not present in config.json.
class SettingNotFound : public std::invalid_argument {
  public:
    using std::invalid_argument::invalid_argument;
};

/// Thrown by get_setting / set_setting when config.json exists but cannot be
/// read or is not a JSON object. Unlike read_settings (which falls back to
/// defaults), single-key access must not silently treat a damaged file as
/// empty: set_setting would otherwise overwrite every other key.
class SettingsFileError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// Read a single dotted key, e.g. "daemon.auto_upgrade".
// Returns the JSON representation of the value as a string.
// Throws SettingNotFound if the key does not exist, SettingsFileError if
// config.json is unreadable or invalid.
std::string get_setting(const fs::path& den_home, const std::string& key);

// Write a single dotted key.  The type is inferred from the existing
// value (bool, number, string).  New keys are always stored as strings.
// Every other key in config.json is preserved.
// Throws std::invalid_argument if a parent path is not an object, and
// SettingsFileError (writing nothing) if config.json is unreadable or invalid.
void set_setting(const fs::path& den_home, const std::string& key, const std::string& value);

// Return a pretty-printed JSON representation of all settings.
std::string display_all_settings(const fs::path& den_home);

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "settings.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace den {

// ---------------------------------------------------------------------------
// JSON serde helpers using NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT.
// The macro generates from_json / to_json overloads that fill in defaults for
// absent fields, so a partial config.json is valid.
// ---------------------------------------------------------------------------

// DaemonSettings — optional fields use a two-step approach because
// NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT does not directly handle
// std::optional.  We therefore write the overloads manually.

inline void to_json(nlohmann::json& j, const DaemonSettings& d) {
    j = nlohmann::json{
        {"auto_download", d.auto_download},
        {"auto_upgrade", d.auto_upgrade},
    };
    if (d.upgrade_window)
        j["upgrade_window"] = *d.upgrade_window;
    if (d.interval_secs)
        j["interval_secs"] = *d.interval_secs;
}

// Accept real JSON booleans and the string forms written by older set_setting.
bool json_as_bool(const nlohmann::json& v, bool* out) {
    if (v.is_boolean()) {
        *out = v.get<bool>();
        return true;
    }
    if (v.is_string()) {
        const auto& s = v.get_ref<const std::string&>();
        if (s == "true") {
            *out = true;
            return true;
        }
        if (s == "false") {
            *out = false;
            return true;
        }
    }
    return false;
}

inline void from_json(const nlohmann::json& j, DaemonSettings& d) {
    d = DaemonSettings{};
    if (j.contains("auto_download"))
        json_as_bool(j["auto_download"], &d.auto_download);
    if (j.contains("auto_upgrade"))
        json_as_bool(j["auto_upgrade"], &d.auto_upgrade);
    if (j.contains("upgrade_window") && j["upgrade_window"].is_string())
        d.upgrade_window = j["upgrade_window"].get<std::string>();
    if (j.contains("interval_secs") && j["interval_secs"].is_number_unsigned())
        d.interval_secs = j["interval_secs"].get<uint64_t>();
}

inline void to_json(nlohmann::json& j, const SearchSettings& s) {
    j = nlohmann::json::object();
    if (s.provider)
        j["provider"] = *s.provider;
}

inline void from_json(const nlohmann::json& j, SearchSettings& s) {
    s = SearchSettings{};
    if (j.contains("provider") && j["provider"].is_string())
        s.provider = j["provider"].get<std::string>();
}

inline void to_json(nlohmann::json& j, const TapSettings& t) {
    j = nlohmann::json::object();
    for (const auto& [name, url] : t.taps)
        j[name] = url;
}

inline void from_json(const nlohmann::json& j, TapSettings& t) {
    t = TapSettings{};
    if (!j.is_object())
        return;
    for (const auto& [name, url] : j.items()) {
        if (url.is_string())
            t.taps[name] = url.get<std::string>();
    }
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Settings, daemon, search, taps)

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------

namespace {

fs::path config_path(const fs::path& den_home) {
    return den_home / "config.json";
}

nlohmann::json load_json(const fs::path& den_home) {
    fs::path p = config_path(den_home);
    if (!fs::exists(p)) {
        SPDLOG_DEBUG("config.json not found at {}, using defaults", p.string());
        return nlohmann::json::object();
    }
    std::ifstream f(p);
    if (!f) {
        SPDLOG_WARN("cannot open config.json at {}", p.string());
        return nlohmann::json::object();
    }
    try {
        return nlohmann::json::parse(f);
    } catch (const nlohmann::json::parse_error& e) {
        SPDLOG_WARN("failed to parse config.json: {}", e.what());
        return nlohmann::json::object();
    }
}

void save_json_atomic(const fs::path& den_home, const nlohmann::json& j) {
    fs::path p = config_path(den_home);
    fs::path tmp = p;
    tmp += ".tmp";

    // Ensure the parent directory exists.
    fs::create_directories(den_home);

    {
        std::ofstream f(tmp);
        if (!f) {
            throw std::runtime_error("cannot write " + tmp.string());
        }
        f << j.dump(4) << '\n';
        if (!f) {
            throw std::runtime_error("write failed for " + tmp.string());
        }
    }
    // Atomic rename.
    fs::rename(tmp, p);
    SPDLOG_DEBUG("wrote config.json to {}", p.string());
}

// Split "daemon.auto_upgrade" → ["daemon", "auto_upgrade"].
std::vector<std::string> split_key(const std::string& key) {
    std::vector<std::string> parts;
    std::istringstream ss(key);
    std::string part;
    while (std::getline(ss, part, '.')) {
        if (part.empty())
            throw std::invalid_argument("invalid key (empty component): " + key);
        parts.push_back(std::move(part));
    }
    if (parts.empty())
        throw std::invalid_argument("empty key");
    return parts;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Settings read_settings(const fs::path& den_home) {
    nlohmann::json j = load_json(den_home);
    Settings s;
    try {
        s = j.get<Settings>();
    } catch (const nlohmann::json::exception& e) {
        SPDLOG_WARN("error deserialising settings, using defaults: {}", e.what());
    }
    return s;
}

void write_settings(const fs::path& den_home, const Settings& s) {
    nlohmann::json j = s;
    save_json_atomic(den_home, j);
}

std::string get_setting(const fs::path& den_home, const std::string& key) {
    auto parts = split_key(key);
    nlohmann::json j = load_json(den_home);

    const nlohmann::json* cur = &j;
    for (const auto& part : parts) {
        if (!cur->is_object() || !cur->contains(part)) {
            throw std::invalid_argument("key not found: " + key);
        }
        cur = &(*cur)[part];
    }

    // Return a compact JSON representation of the value.
    if (cur->is_string()) {
        return cur->get<std::string>();
    }
    return cur->dump();
}

void set_setting(const fs::path& den_home, const std::string& key, const std::string& value) {
    auto parts = split_key(key);
    nlohmann::json j = load_json(den_home);

    // Navigate to the parent object, creating intermediate objects as needed.
    nlohmann::json* cur = &j;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::string& part = parts[i];
        if (!cur->contains(part)) {
            (*cur)[part] = nlohmann::json::object();
        } else if (!(*cur)[part].is_object()) {
            throw std::invalid_argument("path component '" + part +
                                        "' is not an object in key: " + key);
        }
        cur = &(*cur)[part];
    }

    const std::string& leaf = parts.back();

    // Known boolean keys always store JSON booleans (not "true"/"false" strings).
    // Without this, first-time `den set daemon.auto_upgrade true` wrote a string
    // that from_json ignored, so the daemon kept the default false.
    const bool known_bool =
        (key == "daemon.auto_download" || key == "daemon.auto_upgrade");
    if (known_bool || (cur->contains(leaf) && (*cur)[leaf].is_boolean())) {
        if (value == "true")
            (*cur)[leaf] = true;
        else if (value == "false")
            (*cur)[leaf] = false;
        else
            throw std::invalid_argument("expected 'true' or 'false' for boolean key: " + key);
        save_json_atomic(den_home, j);
        return;
    }

    // Infer the stored type from the existing value, if present.
    if (cur->contains(leaf)) {
        const nlohmann::json& existing = (*cur)[leaf];
        if (existing.is_number_unsigned()) {
            try {
                (*cur)[leaf] = static_cast<uint64_t>(std::stoull(value));
            } catch (...) {
                throw std::invalid_argument("expected unsigned integer for key: " + key);
            }
            save_json_atomic(den_home, j);
            return;
        }
        if (existing.is_number_integer()) {
            try {
                (*cur)[leaf] = std::stoll(value);
            } catch (...) {
                throw std::invalid_argument("expected integer for key: " + key);
            }
            save_json_atomic(den_home, j);
            return;
        }
        if (existing.is_number_float()) {
            try {
                (*cur)[leaf] = std::stod(value);
            } catch (...) {
                throw std::invalid_argument("expected number for key: " + key);
            }
            save_json_atomic(den_home, j);
            return;
        }
        // String or null — fall through to store as string.
    }

    // New key or existing string: store as string.
    (*cur)[leaf] = value;
    save_json_atomic(den_home, j);
}

std::string display_all_settings(const fs::path& den_home) {
    Settings s = read_settings(den_home);
    nlohmann::json j = s;
    return j.dump(4) + '\n';
}

} // namespace den

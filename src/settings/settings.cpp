// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "settings.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
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

inline void to_json(nlohmann::json& j, const StoreSettings& s) {
    j = nlohmann::json::object();
    if (s.prefix)
        j["prefix"] = *s.prefix;
}

inline void from_json(const nlohmann::json& j, StoreSettings& s) {
    s = StoreSettings{};
    if (j.is_object() && j.contains("prefix") && j["prefix"].is_string())
        s.prefix = j["prefix"].get<std::string>();
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

inline void to_json(nlohmann::json& j, const TrustSettings& t) {
    j = nlohmann::json{
        {"unattended", t.unattended},
        {"notify", t.notify},
        {"unattended_source_builds", t.unattended_source_builds},
        {"source_build_allowlist", t.source_build_allowlist},
    };
}

inline void from_json(const nlohmann::json& j, TrustSettings& t) {
    t = TrustSettings{};
    if (!j.is_object())
        return;
    // A value of the wrong type is kept as its JSON text rather than
    // dropped back to the default, so it fails closed (trust.unattended →
    // refuse, unattended_source_builds → off) and `den doctor` names it.
    auto enum_value = [](const nlohmann::json& v) {
        return v.is_string() ? v.get<std::string>() : v.dump();
    };
    if (j.contains("unattended"))
        t.unattended = enum_value(j["unattended"]);
    if (j.contains("notify"))
        json_as_bool(j["notify"], &t.notify);
    if (j.contains("unattended_source_builds"))
        t.unattended_source_builds = enum_value(j["unattended_source_builds"]);
    if (j.contains("source_build_allowlist")) {
        const auto& v = j["source_build_allowlist"];
        std::vector<std::string> names;
        if (v.is_array()) {
            for (const auto& n : v)
                if (n.is_string() && !n.get_ref<const std::string&>().empty())
                    names.push_back(n.get<std::string>());
        } else if (v.is_string()) {
            std::stringstream ss(v.get<std::string>());
            std::string n;
            while (std::getline(ss, n, ',')) {
                auto b = n.find_first_not_of(" \t");
                auto e = n.find_last_not_of(" \t");
                if (b != std::string::npos)
                    names.push_back(n.substr(b, e - b + 1));
            }
        }
        t.source_build_allowlist = std::move(names);
    }
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Settings, daemon, store, search, taps, trust, casks)

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

// Strict variant for single-key access: an absent file is an empty object,
// but an unreadable or invalid one is an error rather than "no settings".
nlohmann::json load_json_strict(const fs::path& den_home) {
    fs::path p = config_path(den_home);
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        if (ec)
            throw SettingsFileError("cannot access " + p.string() + ": " + ec.message());
        return nlohmann::json::object();
    }
    std::ifstream f(p);
    if (!f)
        throw SettingsFileError("cannot open " + p.string());
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(f);
    } catch (const nlohmann::json::parse_error& e) {
        throw SettingsFileError(p.string() + " is not valid JSON (" + e.what() +
                                "); fix or remove it");
    }
    if (!j.is_object())
        throw SettingsFileError(p.string() + " is not a JSON object; fix or remove it");
    return j;
}

void save_json_atomic(const fs::path& den_home, const nlohmann::json& j) {
    fs::path p = config_path(den_home);
    fs::path tmp = p;
    tmp += ".tmp";

    // Ensure the parent directory exists.
    fs::create_directories(den_home);

    // config.json is a sensitive file (`den doctor` expects kConfigFileMode).
    // Create the temp file with that mode and fchmod it before writing, so the
    // mode does not depend on the umask or on a stale temp file, and the file
    // is never visible at its final path with a wider mode.
    const std::string body = j.dump(4) + '\n';
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kConfigFileMode);
    if (fd < 0) {
        throw std::runtime_error("cannot write " + tmp.string() + ": " + std::strerror(errno));
    }
    auto fail = [&](const std::string& what) {
        int saved = errno;
        ::close(fd);
        std::error_code ec;
        fs::remove(tmp, ec);
        throw std::runtime_error(what + " " + tmp.string() + ": " + std::strerror(saved));
    };
    if (::fchmod(fd, kConfigFileMode) != 0) {
        fail("cannot set mode on");
    }
    const char* data = body.data();
    size_t left = body.size();
    while (left > 0) {
        ssize_t n = ::write(fd, data, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fail("write failed for");
        }
        data += n;
        left -= static_cast<size_t>(n);
    }
    if (::close(fd) != 0) {
        std::error_code ec;
        fs::remove(tmp, ec);
        throw std::runtime_error("write failed for " + tmp.string());
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
    nlohmann::json j = load_json_strict(den_home);

    const nlohmann::json* cur = &j;
    for (const auto& part : parts) {
        if (!cur->is_object() || !cur->contains(part)) {
            throw SettingNotFound("key not found: " + key);
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
    nlohmann::json j = load_json_strict(den_home);

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
        (key == "daemon.auto_download" || key == "daemon.auto_upgrade" || key == "trust.notify");
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

    // Enumerated trust keys: reject typos here rather than letting a bad value
    // fail closed later without explanation.
    if (key == "trust.unattended" && value != "warn" && value != "refuse")
        throw std::invalid_argument("expected 'warn' or 'refuse' for key: " + key);
    if (key == "trust.unattended_source_builds" && value != "off" && value != "allowlist")
        throw std::invalid_argument("expected 'off' or 'allowlist' for key: " + key);

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

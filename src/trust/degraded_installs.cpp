// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "degraded_installs.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace den {

namespace fs = std::filesystem;

namespace {

constexpr int kSchemaVersion = 1;

std::string key_of(const std::string& name, const std::string& version) {
    return name + "@" + version;
}

// Read the ledger. Missing → empty document. Unreadable → nullopt + error.
std::optional<nlohmann::json> read_ledger(const fs::path& path, std::string& error) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return nlohmann::json{{"schema_version", kSchemaVersion},
                              {"installs", nlohmann::json::object()}};
    }
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path.string();
        return std::nullopt;
    }
    try {
        auto j = nlohmann::json::parse(in);
        if (!j.is_object() || !j.contains("installs") || !j["installs"].is_object()) {
            error = path.string() + ": missing 'installs' object";
            return std::nullopt;
        }
        return j;
    } catch (const std::exception& e) {
        error = path.string() + ": " + e.what();
        return std::nullopt;
    }
}

void write_ledger(const fs::path& path, const nlohmann::json& j) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp." + std::to_string(::getpid());
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            SPDLOG_WARN("cannot write degraded-install ledger {}", tmp.string());
            return;
        }
        out << j.dump(2) << "\n";
        if (!out) {
            SPDLOG_WARN("cannot write degraded-install ledger {}", tmp.string());
            fs::remove(tmp, ec);
            return;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        SPDLOG_WARN("cannot update degraded-install ledger {}: {}", path.string(), ec.message());
        fs::remove(tmp, ec);
    }
}

// Load for modification. A corrupt ledger is moved aside (kept for
// inspection) rather than silently overwritten.
nlohmann::json load_for_update(const fs::path& path) {
    std::string error;
    if (auto j = read_ledger(path, error)) {
        return *j;
    }
    auto aside = path;
    aside += ".corrupt";
    std::error_code ec;
    fs::rename(path, aside, ec);
    SPDLOG_WARN("degraded-install ledger unreadable ({}); moved to {} and starting afresh", error,
                aside.string());
    return nlohmann::json{{"schema_version", kSchemaVersion},
                          {"installs", nlohmann::json::object()}};
}

} // namespace

fs::path degraded_installs_path(const Config& config) {
    return config.den_home / "trust" / "degraded_installs.json";
}

void record_degraded_install(const Config& config, const DegradedInstall& install) {
    try {
        auto path = degraded_installs_path(config);
        auto j = load_for_update(path);
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        j["installs"][key_of(install.name, install.version)] = {
            {"name", install.name},        {"version", install.version},
            {"reason", install.reason},    {"bottle_tag", install.bottle_tag},
            {"keg", install.keg.string()}, {"recorded_at", now},
        };
        write_ledger(path, j);
    } catch (const std::exception& e) {
        SPDLOG_WARN("cannot record degraded install of {} {}: {}", install.name, install.version,
                    e.what());
    }
}

void clear_degraded_install(const Config& config, const std::string& name,
                            const std::string& version) {
    try {
        auto path = degraded_installs_path(config);
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            return;
        }
        auto j = load_for_update(path);
        if (j["installs"].erase(key_of(name, version)) > 0) {
            write_ledger(path, j);
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("cannot update degraded-install ledger for {} {}: {}", name, version, e.what());
    }
}

DegradedInstallSummary summarize_degraded_installs(const Config& config) {
    DegradedInstallSummary summary;
    std::string error;
    auto j = read_ledger(degraded_installs_path(config), error);
    if (!j) {
        summary.ledger_error = error;
        return summary;
    }
    for (const auto& [key, v] : (*j)["installs"].items()) {
        if (!v.is_object()) {
            continue;
        }
        DegradedInstall d;
        d.name = v.value("name", key);
        d.version = v.value("version", std::string());
        d.reason = v.value("reason", std::string("unknown"));
        d.bottle_tag = v.value("bottle_tag", std::string());
        d.keg = v.value("keg", std::string());
        std::error_code ec;
        if (d.keg.empty() || !fs::is_directory(d.keg, ec)) {
            continue; // keg gone (cleaned up or replaced): no longer installed
        }
        ++summary.total;
        ++summary.by_reason[d.reason];
        summary.installs.push_back(std::move(d));
    }
    std::sort(summary.installs.begin(), summary.installs.end(),
              [](const DegradedInstall& a, const DegradedInstall& b) {
                  return a.name != b.name ? a.name < b.name : a.version < b.version;
              });
    return summary;
}

std::string format_degraded_reasons(const DegradedInstallSummary& summary) {
    if (summary.by_reason.empty()) {
        return {};
    }
    std::ostringstream out;
    out << "(";
    bool first = true;
    for (const auto& [reason, n] : summary.by_reason) {
        out << (first ? "" : ", ") << reason << ": " << n;
        first = false;
    }
    out << ")";
    return out.str();
}

std::string format_degraded_counts(const DegradedInstallSummary& summary) {
    if (summary.ledger_error) {
        return "unknown (ledger unreadable: " + *summary.ledger_error + ")";
    }
    auto reasons = format_degraded_reasons(summary);
    return std::to_string(summary.total) + (reasons.empty() ? "" : " " + reasons);
}

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "activity.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>

namespace den {

namespace {

fs::path activity_path(const fs::path& den_home) {
    return den_home / "activity.json";
}

nlohmann::json entry_to_json(const ActivityEntry& e) {
    return {
        {"timestamp", e.timestamp}, {"package", e.package}, {"from", e.from_version},
        {"to", e.to_version},       {"trigger", e.trigger},
    };
}

ActivityEntry json_to_entry(const nlohmann::json& j) {
    ActivityEntry e;
    if (j.contains("timestamp") && j["timestamp"].is_number_unsigned())
        e.timestamp = j["timestamp"].get<uint64_t>();
    if (j.contains("package") && j["package"].is_string())
        e.package = j["package"].get<std::string>();
    if (j.contains("from") && j["from"].is_string())
        e.from_version = j["from"].get<std::string>();
    if (j.contains("to") && j["to"].is_string())
        e.to_version = j["to"].get<std::string>();
    if (j.contains("trigger") && j["trigger"].is_string())
        e.trigger = j["trigger"].get<std::string>();
    return e;
}

} // namespace

void record_activity(const fs::path& den_home, const std::vector<ActivityEntry>& entries) {
    if (entries.empty())
        return;

    fs::path path = activity_path(den_home);

    // Read existing entries.
    nlohmann::json arr = nlohmann::json::array();
    try {
        std::ifstream f(path);
        if (f.is_open()) {
            nlohmann::json j;
            f >> j;
            if (j.is_array())
                arr = std::move(j);
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("activity: failed to read {}: {}", path.string(), e.what());
    }

    // Append new entries.
    for (const auto& entry : entries)
        arr.push_back(entry_to_json(entry));

    // Atomic write.
    fs::path tmp = path.string() + ".tmp";
    try {
        fs::create_directories(den_home);
        {
            std::ofstream f(tmp);
            if (!f) {
                SPDLOG_ERROR("activity: cannot open {} for writing", tmp.string());
                return;
            }
            f << arr.dump(2) << '\n';
        }
        fs::rename(tmp, path);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("activity: failed to write: {}", e.what());
        std::error_code ec;
        fs::remove(tmp, ec);
    }
}

std::vector<ActivityEntry> read_activity(const fs::path& den_home, size_t max) {
    fs::path path = activity_path(den_home);
    std::vector<ActivityEntry> result;

    try {
        std::ifstream f(path);
        if (!f.is_open())
            return result;
        nlohmann::json j;
        f >> j;
        if (!j.is_array())
            return result;

        // Parse all entries.
        result.reserve(j.size());
        for (const auto& item : j) {
            if (item.is_object())
                result.push_back(json_to_entry(item));
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("activity: failed to read {}: {}", path.string(), e.what());
        return result;
    }

    // Sort most recent first.
    std::sort(result.begin(), result.end(), [](const ActivityEntry& a, const ActivityEntry& b) {
        return a.timestamp > b.timestamp;
    });

    if (max > 0 && result.size() > max)
        result.resize(max);

    return result;
}

} // namespace den

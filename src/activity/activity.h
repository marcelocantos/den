// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

/// A single recorded upgrade event.
struct ActivityEntry {
    uint64_t timestamp; // Unix seconds
    std::string package;
    std::string from_version;
    std::string to_version;
    std::string trigger; // "manual", "daemon", "auto"
};

/// Append one or more upgrade events to the activity log.
void record_activity(const fs::path& den_home, const std::vector<ActivityEntry>& entries);

/// Read all activity entries, most recent first.
/// If max > 0, returns at most that many entries.
std::vector<ActivityEntry> read_activity(const fs::path& den_home, size_t max = 0);

} // namespace den

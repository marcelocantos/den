// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"

#include <optional>
#include <string>

namespace den {

struct UpdateInfo {
    std::string latest_version; // e.g. "0.5.0" (no v prefix)
    std::string download_url;   // tarball URL for this platform
    std::string checksum_url;   // .sha256 URL
};

/// Check GitHub Releases for a newer version of den.
/// Returns nullopt if already up to date or unable to determine.
std::optional<UpdateInfo> check_for_update(const Config& cfg);

/// Download and install a new den binary, replacing the current one.
/// The running binary is atomically replaced via rename.
void apply_update(const UpdateInfo& info, const Config& cfg);

} // namespace den

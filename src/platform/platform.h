// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/types.h"

#include <optional>
#include <string>
#include <vector>

namespace den {

/// Detect the current CPU architecture at compile time.
Arch detect_arch();

/// Detect the macOS version from the kernel utsname, or nullopt on Linux.
std::optional<MacOsVersion> detect_macos_version();

/// Map a macOS major version number to its lowercase codename.
/// Returns nullopt for unknown versions.
/// Supported: 26=tahoe, 15=sequoia, 14=sonoma, 13=ventura, 12=monterey.
std::optional<std::string> macos_codename(int major);

/// Return the bottle tag for this platform (e.g. "arm64_sequoia").
/// Returns nullopt when the macOS version has no known codename.
std::optional<std::string> bottle_tag(Arch arch, const MacOsVersion& macos);

/// Return all bottle tag candidates to try, from the current macOS version
/// down to older ones. The first entry is the most preferred tag.
/// Returns {"x86_64_linux"} on Linux.
std::vector<std::string> bottle_tag_candidates(
    Arch arch,
    const std::optional<MacOsVersion>& macos);

/// Pick the best available bottle tag from a list of available tags.
/// Returns nullopt if none of the candidates appear in available_tags.
std::optional<std::string> best_archive_tag(
    Arch arch,
    const std::optional<MacOsVersion>& macos,
    const std::vector<std::string>& available_tags);

} // namespace den

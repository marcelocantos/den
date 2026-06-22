// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/types.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace den {

/// Host toolchain / OS facts surfaced by `den config` and `den doctor`.
///
/// Every field is optional: detection shells out to host tools that may be
/// absent (no Xcode, no Command Line Tools, no Homebrew). A missing field is
/// reported as "n/a" rather than failing.
///
/// `homebrew_config` mirrors the stable `KEY: value` lines from `brew config`
/// (relative-time lines such as "Last commit" are filtered out so the output
/// stays byte-stable between runs). The pairs preserve `brew config`'s order.
struct HostFacts {
#ifdef __APPLE__
    std::optional<std::string> xcode_version; // e.g. "26.5"
    std::optional<std::string> clt_version;   // e.g. "26.5.0.0.1777544298"
    std::optional<std::string> sdk_path;      // e.g. ".../MacOSX.sdk"
#else
    std::optional<std::string> os_name;    // e.g. "Ubuntu"
    std::optional<std::string> os_version; // e.g. "22.04"
    std::optional<std::string> kernel;     // e.g. "6.5.0-21-generic"
    std::optional<std::string> glibc;      // e.g. "2.35"
#endif
    // Stable subset of `brew config` (empty when brew is absent).
    std::vector<std::pair<std::string, std::string>> homebrew_config;
};

/// Detect host toolchain / OS facts by shelling out to platform tools.
/// Runs each command defensively: a non-zero exit or missing tool yields a
/// nullopt field rather than an error.
HostFacts detect_host_facts();

/// Run a command, capturing trimmed stdout. Returns nullopt when the command
/// fails to launch, exits non-zero, or produces no output. Stderr is
/// discarded so probe failures stay quiet.
std::optional<std::string> capture_command(const std::string& cmd);

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
std::vector<std::string> bottle_tag_candidates(Arch arch, const std::optional<MacOsVersion>& macos);

/// Pick the best available bottle tag from a list of available tags.
/// Returns nullopt if none of the candidates appear in available_tags.
std::optional<std::string> best_archive_tag(Arch arch, const std::optional<MacOsVersion>& macos,
                                            const std::vector<std::string>& available_tags);

} // namespace den

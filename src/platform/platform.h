// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/types.h"

#include <optional>
#include <string>
#include <string_view>
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

/// The macOS SDK den steers source builds to, resolved without xcrun (🎯T45):
/// an absolute `SDKROOT` that names a directory, else the developer
/// directory from `xcode-select -p` (Xcode's
/// Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk or the Command Line
/// Tools' SDKs/MacOSX.sdk), else /Library/Developer/CommandLineTools/SDKs/
/// MacOSX.sdk. Returns nullopt on Linux or when none exists.
std::optional<std::string> macos_sdk_path();

/// Detect the current CPU architecture at compile time.
Arch detect_arch();

/// Detect the macOS version, or nullopt on Linux. Prefers
/// sysctl kern.osproductversion and cross-checks it against the Darwin
/// kernel release (see resolve_macos_version).
std::optional<MacOsVersion> detect_macos_version();

/// Parse a product version string such as "26.6.2" or "15.1" into
/// major.minor. Returns nullopt for unparseable input.
std::optional<MacOsVersion> parse_macos_product_version(std::string_view s);

/// Map a Darwin kernel release ("25.6.0") to the macOS version it ships
/// with: Darwin 20–24 → macOS 11–15, Darwin 25 → macOS 26, Darwin 27+ →
/// macOS 27+ (same number). Returns nullopt for pre-Big Sur, the unshipped
/// Darwin 26, or unparseable input.
std::optional<MacOsVersion> macos_version_from_darwin(std::string_view release);

/// Combine the reported product version with the Darwin release. The
/// product version wins unless it is a SYSTEM_VERSION_COMPAT shim (10.16,
/// 16.x–25.x) or disagrees with the kernel, in which case the Darwin
/// mapping is used.
std::optional<MacOsVersion> resolve_macos_version(const std::optional<std::string>& product_version,
                                                  const std::optional<std::string>& darwin_release);

/// Map a macOS major version number to its lowercase codename.
/// Returns nullopt for unknown versions.
/// Supported: 27=golden_gate, 26=tahoe, 15=sequoia, 14=sonoma, 13=ventura, 12=monterey.
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

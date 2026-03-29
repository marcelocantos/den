// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "platform.h"

#include <spdlog/spdlog.h>

#include <algorithm>

#ifdef __APPLE__
#include <sys/utsname.h>
#endif

namespace den {

Arch detect_arch() {
#if defined(__aarch64__) || defined(__arm64__)
    return Arch::Arm64;
#elif defined(__x86_64__) || defined(__amd64__)
    return Arch::X86_64;
#else
#error "Unsupported architecture"
#endif
}

std::optional<MacOsVersion> detect_macos_version() {
#ifdef __APPLE__
    struct utsname uts{};
    if (uname(&uts) == 0) {
        // Darwin kernel version is offset by 9 relative to macOS version.
        // Darwin 24.x = macOS 15.x (Sequoia), Darwin 25.x = macOS 16.x, etc.
        int darwin_major = 0;
        if (sscanf(uts.release, "%d", &darwin_major) == 1 && darwin_major >= 20) {
            SPDLOG_DEBUG("Detected Darwin kernel version {}", darwin_major);
            return MacOsVersion{darwin_major - 9, 0};
        }
    }
#endif
    return std::nullopt;
}

std::optional<std::string> macos_codename(int major) {
    switch (major) {
    case 26:
        return "tahoe";
    case 15:
        return "sequoia";
    case 14:
        return "sonoma";
    case 13:
        return "ventura";
    case 12:
        return "monterey";
    default:
        return std::nullopt;
    }
}

std::optional<std::string> bottle_tag(Arch arch, const MacOsVersion& macos) {
    auto codename = macos_codename(macos.major);
    if (!codename) {
        return std::nullopt;
    }
    switch (arch) {
    case Arch::Arm64:
        return "arm64_" + *codename;
    case Arch::X86_64:
        return *codename;
    }
    return std::nullopt;
}

std::vector<std::string> bottle_tag_candidates(Arch arch,
                                               const std::optional<MacOsVersion>& macos) {
    // Ordered from newest to oldest — first match wins.
    static const std::pair<int, const char*> kCodenames[] = {
        {26, "tahoe"}, {15, "sequoia"}, {14, "sonoma"}, {13, "ventura"}, {12, "monterey"},
    };

    if (!macos) {
        // Linux: only one tag.
        if (arch == Arch::X86_64) {
            return {"x86_64_linux"};
        }
        return {"arm64_linux"};
    }

    std::vector<std::string> candidates;
    for (const auto& [major, codename] : kCodenames) {
        if (major <= macos->major) {
            switch (arch) {
            case Arch::Arm64:
                candidates.push_back(std::string("arm64_") + codename);
                break;
            case Arch::X86_64:
                candidates.push_back(codename);
                break;
            }
        }
    }
    return candidates;
}

std::optional<std::string> best_archive_tag(Arch arch, const std::optional<MacOsVersion>& macos,
                                            const std::vector<std::string>& available_tags) {
    const auto candidates = bottle_tag_candidates(arch, macos);
    for (const auto& candidate : candidates) {
        if (std::find(available_tags.begin(), available_tags.end(), candidate) !=
            available_tags.end()) {
            return candidate;
        }
    }
    // Fallback: "all" tag (platform-independent packages like ca-certificates).
    if (std::find(available_tags.begin(), available_tags.end(), "all") != available_tags.end()) {
        return "all";
    }
    return std::nullopt;
}

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "platform.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string_view>

#include <sys/utsname.h>
#include <sys/wait.h>

namespace den {

namespace {

/// Trim ASCII whitespace from both ends.
std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

} // namespace

std::optional<std::string> capture_command(const std::string& cmd) {
    // Discard stderr so a tool that prints a diagnostic on failure stays quiet.
    std::string full = cmd + " 2>/dev/null";
    FILE* pipe = ::popen(full.c_str(), "r");
    if (!pipe) {
        SPDLOG_DEBUG("capture_command: popen failed for '{}'", cmd);
        return std::nullopt;
    }
    std::string out;
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        out += buf.data();
    }
    int status = ::pclose(pipe);
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (rc != 0) {
        SPDLOG_DEBUG("capture_command: '{}' exited {}", cmd, rc);
        return std::nullopt;
    }
    auto trimmed = trim(out);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    return trimmed;
}

namespace {

#ifdef __APPLE__

/// Parse the `version:` line out of `pkgutil --pkg-info` output.
std::optional<std::string> parse_pkgutil_version(const std::string& info) {
    std::istringstream ss(info);
    std::string line;
    while (std::getline(ss, line)) {
        constexpr std::string_view kPrefix = "version: ";
        if (line.rfind(kPrefix, 0) == 0) {
            return trim(line.substr(kPrefix.size()));
        }
    }
    return std::nullopt;
}

/// Extract the version token from `xcodebuild -version` ("Xcode 26.5\n...").
std::optional<std::string> parse_xcode_version(const std::string& out) {
    std::istringstream ss(out);
    std::string word;
    if (ss >> word && word == "Xcode" && (ss >> word)) {
        return word;
    }
    return std::nullopt;
}

#else // Linux

/// Read NAME / VERSION_ID from /etc/os-release.
void read_os_release(std::optional<std::string>& name, std::optional<std::string>& version) {
    std::ifstream f("/etc/os-release");
    if (!f)
        return;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // Strip surrounding double quotes.
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }
        if (key == "NAME")
            name = trim(val);
        else if (key == "VERSION_ID")
            version = trim(val);
    }
}

#endif

/// Lines in `brew config` whose values are timestamps (relative ages or tap
/// JSON freshness dates); these change over time and would break output
/// stability, so they are dropped.
bool is_volatile_brew_key(const std::string& key) {
    return key == "Last commit" || key == "Core tap last commit" ||
           key == "Core cask tap last commit" || key == "Core tap JSON" ||
           key == "Core cask tap JSON";
}

/// Parse the stable `KEY: value` lines from `brew config`, preserving order.
std::vector<std::pair<std::string, std::string>> parse_brew_config(const std::string& out) {
    std::vector<std::pair<std::string, std::string>> result;
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
        auto colon = line.find(": ");
        if (colon == std::string::npos)
            continue;
        std::string key = trim(line.substr(0, colon));
        std::string val = trim(line.substr(colon + 2));
        if (key.empty() || is_volatile_brew_key(key))
            continue;
        result.emplace_back(std::move(key), std::move(val));
    }
    return result;
}

} // namespace

HostFacts detect_host_facts() {
    HostFacts facts;

#ifdef __APPLE__
    if (auto v = capture_command("xcodebuild -version")) {
        facts.xcode_version = parse_xcode_version(*v);
    }
    if (auto info = capture_command("pkgutil --pkg-info=com.apple.pkg.CLTools_Executables")) {
        facts.clt_version = parse_pkgutil_version(*info);
    }
    facts.sdk_path = capture_command("xcrun --show-sdk-path");
#else
    read_os_release(facts.os_name, facts.os_version);
    struct utsname uts{};
    if (uname(&uts) == 0) {
        facts.kernel = std::string(uts.release);
    }
    // glibc version via `getconf GNU_LIBC_VERSION` ("glibc 2.35").
    if (auto v = capture_command("getconf GNU_LIBC_VERSION")) {
        std::istringstream ss(*v);
        std::string label;
        std::string ver;
        if (ss >> label >> ver) {
            facts.glibc = ver;
        }
    }
#endif

    if (auto cfg = capture_command("brew config")) {
        facts.homebrew_config = parse_brew_config(*cfg);
    }

    return facts;
}

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

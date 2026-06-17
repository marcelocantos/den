// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "selfupdate.h"

#include "../core/error.h"
#include "../download/http.h"
#include "../download/sha256.h"
#include "../platform/platform.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace den {

namespace fs = std::filesystem;

namespace {

/// Determine the platform suffix for tarball names (e.g. "darwin-aarch64").
std::string platform_suffix(const Config& cfg) {
#ifdef __APPLE__
    std::string os = "darwin";
#else
    std::string os = "linux";
#endif
    return os + "-" + to_string(cfg.arch);
}

/// Find the running binary's path.
fs::path self_exe() {
#ifdef __APPLE__
    // macOS: use _NSGetExecutablePath.
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        return fs::canonical(buf);
    }
    throw UserError("cannot determine executable path");
#else
    // Linux: /proc/self/exe.
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    if (ec) {
        throw UserError("cannot determine executable path: " + ec.message());
    }
    return p;
#endif
}

} // namespace

std::string resolve_latest_version() {
    std::string url = "https://api.github.com/repos/marcelocantos/den/releases/latest";

    SPDLOG_INFO("checking for latest version...");
    std::string response;
    try {
        response = fetch_url(url);
    } catch (const std::exception& e) {
        SPDLOG_WARN("failed to fetch latest version: {}", e.what());
        return {};
    }

    auto j = nlohmann::json::parse(response, nullptr, false);
    if (j.is_discarded() || !j.contains("tag_name")) {
        SPDLOG_WARN("unexpected response from GitHub releases API");
        return {};
    }

    std::string tag = j["tag_name"].get<std::string>();
    return tag.starts_with("v") ? tag.substr(1) : tag;
}

std::optional<UpdateInfo> check_for_update(const Config& cfg) {
    std::string url = "https://api.github.com/repos/marcelocantos/den/releases/latest";

    SPDLOG_INFO("checking for updates...");
    std::string response;
    try {
        response = fetch_url(url);
    } catch (const std::exception& e) {
        SPDLOG_WARN("failed to check for updates: {}", e.what());
        return std::nullopt;
    }

    auto j = nlohmann::json::parse(response, nullptr, false);
    if (j.is_discarded() || !j.contains("tag_name")) {
        SPDLOG_WARN("unexpected response from GitHub releases API");
        return std::nullopt;
    }

    std::string tag = j["tag_name"].get<std::string>();
    // Strip leading 'v'.
    std::string latest = tag.starts_with("v") ? tag.substr(1) : tag;

    if (latest == DEN_VERSION) {
        return std::nullopt;
    }

    // Find the download URL for our platform.
    std::string suffix = platform_suffix(cfg);
    std::string tarball_name = "den-" + latest + "-" + suffix + ".tar.gz";
    std::string checksum_name = tarball_name + ".sha256";

    std::string download_url;
    std::string checksum_url;

    if (j.contains("assets")) {
        for (const auto& asset : j["assets"]) {
            auto name = asset["name"].get<std::string>();
            if (name == tarball_name) {
                download_url = asset["browser_download_url"].get<std::string>();
            } else if (name == checksum_name) {
                checksum_url = asset["browser_download_url"].get<std::string>();
            }
        }
    }

    if (download_url.empty()) {
        SPDLOG_WARN("no binary available for {} in release {}", suffix, tag);
        return std::nullopt;
    }

    return UpdateInfo{
        .latest_version = latest,
        .download_url = download_url,
        .checksum_url = checksum_url,
    };
}

void apply_update(const UpdateInfo& info, const Config& cfg) {
    auto exe = self_exe();

    // Download the checksum file to get expected hash.
    std::string expected_hash;
    if (!info.checksum_url.empty()) {
        auto checksum_content = fetch_url(info.checksum_url);
        // Format: "<hash>  <filename>\n"
        std::istringstream ss(checksum_content);
        ss >> expected_hash;
    }

    // Download the tarball.
    std::cout << "Downloading den " << info.latest_version << "...\n";
    auto tarball_data = fetch_url(info.download_url);

    // Verify checksum if available.
    if (!expected_hash.empty()) {
        auto actual = hash_string(tarball_data);
        if (actual != expected_hash) {
            throw UserError("SHA256 mismatch: expected " + expected_hash + ", got " + actual);
        }
        SPDLOG_INFO("checksum verified");
    }

    // Extract to a temp directory.
    auto tmp_dir = fs::temp_directory_path() / "den-self-update";
    fs::create_directories(tmp_dir);

    auto tarball_path = tmp_dir / "den.tar.gz";
    {
        std::ofstream out(tarball_path, std::ios::binary);
        out.write(tarball_data.data(), static_cast<std::streamsize>(tarball_data.size()));
    }

    // Extract — tarball contains just "den" at the top level.
    auto rc = std::system(("tar xzf " + tarball_path.string() + " -C " + tmp_dir.string()).c_str());
    if (rc != 0) {
        fs::remove_all(tmp_dir);
        throw UserError("failed to extract update tarball");
    }

    auto new_binary = tmp_dir / "den";
    if (!fs::exists(new_binary)) {
        fs::remove_all(tmp_dir);
        throw UserError("extracted tarball does not contain 'den' binary");
    }

    // Make executable and writable (owner needs write for future updates).
    fs::permissions(new_binary,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                        fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace);

    // Atomic replace: rename new binary over old.
    // On the same filesystem this is atomic. If they're on different
    // filesystems, copy + rename.
    std::error_code ec;
    fs::rename(new_binary, exe, ec);
    if (ec) {
        // Different filesystems — copy then rename.
        auto staging = exe;
        staging += ".new";
        fs::copy_file(new_binary, staging, fs::copy_options::overwrite_existing);
        fs::rename(staging, exe, ec);
        if (ec) {
            fs::remove(staging);
            fs::remove_all(tmp_dir);
            throw UserError("failed to replace binary: " + ec.message());
        }
    }

    fs::remove_all(tmp_dir);

    std::cout << "Updated den " << DEN_VERSION << " → " << info.latest_version << "\n";
}

} // namespace den

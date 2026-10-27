// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "selfupdate.h"

#include "../core/error.h"
#include "../core/self_exe.h"
#include "../download/http.h"
#include "../download/sha256.h"
#include "../index/sat_solver.h"
#include "../platform/platform.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

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

namespace {

/// Index of the first `a/b` component pair in `comps` (with at least `after`
/// further components), or npos.
size_t find_pair(const std::vector<std::string>& comps, const char* a, const char* b,
                 size_t after) {
    for (size_t i = 0; i + 1 + after < comps.size(); ++i) {
        if (comps[i] == a && comps[i + 1] == b)
            return i;
    }
    return std::string::npos;
}

std::optional<fs::path> keg_in(const fs::path& p) {
    std::vector<std::string> comps;
    for (const auto& c : p.lexically_normal())
        comps.push_back(c.string());
    // <...>/Cellar/den/<version>/<...>
    if (size_t i = find_pair(comps, "Cellar", "den", 2); i != std::string::npos) {
        fs::path keg;
        for (size_t j = 0; j <= i + 2; ++j)
            keg /= comps[j];
        return keg;
    }
    // <P>/opt/den/<...>, only when <P> is a Homebrew prefix with a den keg.
    if (size_t i = find_pair(comps, "opt", "den", 1); i != std::string::npos) {
        fs::path prefix;
        for (size_t j = 0; j < i; ++j)
            prefix /= comps[j];
        std::error_code ec;
        if (fs::is_directory(prefix / "Cellar" / "den", ec))
            return prefix / "opt" / "den";
    }
    return std::nullopt;
}

} // namespace

std::optional<fs::path> brew_keg_for_exe(const fs::path& exe) {
    if (auto k = keg_in(exe))
        return k;
    std::error_code ec;
    auto real = fs::canonical(exe, ec);
    if (!ec) {
        if (auto k = keg_in(real))
            return k;
    }
    return std::nullopt;
}

void refuse_self_update_if_brew_managed(const fs::path& exe) {
    if (auto keg = brew_keg_for_exe(exe)) {
        throw UserError("den is installed by Homebrew (" + keg->string() +
                        "); self-update would bypass brew. Run `" + kBrewUpgradeCommand +
                        "` instead.");
    }
}

std::optional<fs::path> running_exe_path() {
    return current_executable_path();
}

void refresh_packaged_replica(const fs::path& extracted_root, const fs::path& exe) {
    // 🎯T42: keep an installed <bin>/../share/den/known_hashes.json in step
    // with the new binary — a stale on-disk copy would otherwise shadow the
    // newer snapshot compiled into it. Only refresh a copy that already
    // exists (never create or delete one); without one den uses its embedded
    // snapshot.
    auto shipped = extracted_root / "share" / "den" / "known_hashes.json";
    auto installed = exe.parent_path() / ".." / "share" / "den" / "known_hashes.json";
    std::error_code ec;
    if (!fs::is_regular_file(installed, ec)) {
        return;
    }
    if (!fs::is_regular_file(shipped, ec)) {
        SPDLOG_WARN("update tarball has no share/den/known_hashes.json; leaving {} as is",
                    installed.string());
        return;
    }
    auto staging = installed;
    staging += ".new";
    fs::copy_file(shipped, staging, fs::copy_options::overwrite_existing, ec);
    if (!ec) {
        fs::rename(staging, installed, ec);
    }
    if (ec) {
        fs::remove(staging, ec);
        SPDLOG_WARN("could not refresh trust replica {}", installed.string());
    }
}

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

    // Numeric version compare (not string equality): GitHub's "latest" may be
    // an older GA (e.g. 0.12.0) while this binary is already 1.0.0 / an RC
    // ahead of the last stable tag. Only offer an update when latest is newer.
    if (sat::version_compare(latest, DEN_VERSION) <= 0) {
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

namespace {

bool is_sha256_hex(const std::string& s) {
    if (s.size() != 64) {
        return false;
    }
    for (unsigned char c : s) {
        if (!std::isxdigit(c)) {
            return false;
        }
    }
    return true;
}

} // namespace

void apply_update(const UpdateInfo& info, const Config& cfg) {
    (void)cfg;
    auto exe = self_exe();
    // Defence in depth: the CLI refuses first, before any network access.
    refuse_self_update_if_brew_managed(exe);

    // Fail closed: never replace the running binary without a verified SHA256.
    if (info.checksum_url.empty()) {
        throw UserError(
            "self-update refused: release has no .sha256 asset — refusing to install unverified "
            "bytes");
    }

    auto checksum_content = fetch_url(info.checksum_url);
    // Format: "<hash>  <filename>\n"
    std::string expected_hash;
    {
        std::istringstream ss(checksum_content);
        ss >> expected_hash;
    }
    if (!is_sha256_hex(expected_hash)) {
        throw UserError(
            "self-update refused: checksum asset is missing or not a 64-char hex SHA256");
    }

    // Download the tarball.
    std::cout << "Downloading den " << info.latest_version << "...\n";
    auto tarball_data = fetch_url(info.download_url);

    auto actual = hash_string(tarball_data);
    if (actual != expected_hash) {
        throw UserError("SHA256 mismatch: expected " + expected_hash + ", got " + actual);
    }
    SPDLOG_INFO("checksum verified");

    // Extract to a temp directory.
    auto tmp_dir = fs::temp_directory_path() / "den-self-update";
    fs::create_directories(tmp_dir);

    auto tarball_path = tmp_dir / "den.tar.gz";
    {
        std::ofstream out(tarball_path, std::ios::binary);
        out.write(tarball_data.data(), static_cast<std::streamsize>(tarball_data.size()));
    }

    // Extract — tarball has "den" at the top level (and, from 🎯T42 on,
    // share/den/known_hashes.json beside it).
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

    refresh_packaged_replica(tmp_dir, exe);

    fs::remove_all(tmp_dir);

    std::cout << "Updated den " << DEN_VERSION << " → " << info.latest_version << "\n";
}

} // namespace den

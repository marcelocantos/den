// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "selfupdate.h"

#include "../core/error.h"
#include "../core/self_exe.h"
#include "../download/archive.h"
#include "../download/http.h"
#include "../download/sha256.h"
#include "../index/sat_solver.h"
#include "../platform/platform.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <vector>

#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace den {

namespace fs = std::filesystem;

std::string platform_suffix(const Config& cfg) {
#ifdef __APPLE__
    std::string os = "darwin";
#else
    std::string os = "linux";
#endif
    return os + "-" + to_string(cfg.arch);
}

namespace {

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
    // 🎯T42: keep <bin>/../share/den/known_hashes.json in step with the new
    // binary — a stale on-disk copy would otherwise shadow the newer
    // snapshot compiled into it. A missing copy is created (installs that
    // predate the shipped replica gain one); a tarball without a replica
    // leaves any installed copy alone.
    auto shipped = extracted_root / "share" / "den" / "known_hashes.json";
    auto installed =
        (exe.parent_path() / ".." / "share" / "den" / "known_hashes.json").lexically_normal();
    std::error_code ec;
    if (!fs::is_regular_file(shipped, ec)) {
        if (fs::is_regular_file(installed, ec)) {
            SPDLOG_WARN("update tarball has no share/den/known_hashes.json; leaving {} as is",
                        installed.string());
        }
        return;
    }
    fs::create_directories(installed.parent_path(), ec);
    if (ec) {
        SPDLOG_WARN("could not create {}: {}", installed.parent_path().string(), ec.message());
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
        SPDLOG_WARN("could not install trust replica {}", installed.string());
    }
}

std::string normalize_release_tag(const std::string& version) {
    static const std::regex re(R"(^v?([0-9]+\.[0-9]+\.[0-9]+(-rc\.[0-9]+)?)$)");
    std::smatch m;
    if (!std::regex_match(version, m, re)) {
        throw UserError("invalid release version '" + version +
                        "' (expected X.Y.Z or X.Y.Z-rc.N, optionally with a leading v)");
    }
    return "v" + m[1].str();
}

std::string asset_version_for_tag(const std::string& tag) {
    std::string v = tag.starts_with("v") ? tag.substr(1) : tag;
    if (auto dash = v.find('-'); dash != std::string::npos) {
        v.resize(dash);
    }
    return v;
}

std::optional<UpdateInfo> update_info_from_release_json(const std::string& json,
                                                        const std::string& suffix) {
    auto j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("tag_name") ||
        !j["tag_name"].is_string()) {
        SPDLOG_WARN("unexpected response from GitHub releases API");
        return std::nullopt;
    }
    std::string tag = j["tag_name"].get<std::string>();
    std::string version = asset_version_for_tag(tag);
    std::string tarball_name = "den-" + version + "-" + suffix + ".tar.gz";
    std::string checksum_name = tarball_name + ".sha256";

    UpdateInfo info{.latest_version = version, .download_url = {}, .checksum_url = {}, .tag = tag};
    if (j.contains("assets") && j["assets"].is_array()) {
        for (const auto& asset : j["assets"]) {
            if (!asset.is_object() || !asset.contains("name") || !asset["name"].is_string() ||
                !asset.contains("browser_download_url") ||
                !asset["browser_download_url"].is_string()) {
                continue;
            }
            auto name = asset["name"].get<std::string>();
            if (name == tarball_name) {
                info.download_url = asset["browser_download_url"].get<std::string>();
            } else if (name == checksum_name) {
                info.checksum_url = asset["browser_download_url"].get<std::string>();
            }
        }
    }
    if (info.download_url.empty()) {
        SPDLOG_WARN("no binary available for {} in release {}", suffix, tag);
        return std::nullopt;
    }
    return info;
}

UpdateInfo fetch_release_for_version(const Config& cfg, const std::string& version) {
    const std::string tag = normalize_release_tag(version);
    const std::string url = "https://api.github.com/repos/marcelocantos/den/releases/tags/" + tag;
    SPDLOG_INFO("fetching release {}...", tag);
    std::string response;
    try {
        response = fetch_url(url);
    } catch (const std::exception& e) {
        throw UserError("could not fetch release " + tag + ": " + e.what());
    }
    const auto suffix = platform_suffix(cfg);
    auto info = update_info_from_release_json(response, suffix);
    if (!info) {
        throw UserError("release " + tag + " has no den binary for " + suffix);
    }
    if (info->tag != tag) {
        throw UserError("GitHub returned release " + info->tag + " when asked for " + tag);
    }
    return *info;
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

    auto info = update_info_from_release_json(response, platform_suffix(cfg));
    if (!info) {
        return std::nullopt;
    }
    // Numeric version compare (not string equality): GitHub's "latest" may be
    // an older GA (e.g. 0.12.0) while this binary is already 1.0.0 / an RC
    // ahead of the last stable tag. Only offer an update when latest is newer.
    if (sat::version_compare(info->latest_version, DEN_VERSION) <= 0) {
        return std::nullopt;
    }
    return info;
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
    std::cout << "Downloading den " << (info.tag.empty() ? info.latest_version : info.tag)
              << "...\n";
    auto tarball_data = fetch_url(info.download_url);

    auto actual = hash_string(tarball_data);
    if (actual != expected_hash) {
        throw UserError("SHA256 mismatch: expected " + expected_hash + ", got " + actual);
    }
    SPDLOG_INFO("checksum verified");

    auto work = make_private_temp_dir("den-self-update");
    struct Cleanup {
        fs::path dir;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    } cleanup{work};

    auto tarball_path = work / "den.tar.gz";
    {
        std::ofstream out(tarball_path, std::ios::binary);
        out.write(tarball_data.data(), static_cast<std::streamsize>(tarball_data.size()));
        if (!out) {
            throw UserError("failed to write update tarball to " + tarball_path.string());
        }
    }

    install_update_tarball(tarball_path, exe);

    std::cout << "Updated den " << DEN_VERSION << " → " << info.latest_version;
    if (!info.tag.empty() && asset_version_for_tag(info.tag) != info.tag.substr(1)) {
        std::cout << " (" << info.tag << ")";
    }
    std::cout << "\n";
}

fs::path make_private_temp_dir(const std::string& stem) {
    std::error_code ec;
    auto base = fs::temp_directory_path(ec);
    if (ec) {
        base = "/tmp";
    }
    std::string tmpl = (base / (stem + ".XXXXXX")).string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (::mkdtemp(buf.data()) == nullptr) {
        throw UserError("cannot create a temporary directory from " + tmpl + ": " +
                        std::strerror(errno));
    }
    return fs::path(buf.data());
}

void install_update_tarball(const fs::path& tarball, const fs::path& exe) {
    auto work = make_private_temp_dir("den-self-update-x");
    struct Cleanup {
        fs::path dir;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    } cleanup{work};

    // Extract in-process with den's hardened libarchive extractor (no shell,
    // so no quoting of paths; rejects absolute, `..` and escaping symlinks).
    // The tarball has "den" at the top level (and, from 🎯T42 on,
    // share/den/known_hashes.json beside it).
    try {
        extract_archive(tarball, work);
    } catch (const std::exception& e) {
        throw UserError(std::string("failed to extract update tarball: ") + e.what());
    }

    auto new_binary = work / "den";
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(new_binary, ec))) {
        throw UserError("extracted tarball does not contain a regular 'den' binary");
    }

    // Make executable and writable (owner needs write for future updates).
    fs::permissions(new_binary,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                        fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace);

    // Atomic replace onto a fresh inode. Never write into the existing file:
    // macOS validates code signatures per vnode, so rewriting the bytes of
    // the inode a running den (e.g. the daemon) is mapped from gets that
    // process killed with OS_REASON_CODESIGNING. rename() swaps the
    // directory entry to the extracted file's own inode; the old inode lives
    // on, untouched, until every process using it exits. Across filesystems,
    // copy to a uniquely named, newly created file beside the target first,
    // then rename that over it.
    fs::rename(new_binary, exe, ec);
    if (ec) {
        auto staging = exe;
        staging += ".new." + std::to_string(static_cast<long>(::getpid()));
        std::error_code ec2;
        fs::remove(staging, ec2); // never reuse a stale file's inode
        ec.clear();
        fs::copy_file(new_binary, staging, fs::copy_options::none, ec);
        if (!ec) {
            fs::rename(staging, exe, ec);
        }
        if (ec) {
            fs::remove(staging, ec2);
            throw UserError("failed to replace binary: " + ec.message());
        }
    }

    refresh_packaged_replica(work, exe);
}

} // namespace den

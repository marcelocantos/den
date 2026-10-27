// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "cask.h"
#include "uninstall_stanza.h"

#include "../core/error.h"
#include "../download/archive.h"
#include "../download/fetch.h"
#include "../download/sha256.h"
#include "../env/environment.h"
#include "../env/manifest.h"
#include "../index/cask_uninstall_json.h"
#include "../platform/platform.h"
#include "../settings/settings.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <system_error>

namespace den {

namespace {

using json = nlohmann::json;

/// Run a shell command; return exit status (0 = success). Captures nothing.
int run_cmd(const std::string& cmd) {
    SPDLOG_DEBUG("cask: exec {}", cmd);
    return std::system(cmd.c_str());
}

/// Shell-escape a path for embedding in a double-quoted command string.
std::string sh_quote(const fs::path& p) {
    std::string s = p.string();
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"' || c == '$' || c == '`') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string lower_ext(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

bool looks_like_zip(const fs::path& p) {
    auto e = lower_ext(p);
    return e == ".zip" || p.string().ends_with(".tar.gz") || p.string().ends_with(".tgz") ||
           p.string().ends_with(".tar.xz") || p.string().ends_with(".tar.bz2");
}

bool looks_like_dmg(const fs::path& p) {
    return lower_ext(p) == ".dmg";
}

bool looks_like_pkg(const fs::path& p) {
    return lower_ext(p) == ".pkg";
}

/// Clear Gatekeeper quarantine on a path tree. Homebrew does the same after
/// placing cask artifacts so first-launch is not blocked by a download xattr.
/// We only strip com.apple.quarantine — other xattrs are left alone.
void clear_quarantine(const fs::path& path) {
#ifdef __APPLE__
    if (!fs::exists(path)) {
        return;
    }
    // xattr -dr is the documented, deliberate quarantine handling for den casks.
    const std::string cmd = "xattr -dr com.apple.quarantine " + sh_quote(path) + " 2>/dev/null";
    (void)run_cmd(cmd);
#else
    (void)path;
#endif
}

/// Copy an .app bundle with ditto (preserves resource forks / Finder info).
void copy_app_bundle(const fs::path& src, const fs::path& dst) {
#ifdef __APPLE__
    fs::create_directories(dst.parent_path());
    if (fs::exists(dst)) {
        std::error_code ec;
        fs::remove_all(dst, ec);
    }
    const int rc = run_cmd("ditto " + sh_quote(src) + " " + sh_quote(dst));
    if (rc != 0) {
        throw Error("failed to copy app bundle with ditto: " + src.string() + " -> " +
                    dst.string());
    }
#else
    fs::create_directories(dst.parent_path());
    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
#endif
}

void write_receipt(const fs::path& version_dir, const CaskReceipt& receipt) {
    json j;
    j["poured_by"] = "den";
    j["token"] = receipt.token;
    j["version"] = receipt.version;
    j["quarantine_cleared"] = receipt.quarantine_cleared;
    j["apps"] = json::array();
    for (const auto& a : receipt.apps) {
        j["apps"].push_back(a.string());
    }
    j["binaries"] = json::array();
    for (const auto& b : receipt.binaries) {
        j["binaries"].push_back({
            {"link", b.link.string()},
            {"target", b.target.string()},
        });
    }
    j["pkgs"] = receipt.pkgs;
    if (!receipt.fonts.empty()) {
        j["fonts"] = json::array();
        for (const auto& f : receipt.fonts) {
            j["fonts"].push_back(f.string());
        }
    }
    if (!receipt.uninstall.empty()) {
        j["uninstall"] = uninstall_to_json(receipt.uninstall);
    }

    const auto dest = version_dir / kDenCaskReceiptFilename;
    const auto tmp = version_dir / (std::string(kDenCaskReceiptFilename) + ".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            throw Error("cannot write cask receipt: " + tmp.string());
        }
        out << j.dump(2) << '\n';
        out.flush();
        if (!out) {
            std::error_code ec;
            fs::remove(tmp, ec);
            throw Error("failed writing cask receipt: " + tmp.string());
        }
    }
    std::error_code ec;
    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw Error("failed to install cask receipt: " + ec.message());
    }
}

/// Resolve the binary artifact source path inside a stage directory.
/// Handles bare names, nested paths, and "$APPDIR/Foo.app/..." references
/// once apps have been installed.
fs::path resolve_binary_source(const fs::path& stage, const fs::path& appdir,
                               const std::string& source) {
    if (source.starts_with("$APPDIR/") || source.starts_with("$appdir/")) {
        return appdir / source.substr(std::string("$APPDIR/").size());
    }
    if (!source.empty() && source[0] == '/') {
        return fs::path(source);
    }
    fs::path candidate = stage / source;
    if (fs::exists(candidate)) {
        return candidate;
    }
    // Some zip layouts nest under a single top-level directory.
    std::error_code ec;
    if (fs::is_directory(stage, ec)) {
        for (const auto& entry : fs::directory_iterator(stage, ec)) {
            if (!entry.is_directory()) {
                continue;
            }
            auto nested = entry.path() / source;
            if (fs::exists(nested)) {
                return nested;
            }
        }
    }
    return candidate;
}

/// If `source` is an "$APPDIR/<App>.app/..." reference, return the part after
/// "$APPDIR/"; otherwise nullopt.
std::optional<std::string> appdir_relative(const std::string& source) {
    for (const char* prefix : {"$APPDIR/", "$appdir/"}) {
        if (source.starts_with(prefix)) {
            return source.substr(std::string_view(prefix).size());
        }
    }
    return std::nullopt;
}

/// Basename of the link den should create for a binary artifact.
std::string binary_link_name(const CaskArtifact& art) {
    if (!art.target.empty()) {
        // Targets are often "$HOMEBREW_PREFIX/bin/foo" — take the filename.
        return fs::path(art.target).filename().string();
    }
    return fs::path(art.source).filename().string();
}

/// Mount a DMG read-only at mount_point. Returns true on success.
bool attach_dmg(const fs::path& dmg, const fs::path& mount_point) {
#ifdef __APPLE__
    fs::create_directories(mount_point);
    const std::string cmd = "hdiutil attach -nobrowse -readonly -mountpoint " +
                            sh_quote(mount_point) + " " + sh_quote(dmg) + " >/dev/null";
    return run_cmd(cmd) == 0;
#else
    (void)dmg;
    (void)mount_point;
    return false;
#endif
}

void detach_dmg(const fs::path& mount_point) {
#ifdef __APPLE__
    if (!fs::exists(mount_point)) {
        return;
    }
    // Best-effort; force if a stray handle remains.
    if (run_cmd("hdiutil detach " + sh_quote(mount_point) + " >/dev/null 2>&1") != 0) {
        (void)run_cmd("hdiutil detach -force " + sh_quote(mount_point) + " >/dev/null 2>&1");
    }
#else
    (void)mount_point;
#endif
}

struct MountGuard {
    fs::path mount_point;
    bool active = false;
    ~MountGuard() {
        if (active) {
            detach_dmg(mount_point);
        }
    }
};

/// Stage the downloaded archive into version_dir/stage and return that path.
fs::path stage_archive(const fs::path& archive, const fs::path& version_dir) {
    const fs::path stage = version_dir / "stage";
    std::error_code ec;
    fs::remove_all(stage, ec);
    fs::create_directories(stage);

    if (looks_like_dmg(archive)) {
#ifdef __APPLE__
        const fs::path mnt = version_dir / "mnt";
        fs::remove_all(mnt, ec);
        MountGuard guard{mnt, false};
        if (!attach_dmg(archive, mnt)) {
            throw Error("failed to mount DMG: " + archive.string());
        }
        guard.active = true;
        // Copy mount contents into stage so we can detach immediately.
        // CRITICAL: DMGs often contain an "Applications" symlink pointing at
        // the real /Applications. Never follow that — it would copy the user's
        // entire Applications folder into the stage. Skip all symlinks and
        // hidden entries; only materialise real files and .app directories.
        for (const auto& entry : fs::directory_iterator(mnt, ec)) {
            const auto name = entry.path().filename().string();
            if (name.starts_with(".")) {
                continue; // skip .Trashes, .DS_Store, .background, …
            }
            std::error_code sec;
            if (entry.is_symlink(sec) || sec) {
                SPDLOG_DEBUG("cask: skipping DMG symlink {}", name);
                continue;
            }
            const auto dest = stage / name;
            if (entry.is_directory(sec)) {
                // Prefer ditto for .app bundles; plain recursive copy otherwise.
                if (name.ends_with(".app")) {
                    copy_app_bundle(entry.path(), dest);
                } else {
                    fs::copy(entry.path(), dest,
                             fs::copy_options::recursive | fs::copy_options::copy_symlinks |
                                 fs::copy_options::skip_symlinks,
                             ec);
                }
            } else if (entry.is_regular_file(sec)) {
                fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, ec);
            }
        }
        guard.active = false;
        detach_dmg(mnt);
        fs::remove_all(mnt, ec);
#else
        throw UserError("DMG casks require macOS");
#endif
    } else if (looks_like_zip(archive) || looks_like_pkg(archive)) {
        // pkg is often wrapped; if the download is a bare .pkg, just copy it.
        if (looks_like_pkg(archive)) {
            fs::copy_file(archive, stage / archive.filename(),
                          fs::copy_options::overwrite_existing);
        } else {
            extract_archive(archive, stage);
        }
    } else {
        // Bare binary (no container): place under stage using the URL basename.
        const auto name =
            archive.filename().string().empty() ? "binary" : archive.filename().string();
        fs::copy_file(archive, stage / name, fs::copy_options::overwrite_existing);
    }

    return stage;
}

/// Find a named .app under stage (exact relative path, or search by basename).
fs::path find_app_in_stage(const fs::path& stage, const std::string& source) {
    fs::path direct = stage / source;
    if (fs::exists(direct)) {
        return direct;
    }
    // Search for basename match (DMGs sometimes nest under a top folder).
    const std::string want = fs::path(source).filename().string();
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             stage, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (it->path().filename().string() == want) {
            return it->path();
        }
    }
    return direct;
}

void record_settings_cask(const Config& config, const std::string& token,
                          const std::string& version) {
    auto settings = read_settings(config.den_home);
    settings.casks[token] = version;
    write_settings(config.den_home, settings);
}

void erase_settings_cask(const Config& config, const std::string& token) {
    auto settings = read_settings(config.den_home);
    settings.casks.erase(token);
    write_settings(config.den_home, settings);
}

/// Install a .pkg with the system installer. Requires admin on real systems;
/// DEN_CASK_PKG_ROOT can redirect -target for tests.
bool install_pkg_file(const fs::path& pkg_file) {
#ifdef __APPLE__
    std::string target = "/";
    if (const char* root = std::getenv("DEN_CASK_PKG_ROOT")) {
        if (root[0] != '\0') {
            target = root;
        }
    }
    const std::string cmd = "installer -pkg " + sh_quote(pkg_file) + " -target " + sh_quote(target);
    std::cout << "==> Running installer for " << pkg_file.filename().string() << "\n";
    return run_cmd(cmd) == 0;
#else
    (void)pkg_file;
    return false;
#endif
}

/// Test-only fault injection for cask install/upgrade. When DEN_CASK_FAULT
/// names `stage` (download, extract, copy, quarantine, swap, receipt), throw
/// as if that step failed. Used by regression tests and the den-test-mac
/// proof to show a failed upgrade never damages the installed version.
/// Print a stanza report (actions → stdout, warnings/admin → stderr) and
/// append it to <den_home>/cask.log.
void emit_stanza_report(const Config& config, const std::string& token, const char* phase,
                        const StanzaReport& r) {
    for (const auto& a : r.actions)
        std::cout << "==> " << a << "\n";
    for (const auto& w : r.warnings)
        std::cerr << "warning: " << token << ": " << w << "\n";
    for (const auto& c : r.needs_admin)
        std::cerr << "warning: " << token << ": needs admin (den does not escalate): run `" << c
                  << "`\n";
    if (r.actions.empty() && r.warnings.empty() && r.needs_admin.empty())
        return;
    std::error_code ec;
    fs::create_directories(config.den_home, ec);
    std::ofstream log(config.den_home / "cask.log", std::ios::app);
    const auto now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    for (const auto& a : r.actions)
        log << ts << " " << token << " " << phase << " " << a << "\n";
    for (const auto& w : r.warnings)
        log << ts << " " << token << " " << phase << " WARNING " << w << "\n";
    for (const auto& c : r.needs_admin)
        log << ts << " " << token << " " << phase << " NEEDS-ADMIN " << c << "\n";
}

void maybe_inject_fault(std::string_view stage) {
    if (const char* f = std::getenv("DEN_CASK_FAULT")) {
        if (stage == f) {
            throw Error("injected cask fault at stage '" + std::string(stage) + "'");
        }
    }
}

} // namespace

fs::path den_caskroom(const Config& config) {
    return config.den_home / "caskroom";
}

fs::path cask_version_path(const Config& config, const std::string& token,
                           const std::string& version) {
    return den_caskroom(config) / token / version;
}

fs::path cask_appdir(const Config& config) {
    if (const char* env = std::getenv("DEN_APPDIR")) {
        if (env[0] != '\0') {
            return fs::path(env);
        }
    }
    return config.appdir;
}

std::optional<CaskReceipt> read_cask_receipt(const fs::path& version_dir) {
    const auto path = version_dir / kDenCaskReceiptFilename;
    std::error_code ec;
    if (fs::is_symlink(path, ec) || ec) {
        return std::nullopt;
    }
    if (!fs::is_regular_file(path, ec) || ec) {
        return std::nullopt;
    }
    std::ifstream in(path);
    if (!in) {
        return std::nullopt;
    }
    try {
        const auto j = json::parse(in);
        if (!j.is_object()) {
            return std::nullopt;
        }
        if (!j.contains("poured_by") || j["poured_by"] != "den") {
            return std::nullopt;
        }
        CaskReceipt r;
        r.token = j.value("token", "");
        r.version = j.value("version", "");
        r.quarantine_cleared = j.value("quarantine_cleared", false);
        if (j.contains("apps") && j["apps"].is_array()) {
            for (const auto& a : j["apps"]) {
                if (a.is_string()) {
                    r.apps.push_back(a.get<std::string>());
                }
            }
        }
        if (j.contains("binaries") && j["binaries"].is_array()) {
            for (const auto& b : j["binaries"]) {
                if (!b.is_object()) {
                    continue;
                }
                CaskBinaryLink link;
                link.link = b.value("link", "");
                link.target = b.value("target", "");
                r.binaries.push_back(std::move(link));
            }
        }
        if (j.contains("pkgs") && j["pkgs"].is_array()) {
            for (const auto& p : j["pkgs"]) {
                if (p.is_string()) {
                    r.pkgs.push_back(p.get<std::string>());
                }
            }
        }
        if (j.contains("fonts") && j["fonts"].is_array()) {
            for (const auto& f : j["fonts"]) {
                if (f.is_string()) {
                    r.fonts.push_back(f.get<std::string>());
                }
            }
        }
        if (j.contains("uninstall")) {
            merge_uninstall_directives(j["uninstall"], r.uninstall);
        }
        if (r.token.empty() || r.version.empty()) {
            return std::nullopt;
        }
        // Receipt must name this directory.
        if (r.token != version_dir.parent_path().filename().string()) {
            return std::nullopt;
        }
        if (r.version != version_dir.filename().string()) {
            return std::nullopt;
        }
        return r;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

namespace {

/// True when `path` is listed in `field` of any den cask receipt.
bool listed_in_den_receipt(const Config& config, const fs::path& app_path,
                           std::vector<fs::path> CaskReceipt::* field) {
    const auto room = den_caskroom(config);
    std::error_code ec;
    if (!fs::is_directory(room, ec) || ec) {
        return false;
    }
    // Normalise for comparison.
    const fs::path want = fs::weakly_canonical(app_path, ec);
    const fs::path want_lex = ec ? app_path.lexically_normal() : want;

    for (const auto& token_ent : fs::directory_iterator(room, ec)) {
        if (!token_ent.is_directory()) {
            continue;
        }
        for (const auto& ver_ent : fs::directory_iterator(token_ent.path(), ec)) {
            if (!ver_ent.is_directory()) {
                continue;
            }
            auto receipt = read_cask_receipt(ver_ent.path());
            if (!receipt) {
                continue;
            }
            for (const auto& a : (*receipt).*field) {
                std::error_code ec2;
                fs::path have = fs::weakly_canonical(a, ec2);
                if (ec2) {
                    have = a.lexically_normal();
                }
                if (have == want_lex) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

bool app_owned_by_den(const Config& config, const fs::path& app_path) {
    return listed_in_den_receipt(config, app_path, &CaskReceipt::apps);
}

bool font_owned_by_den(const Config& config, const fs::path& font_path) {
    return listed_in_den_receipt(config, font_path, &CaskReceipt::fonts);
}

fs::path cask_fontdir(const Config& config) {
    (void)config;
    if (const char* env = std::getenv("DEN_FONTDIR")) {
        if (env[0] != '\0') {
            return fs::path(env);
        }
    }
    const char* home = std::getenv("HOME");
    return fs::path(home && home[0] != '\0' ? home : "/") / "Library" / "Fonts";
}

bool is_cask_installed(const Config& config, std::string_view token) {
    return !installed_cask_version(config, token).empty();
}

std::string installed_cask_version(const Config& config, std::string_view token) {
    const auto token_dir = den_caskroom(config) / std::string(token);
    std::error_code ec;
    if (!fs::is_directory(token_dir, ec) || ec) {
        return {};
    }
    // Prefer settings; fall back to any receipt on disk.
    auto settings = read_settings(config.den_home);
    if (auto it = settings.casks.find(std::string(token)); it != settings.casks.end()) {
        if (read_cask_receipt(token_dir / it->second)) {
            return it->second;
        }
    }
    std::string found;
    for (const auto& ver_ent : fs::directory_iterator(token_dir, ec)) {
        if (!ver_ent.is_directory()) {
            continue;
        }
        if (read_cask_receipt(ver_ent.path())) {
            found = ver_ent.path().filename().string();
        }
    }
    return found;
}

std::vector<InstalledPackage> list_den_casks(const Config& config) {
    std::vector<InstalledPackage> out;
    const auto room = den_caskroom(config);
    std::error_code ec;
    if (!fs::is_directory(room, ec) || ec) {
        return out;
    }
    for (const auto& token_ent : fs::directory_iterator(room, ec)) {
        if (!token_ent.is_directory()) {
            continue;
        }
        const std::string token = token_ent.path().filename().string();
        if (token.starts_with(".")) {
            continue;
        }
        std::string version = installed_cask_version(config, token);
        if (version.empty()) {
            continue;
        }
        out.push_back(InstalledPackage{
            .name = token,
            .version = version,
            .path = cask_version_path(config, token, version),
        });
    }
    std::sort(out.begin(), out.end(),
              [](const InstalledPackage& a, const InstalledPackage& b) { return a.name < b.name; });
    return out;
}

InstallResult install_cask(const Config& config, const Package& pkg) {
#ifndef __APPLE__
    (void)config;
    (void)pkg;
    throw UserError("casks are not supported on Linux (macOS only)");
#else
    if (pkg.archives.empty()) {
        throw UserError("cask '" + pkg.name + "' has no download URL in the index");
    }

    // Prefer platform bottle-style tags if present, else "default".
    std::vector<std::string> tags;
    tags.reserve(pkg.archives.size());
    for (const auto& [tag, _] : pkg.archives) {
        tags.push_back(tag);
    }
    auto best = best_archive_tag(config.arch, config.macos_version, tags);
    if (!best) {
        throw UserError("no archive available for cask '" + pkg.name + "' on this platform");
    }
    const auto& archive = pkg.archives.at(*best);
    if (archive.url.empty()) {
        throw UserError("cask '" + pkg.name + "' has an empty download URL");
    }

    // Strip optional "cask/" disambiguation prefix for on-disk token.
    std::string token = pkg.name;
    if (token.starts_with("cask/")) {
        token = token.substr(5);
    }

    // Upgrade safety: the previously installed version (if any) is left
    // completely untouched until the new version is fully staged. Only the
    // final swap modifies the appdir, and it rolls back on any failure.
    std::optional<CaskReceipt> old_receipt;
    std::string old_version;
    if (is_cask_installed(config, token)) {
        old_version = installed_cask_version(config, token);
        if (old_version == pkg.version) {
            SPDLOG_INFO("cask {} {} already installed", token, old_version);
            InstallResult r;
            r.resolved_version = pkg.version;
            return r;
        }
        old_receipt = read_cask_receipt(cask_version_path(config, token, old_version));
    }

    const auto version_dir = cask_version_path(config, token, pkg.version);
    const fs::path appdir = cask_appdir(config);
    // Staging lives inside the appdir so the final swap is a same-volume rename.
    const fs::path app_staging = appdir / (".den-staging-" + token + "-" + pkg.version);

    std::error_code ec;
    // Everything below up to the swap only creates new state; on failure we
    // discard it and the old version is exactly as it was.
    auto discard_new = [&]() {
        std::error_code dec;
        fs::remove_all(app_staging, dec);
        fs::remove_all(version_dir, dec);
    };

    struct StagedApp {
        fs::path staged;
        fs::path dest;
    };
    struct PlannedLink {
        fs::path link;
        fs::path target;
    };
    struct PlannedFont {
        fs::path source; // inside the stage
        fs::path dest;
    };
    std::vector<StagedApp> staged_apps;
    std::vector<PlannedLink> planned_links;
    std::vector<PlannedFont> planned_fonts;
    const fs::path fontdir = cask_fontdir(config);

    CaskReceipt receipt;
    receipt.token = token;
    receipt.version = pkg.version;
    receipt.uninstall = pkg.cask_uninstall;
    if (!pkg.cask_uninstall.unsupported.empty()) {
        std::string names;
        for (const auto& n : pkg.cask_uninstall.unsupported)
            names += (names.empty() ? "" : ", ") + n;
        std::cerr << "warning: " << token
                  << ": uninstall directive(s) den does not run yet: " << names
                  << " (recorded in the receipt; uninstall will remind you)\n";
    }

    try {
        fs::remove_all(version_dir, ec);
        fs::create_directories(version_dir);

        std::cout << "==> Downloading cask " << token << " " << pkg.version << "\n";
        fs::path archive_path;
        // Local fixtures (unit tests) may pass file:// or absolute paths.
        if (archive.url.starts_with("file://")) {
            archive_path = fs::path(archive.url.substr(7));
            if (!fs::exists(archive_path)) {
                throw UserError("local cask archive not found: " + archive_path.string());
            }
        } else if (!archive.url.empty() && archive.url[0] == '/') {
            archive_path = fs::path(archive.url);
            if (!fs::exists(archive_path)) {
                throw UserError("local cask archive not found: " + archive_path.string());
            }
        } else {
            auto cache_dir = config.cache / "cask-archives";
            fs::create_directories(cache_dir);
            archive_path = fetch_archive(archive.url, archive.sha256, cache_dir);
        }
        maybe_inject_fault("download");

        std::cout << "==> Staging " << token << "\n";
        const fs::path stage = stage_archive(archive_path, version_dir);
        maybe_inject_fault("extract");

        fs::create_directories(appdir);

        // Determine artifacts. If the index carried none, fall back to "any
        // .app in the stage" so simple casks still install after a partial index.
        std::vector<CaskArtifact> artifacts = pkg.cask_artifacts;
        // Apps are staged before anything else so a binary that points into
        // one ("$APPDIR/Foo.app/...") can be resolved against the staged copy
        // regardless of the order the index lists the artifacts in.
        std::stable_partition(artifacts.begin(), artifacts.end(), [](const CaskArtifact& a) {
            return a.kind == CaskArtifactKind::App;
        });
        if (artifacts.empty()) {
            for (auto it = fs::recursive_directory_iterator(
                     stage, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }
                if (it->path().extension() == ".app" && it->is_directory()) {
                    CaskArtifact a;
                    a.kind = CaskArtifactKind::App;
                    a.source = fs::relative(it->path(), stage).string();
                    artifacts.push_back(std::move(a));
                    it.disable_recursion_pending();
                }
            }
        }

        bool placed_anything = false;
        const auto active = active_env_path(config.den_home);
        const fs::path bin_dir = env_dir(config.den_home, active) / "bin";

        for (const auto& art : artifacts) {
            if (art.kind == CaskArtifactKind::App) {
                const fs::path src = find_app_in_stage(stage, art.source);
                if (!fs::exists(src)) {
                    throw UserError("cask '" + token +
                                    "': app not found in archive: " + art.source);
                }
                const std::string app_name = art.target.empty()
                                                 ? src.filename().string()
                                                 : fs::path(art.target).filename().string();
                if (app_name.find('/') != std::string::npos || app_name == ".." ||
                    app_name.find("..") != std::string::npos) {
                    throw UserError("cask '" + token + "': unsafe app name: " + app_name);
                }
                const fs::path dest = appdir / app_name;
                if (fs::exists(dest) && !app_owned_by_den(config, dest)) {
                    throw UserError("refusing to overwrite '" + dest.string() +
                                    "': not installed by den. "
                                    "Remove it manually or choose a different DEN_APPDIR.");
                }
                const fs::path staged = app_staging / app_name;
                std::cout << "==> Staging " << app_name << " in " << appdir.string() << "\n";
                copy_app_bundle(src, staged);
                maybe_inject_fault("copy");
                // Codesign/quarantine step runs on the staged copy, never on
                // the live app.
                clear_quarantine(staged);
                maybe_inject_fault("quarantine");
                staged_apps.push_back({staged, dest});
                receipt.apps.push_back(dest);
                placed_anything = true;
            } else if (art.kind == CaskArtifactKind::Binary) {
                const std::string link_name = binary_link_name(art);
                if (link_name.empty() || link_name.find('/') != std::string::npos) {
                    throw UserError("cask '" + token + "': unsafe binary name");
                }
                // "$APPDIR/Foo.app/..." inside an app this cask is installing:
                // the app is still in staging, so check the staged copy and
                // link to where it will live after the swap. The binary stays
                // inside the bundle (as Homebrew does) so bundle-relative
                // resources keep working.
                if (const auto rel = appdir_relative(art.source)) {
                    const fs::path rel_path = fs::path(*rel).lexically_normal();
                    const auto first = rel_path.begin();
                    if (first != rel_path.end() && *first != ".." && rel_path.is_relative()) {
                        const auto sa = std::find_if(
                            staged_apps.begin(), staged_apps.end(),
                            [&](const StagedApp& s) { return s.dest.filename() == *first; });
                        if (sa != staged_apps.end()) {
                            const fs::path inner = rel_path.lexically_relative(*first);
                            const fs::path staged_src = sa->staged / inner;
                            if (!fs::exists(staged_src)) {
                                throw UserError("cask '" + token +
                                                "': binary not found: " + art.source);
                            }
                            fs::permissions(staged_src,
                                            fs::perms::owner_exec | fs::perms::group_exec |
                                                fs::perms::others_exec,
                                            fs::perm_options::add, ec);
                            const fs::path final_target = sa->dest / inner;
                            const fs::path link = bin_dir / link_name;
                            if (!fs::is_symlink(link, ec) && fs::exists(link, ec)) {
                                throw UserError("refusing to overwrite binary '" + link.string() +
                                                "'");
                            }
                            planned_links.push_back({link, final_target});
                            receipt.binaries.push_back(CaskBinaryLink{link, final_target});
                            placed_anything = true;
                            continue;
                        }
                    }
                }
                const fs::path src = resolve_binary_source(stage, appdir, art.source);
                if (!fs::exists(src)) {
                    throw UserError("cask '" + token + "': binary not found: " + art.source);
                }
                // Keep a durable copy under the stage so env rebuilds can relink.
                fs::path staged_bin = stage / link_name;
                if (src.string().find(stage.string()) == 0) {
                    if (!fs::exists(staged_bin)) {
                        staged_bin = src;
                    }
                } else if (!fs::exists(staged_bin)) {
                    fs::copy_file(src, staged_bin, fs::copy_options::overwrite_existing);
                }
                fs::permissions(staged_bin,
                                fs::perms::owner_exec | fs::perms::group_exec |
                                    fs::perms::others_exec,
                                fs::perm_options::add, ec);
                clear_quarantine(staged_bin);
                const fs::path link = bin_dir / link_name;
                if (!fs::is_symlink(link, ec) && fs::exists(link, ec)) {
                    throw UserError("refusing to overwrite binary '" + link.string() + "'");
                }
                planned_links.push_back({link, staged_bin});
                receipt.binaries.push_back(CaskBinaryLink{link, staged_bin});
                placed_anything = true;
            } else if (art.kind == CaskArtifactKind::Font) {
                const fs::path src = find_app_in_stage(stage, art.source);
                if (!fs::exists(src)) {
                    throw UserError("cask '" + token +
                                    "': font not found in archive: " + art.source);
                }
                // Homebrew's target is "/$HOME/Library/Fonts/<name>"; only the
                // basename matters, the directory is cask_fontdir.
                const std::string font_name = art.target.empty()
                                                  ? src.filename().string()
                                                  : fs::path(art.target).filename().string();
                if (font_name.empty() || font_name == "." ||
                    font_name.find('/') != std::string::npos ||
                    font_name.find("..") != std::string::npos) {
                    throw UserError("cask '" + token + "': unsafe font name: " + font_name);
                }
                const fs::path dest = fontdir / font_name;
                if ((fs::exists(dest, ec) || fs::is_symlink(dest, ec)) &&
                    !font_owned_by_den(config, dest)) {
                    throw UserError("refusing to overwrite '" + dest.string() +
                                    "': not installed by den. "
                                    "Remove it manually or choose a different DEN_FONTDIR.");
                }
                planned_fonts.push_back({src, dest});
                receipt.fonts.push_back(dest);
                placed_anything = true;
            } else if (art.kind == CaskArtifactKind::Pkg) {
                fs::path pkg_file = stage / art.source;
                if (!fs::exists(pkg_file)) {
                    pkg_file = find_app_in_stage(stage, art.source);
                }
                if (!fs::exists(pkg_file)) {
                    // Content-addressed downloads keep the sha256 as the
                    // filename (e.g. <hash>.pkg). Accept any single .pkg.
                    std::error_code pec;
                    std::vector<fs::path> pkgs;
                    for (const auto& e : fs::directory_iterator(stage, pec)) {
                        if (e.is_regular_file() && e.path().extension() == ".pkg") {
                            pkgs.push_back(e.path());
                        }
                    }
                    if (pkgs.size() == 1) {
                        pkg_file = pkgs.front();
                    } else if (!pkgs.empty()) {
                        const auto want = fs::path(art.source).filename().string();
                        for (const auto& p : pkgs) {
                            if (p.filename().string() == want) {
                                pkg_file = p;
                                break;
                            }
                        }
                        if (!fs::exists(pkg_file)) {
                            pkg_file = pkgs.front();
                        }
                    }
                }
                if (!fs::exists(pkg_file)) {
                    throw UserError("cask '" + token + "': pkg not found: " + art.source);
                }
                // NOTE: the system installer cannot be staged or rolled back;
                // a pkg failure aborts before the app swap, leaving apps intact.
                if (!install_pkg_file(pkg_file)) {
                    throw UserError(
                        "cask '" + token +
                        "': pkg install failed (is admin authorisation available?). "
                        "PKG casks may require an interactive sudo via /usr/bin/installer.");
                }
                receipt.pkgs.push_back(art.source);
                placed_anything = true;
            }
        }

        if (!placed_anything) {
            throw UserError("cask '" + token +
                            "' has no supported artifacts (need app, binary, font, or pkg). "
                            "Complex casks (installer scripts, preflight) are not supported yet.");
        }
    } catch (...) {
        discard_new();
        throw;
    }

    // ---- Stop phase (upgrade only): quit/signal/launchctl from the old
    // version's stanza, right before the swap, once the new version is
    // fully staged. Falls back to the new cask's stanza for receipts that
    // predate stanza recording. Destructive directives are not run here.
    if (old_receipt) {
        const CaskUninstall& stop =
            old_receipt->uninstall.empty() ? pkg.cask_uninstall : old_receipt->uninstall;
        if (!stop.empty()) {
            std::cout << "==> Stopping " << token << " " << old_version << " before upgrade\n";
            emit_stanza_report(config, token, "upgrade-stop",
                               run_uninstall_stanza(stop, StanzaPhase::Stop, cask_system(),
                                                    default_stanza_options(appdir)));
        }
    }

    // ---- Swap phase: the only part that touches live state. ----
    struct MovedApp {
        fs::path dest;
        fs::path backup; // empty if nothing existed at dest
        bool placed = false;
    };
    struct ReplacedLink {
        fs::path link;
        fs::path old_target; // empty if no link existed
        bool replaced = false;
    };
    std::vector<MovedApp> moved;
    std::vector<ReplacedLink> relinked;
    std::vector<MovedApp> placed_fonts;
    const fs::path backup_dir = appdir / (".den-backup-" + token + "-" + pkg.version);
    // Same directory as the fonts, so backing up an old den font is a rename.
    const fs::path font_backup_dir = fontdir / (".den-backup-" + token + "-" + pkg.version);

    auto rollback = [&]() {
        std::error_code rec;
        for (auto it = placed_fonts.rbegin(); it != placed_fonts.rend(); ++it) {
            if (it->placed) {
                fs::remove_all(it->dest, rec);
            }
            if (!it->backup.empty()) {
                fs::rename(it->backup, it->dest, rec);
                if (rec) {
                    std::cerr << "error: could not restore " << it->dest.string() << " from "
                              << it->backup.string() << ": " << rec.message() << "\n";
                }
            }
        }
        fs::remove_all(font_backup_dir, rec);
        for (auto it = relinked.rbegin(); it != relinked.rend(); ++it) {
            if (!it->replaced) {
                continue;
            }
            fs::remove(it->link, rec);
            if (!it->old_target.empty()) {
                fs::create_symlink(it->old_target, it->link, rec);
            }
        }
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            if (it->placed) {
                fs::remove_all(it->dest, rec);
            }
            if (!it->backup.empty()) {
                fs::rename(it->backup, it->dest, rec);
                if (rec) {
                    std::cerr << "error: could not restore " << it->dest.string() << " from "
                              << it->backup.string() << ": " << rec.message() << "\n";
                }
            }
        }
        fs::remove_all(backup_dir, rec);
        discard_new();
    };

    try {
        std::cout << "==> Installing " << token << " " << pkg.version << "\n";
        for (const auto& sa : staged_apps) {
            MovedApp m{sa.dest, {}, false};
            if (fs::exists(sa.dest) || fs::is_symlink(sa.dest)) {
                fs::create_directories(backup_dir);
                m.backup = backup_dir / sa.dest.filename();
                fs::rename(sa.dest, m.backup);
            }
            moved.push_back(m);
            maybe_inject_fault("swap");
            fs::rename(sa.staged, sa.dest);
            moved.back().placed = true;
        }
        for (const auto& pl : planned_links) {
            fs::create_directories(pl.link.parent_path());
            ReplacedLink r{pl.link, {}, false};
            if (fs::is_symlink(pl.link)) {
                r.old_target = fs::read_symlink(pl.link);
                fs::remove(pl.link);
            }
            relinked.push_back(r);
            fs::create_symlink(pl.target, pl.link);
            relinked.back().replaced = true;
            std::cout << "==> Linked " << pl.link.filename().string() << "\n";
        }
        for (const auto& pf : planned_fonts) {
            fs::create_directories(pf.dest.parent_path());
            MovedApp m{pf.dest, {}, false};
            if (fs::exists(pf.dest) || fs::is_symlink(pf.dest)) {
                fs::create_directories(font_backup_dir);
                m.backup = font_backup_dir / pf.dest.filename();
                fs::rename(pf.dest, m.backup);
            }
            placed_fonts.push_back(m);
            // Copy beside the destination, then rename, so a font never
            // appears half-written.
            const fs::path tmp =
                pf.dest.parent_path() / (".den-tmp-" + pf.dest.filename().string());
            fs::remove_all(tmp);
            fs::copy(pf.source, tmp, fs::copy_options::recursive);
            clear_quarantine(tmp);
            fs::rename(tmp, pf.dest);
            placed_fonts.back().placed = true;
        }
        if (!planned_fonts.empty()) {
            std::cout << "==> Installed " << planned_fonts.size()
                      << (planned_fonts.size() == 1 ? " font" : " fonts") << " in "
                      << fontdir.string() << "\n";
        }
        receipt.quarantine_cleared = true;
        maybe_inject_fault("receipt");
        write_receipt(version_dir, receipt);
        record_settings_cask(config, token, pkg.version);
    } catch (...) {
        rollback();
        throw;
    }

    // ---- Commit: new version is live; retire the old one. ----
    fs::remove_all(backup_dir, ec);
    fs::remove_all(font_backup_dir, ec);
    fs::remove_all(app_staging, ec);
    if (old_receipt) {
        for (const auto& font : old_receipt->fonts) {
            const bool still_used =
                std::find(receipt.fonts.begin(), receipt.fonts.end(), font) != receipt.fonts.end();
            if (!still_used && !fs::is_symlink(font, ec) && fs::exists(font, ec) &&
                font_owned_by_den(config, font)) {
                fs::remove_all(font, ec);
            }
        }
        for (const auto& app : old_receipt->apps) {
            const bool still_used =
                std::find(receipt.apps.begin(), receipt.apps.end(), app) != receipt.apps.end();
            if (!still_used && fs::exists(app) && app_owned_by_den(config, app)) {
                fs::remove_all(app, ec);
            }
        }
        for (const auto& b : old_receipt->binaries) {
            const bool still_used =
                std::any_of(receipt.binaries.begin(), receipt.binaries.end(),
                            [&](const CaskBinaryLink& n) { return n.link == b.link; });
            if (!still_used && fs::is_symlink(b.link, ec)) {
                fs::remove(b.link, ec);
            }
        }
        fs::remove_all(cask_version_path(config, token, old_version), ec);
    }

    InstallResult result;
    result.resolved_version = pkg.version;
    return result;
#endif
}

void uninstall_cask(const Config& config, std::string_view token_sv) {
#ifndef __APPLE__
    throw UserError("casks are not supported on Linux (macOS only)");
#else
    std::string token(token_sv);
    if (token.starts_with("cask/")) {
        token = token.substr(5);
    }

    const std::string version = installed_cask_version(config, token);
    if (version.empty()) {
        throw UserError("cask '" + token + "' is not installed by den");
    }

    const auto version_dir = cask_version_path(config, token, version);
    auto receipt = read_cask_receipt(version_dir);
    if (!receipt) {
        throw UserError("cask '" + token +
                        "' has no den receipt — refusing to uninstall "
                        "(not installed by den, or receipt missing)");
    }

    const StanzaOptions sopts = default_stanza_options(cask_appdir(config));
    if (!receipt->uninstall.empty()) {
        emit_stanza_report(
            config, token, "uninstall",
            run_uninstall_stanza(receipt->uninstall, StanzaPhase::PreRemove, cask_system(), sopts));
    }

    std::error_code ec;
    for (const auto& app : receipt->apps) {
        if (!fs::exists(app)) {
            continue;
        }
        if (!app_owned_by_den(config, app)) {
            std::cerr << "warning: leaving '" << app.string()
                      << "' in place (ownership check failed)\n";
            continue;
        }
        std::cout << "==> Removing " << app.string() << "\n";
        fs::remove_all(app, ec);
        if (ec) {
            throw Error("failed to remove " + app.string() + ": " + ec.message());
        }
    }

    for (const auto& b : receipt->binaries) {
        if (fs::is_symlink(b.link, ec) || fs::exists(b.link, ec)) {
            // Only remove the link if it still points at our target (or is gone).
            if (fs::is_symlink(b.link, ec)) {
                auto tgt = fs::read_symlink(b.link, ec);
                auto resolved = fs::weakly_canonical(b.link.parent_path() / tgt, ec);
                auto want = fs::weakly_canonical(b.target, ec);
                if (!ec && resolved == want) {
                    fs::remove(b.link, ec);
                } else {
                    // Stale or retargeted — remove the link name only if it is a symlink we made.
                    fs::remove(b.link, ec);
                }
            } else {
                // Refuse to delete a regular file at the link path.
                std::cerr << "warning: leaving '" << b.link.string() << "' (not a den symlink)\n";
            }
        }
    }

    for (const auto& font : receipt->fonts) {
        if (fs::is_symlink(font, ec)) {
            std::cerr << "warning: leaving '" << font.string() << "' (not a den font)\n";
            continue;
        }
        if (!fs::exists(font, ec)) {
            continue;
        }
        fs::remove_all(font, ec);
        if (ec) {
            throw Error("failed to remove " + font.string() + ": " + ec.message());
        }
    }
    if (!receipt->fonts.empty()) {
        std::cout << "==> Removed " << receipt->fonts.size()
                  << (receipt->fonts.size() == 1 ? " font" : " fonts") << "\n";
    }

    if (!receipt->uninstall.empty()) {
        emit_stanza_report(config, token, "uninstall",
                           run_uninstall_stanza(receipt->uninstall, StanzaPhase::PostRemove,
                                                cask_system(), sopts));
    }

    if (receipt->uninstall.pkgutil.empty()) {
        for (const auto& pkg_id : receipt->pkgs) {
            std::cerr << "note: pkg artifact '" << pkg_id
                      << "' was installed via installer and the cask declares no "
                         "`uninstall pkgutil:`; system files may remain\n";
        }
    }

    fs::remove_all(version_dir, ec);
    // Drop empty token dir.
    const auto token_dir = version_dir.parent_path();
    if (fs::is_directory(token_dir, ec) && fs::is_empty(token_dir, ec)) {
        fs::remove(token_dir, ec);
    }
    erase_settings_cask(config, token);
#endif
}

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "cask.h"

#include "../core/error.h"
#include "../download/archive.h"
#include "../download/fetch.h"
#include "../download/sha256.h"
#include "../env/environment.h"
#include "../env/manifest.h"
#include "../platform/platform.h"
#include "../settings/settings.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
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
        const auto name = archive.filename().string().empty() ? "binary" : archive.filename().string();
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
    const std::string cmd =
        "installer -pkg " + sh_quote(pkg_file) + " -target " + sh_quote(target);
    std::cout << "==> Running installer for " << pkg_file.filename().string() << "\n";
    return run_cmd(cmd) == 0;
#else
    (void)pkg_file;
    return false;
#endif
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

bool app_owned_by_den(const Config& config, const fs::path& app_path) {
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
            for (const auto& a : receipt->apps) {
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
    std::sort(out.begin(), out.end(), [](const InstalledPackage& a, const InstalledPackage& b) {
        return a.name < b.name;
    });
    return out;
}

InstallResult install_cask(const Config& config, const Package& pkg) {
#ifndef __APPLE__
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

    if (is_cask_installed(config, token)) {
        const auto ver = installed_cask_version(config, token);
        if (ver == pkg.version) {
            SPDLOG_INFO("cask {} {} already installed", token, ver);
            InstallResult r;
            r.resolved_version = pkg.version;
            return r;
        }
        // Different version — uninstall the old one first (receipt-scoped).
        uninstall_cask(config, token);
    }

    const auto version_dir = cask_version_path(config, token, pkg.version);
    std::error_code ec;
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

    std::cout << "==> Staging " << token << "\n";
    fs::path stage;
    try {
        stage = stage_archive(archive_path, version_dir);
    } catch (...) {
        fs::remove_all(version_dir, ec);
        throw;
    }

    const fs::path appdir = cask_appdir(config);
    fs::create_directories(appdir);

    CaskReceipt receipt;
    receipt.token = token;
    receipt.version = pkg.version;

    // Determine artifacts. If the index carried none, fall back to "any .app
    // in the stage" so simple casks still install after a partial index.
    std::vector<CaskArtifact> artifacts = pkg.cask_artifacts;
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

    for (const auto& art : artifacts) {
        if (art.kind == CaskArtifactKind::App) {
            const fs::path src = find_app_in_stage(stage, art.source);
            if (!fs::exists(src)) {
                throw UserError("cask '" + token + "': app not found in archive: " + art.source);
            }
            const std::string app_name =
                art.target.empty() ? src.filename().string() : fs::path(art.target).filename().string();
            // Refuse path traversal in app names.
            if (app_name.find('/') != std::string::npos || app_name == ".." ||
                app_name.find("..") != std::string::npos) {
                throw UserError("cask '" + token + "': unsafe app name: " + app_name);
            }
            const fs::path dest = appdir / app_name;
            if (fs::exists(dest)) {
                if (!app_owned_by_den(config, dest)) {
                    // Roll back stage; do not touch the foreign app.
                    fs::remove_all(version_dir, ec);
                    throw UserError("refusing to overwrite '" + dest.string() +
                                    "': not installed by den. "
                                    "Remove it manually or choose a different DEN_APPDIR.");
                }
                // Owned by den (prior version) — safe to replace.
                fs::remove_all(dest, ec);
            }
            std::cout << "==> Installing " << app_name << " to " << appdir.string() << "\n";
            copy_app_bundle(src, dest);
            clear_quarantine(dest);
            receipt.apps.push_back(dest);
            placed_anything = true;
        } else if (art.kind == CaskArtifactKind::Binary) {
            const fs::path src = resolve_binary_source(stage, appdir, art.source);
            if (!fs::exists(src)) {
                // Binary may live inside an app we just installed; retry.
                throw UserError("cask '" + token + "': binary not found: " + art.source);
            }
            const std::string link_name = binary_link_name(art);
            if (link_name.empty() || link_name.find('/') != std::string::npos) {
                throw UserError("cask '" + token + "': unsafe binary name");
            }
            // Keep a durable copy under the stage so env rebuilds can relink.
            fs::path staged_bin = stage / link_name;
            if (fs::weakly_canonical(src) != fs::weakly_canonical(staged_bin, ec)) {
                if (!fs::exists(staged_bin)) {
                    // If source is inside stage already, just reference it.
                    if (src.string().find(stage.string()) == 0) {
                        staged_bin = src;
                    } else {
                        fs::copy_file(src, staged_bin, fs::copy_options::overwrite_existing);
                    }
                }
            } else {
                staged_bin = src;
            }
            // Make executable.
            fs::permissions(staged_bin,
                            fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            fs::perm_options::add, ec);

            const auto active = active_env_path(config.den_home);
            const fs::path bin_dir = env_dir(config.den_home, active) / "bin";
            fs::create_directories(bin_dir);
            const fs::path link = bin_dir / link_name;
            if (fs::exists(link) || fs::is_symlink(link)) {
                // Only replace if it points into our caskroom or doesn't exist as foreign file.
                std::error_code ec2;
                if (fs::is_symlink(link, ec2)) {
                    auto tgt = fs::read_symlink(link, ec2);
                    auto canon = fs::weakly_canonical(link.parent_path() / tgt, ec2);
                    if (!ec2 && canon.string().find(den_caskroom(config).string()) != 0 &&
                        fs::exists(link)) {
                        // Could be a den env link from a formula — still our env, replace.
                    }
                    fs::remove(link, ec2);
                } else if (fs::exists(link)) {
                    fs::remove_all(version_dir, ec);
                    throw UserError("refusing to overwrite binary '" + link.string() + "'");
                }
            }
            fs::create_symlink(staged_bin, link, ec);
            if (ec) {
                fs::remove_all(version_dir, ec);
                throw Error("failed to link binary " + link.string() + ": " + ec.message());
            }
            clear_quarantine(staged_bin);
            receipt.binaries.push_back(CaskBinaryLink{link, staged_bin});
            placed_anything = true;
            std::cout << "==> Linked " << link_name << "\n";
        } else if (art.kind == CaskArtifactKind::Pkg) {
            fs::path pkg_file = stage / art.source;
            if (!fs::exists(pkg_file)) {
                // Search by basename (nested containers).
                pkg_file = find_app_in_stage(stage, art.source);
            }
            if (!fs::exists(pkg_file)) {
                // Content-addressed downloads keep the sha256 as the filename
                // (e.g. <hash>.pkg). Accept any single .pkg under stage.
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
                    // Prefer a name that matches the artifact source basename.
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
            if (!install_pkg_file(pkg_file)) {
                fs::remove_all(version_dir, ec);
                throw UserError(
                    "cask '" + token +
                    "': pkg install failed (is admin authorisation available?). "
                    "PKG casks may require an interactive sudo via /usr/bin/installer.");
            }
            // Record any pkgutil id from uninstall stanzas if present in source name.
            receipt.pkgs.push_back(art.source);
            placed_anything = true;
        }
    }

    if (!placed_anything) {
        fs::remove_all(version_dir, ec);
        throw UserError(
            "cask '" + token +
            "' has no supported artifacts (need app, binary, or pkg). "
            "Complex casks (installer scripts, preflight) are not supported yet.");
    }

    receipt.quarantine_cleared = true;
    write_receipt(version_dir, receipt);
    record_settings_cask(config, token, pkg.version);

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
                std::cerr << "warning: leaving '" << b.link.string()
                          << "' (not a den symlink)\n";
            }
        }
    }

    for (const auto& pkg_id : receipt->pkgs) {
        // Best-effort note: full pkgutil --forget needs sudo and is a follow-up.
        std::cerr << "note: pkg artifact '" << pkg_id
                  << "' was installed via installer; "
                     "system files may remain (pkgutil --forget not run)\n";
        (void)pkg_id;
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

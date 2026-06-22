// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "node_provider.h"

#include "../core/config.h"
#include "../core/error.h"
#include "exec.h"

#include <cctype>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <fstream>
#include <string>

namespace den {

namespace {

constexpr std::string_view kProviderName = "npm";

fs::path provider_root(const Config& config) {
    return config.den_home / "providers" / "npm";
}

fs::path pkg_versioned_root(const Config& config, std::string_view name, std::string_view version) {
    return provider_root(config) / std::string(name) / std::string(version);
}

// npm package names: lowercase, may be scoped (@scope/name), and may contain
// digits, hyphens, dots, underscores. We accept the user-typed form and reject
// path traversal / leading dots.
bool valid_npm_name(std::string_view name) {
    if (name.empty() || name.front() == '.') {
        return false;
    }
    if (name.find("..") != std::string_view::npos) {
        return false;
    }
    for (char c : name) {
        bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' ||
                  c == '@' || c == '/';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// Read the installed version from <root>/node_modules/<name>/package.json.
std::string read_installed_version(const fs::path& root, std::string_view name) {
    auto pkg_json = root / "node_modules" / std::string(name) / "package.json";
    std::ifstream in(pkg_json);
    if (!in) {
        return {};
    }
    try {
        nlohmann::json j;
        in >> j;
        if (j.contains("version") && j["version"].is_string()) {
            return j["version"].get<std::string>();
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("npm provider: failed to parse {}: {}", pkg_json.string(), e.what());
    }
    return {};
}

// Build <root>/bin/ with absolute symlinks into <root>/node_modules/.bin/.
// npm's .bin entries are relative symlinks into sibling package dirs; an
// absolute hop through our own bin/ preserves their resolution context when
// the env materialiser links them onward.
void materialise_bin(const fs::path& root) {
    auto npm_bin = root / "node_modules" / ".bin";
    std::error_code ec;
    if (!fs::is_directory(npm_bin, ec)) {
        return;
    }
    auto bin = root / "bin";
    fs::create_directories(bin, ec);
    for (const auto& entry : fs::directory_iterator(npm_bin, ec)) {
        auto link = bin / entry.path().filename();
        fs::remove(link, ec);
        fs::create_symlink(fs::absolute(entry.path()), link, ec);
        if (ec) {
            SPDLOG_WARN("npm provider: failed to link {}: {}", link.string(), ec.message());
            ec.clear();
        }
    }
}

} // namespace

NodeProvider::NodeProvider() = default;

std::string_view NodeProvider::provider_name() const {
    return kProviderName;
}

bool NodeProvider::accepts(std::string_view /*name*/) const {
    // Never auto-claim — only `--provider npm` or a manifest hint selects npm.
    return false;
}

InstallResult NodeProvider::install(const Config& config, std::string_view name,
                                    std::string_view version_hint) {
    if (!valid_npm_name(name)) {
        throw UserError("npm provider: invalid package name '" + std::string(name) + "'");
    }
    if (!tool_available("npm")) {
        throw UserError("npm provider: `npm` not found on PATH. Install Node.js to install "
                        "npm packages, or use a different provider.");
    }

    auto staging = provider_root(config) / std::string(name) / ".staging";
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging);

    std::string spec(name);
    if (!version_hint.empty()) {
        spec += "@";
        spec += version_hint;
    }

    // --prefix installs into staging/node_modules; --no-save avoids touching a
    // package.json; --no-fund/--no-audit keep output terse and offline-ish.
    auto inst = run_tool({"npm", "install", "--prefix", staging.string(), "--no-save", "--no-fund",
                          "--no-audit", spec});
    if (inst.exit_code != 0) {
        fs::remove_all(staging, ec);
        throw UserError("npm provider: failed to install '" + std::string(name) + "':\n" +
                        inst.output);
    }

    std::string resolved = read_installed_version(staging, name);
    if (resolved.empty()) {
        fs::remove_all(staging, ec);
        throw UserError("npm provider: installed '" + std::string(name) +
                        "' but could not determine its version");
    }

    auto dest = pkg_versioned_root(config, name, resolved);
    fs::remove_all(dest, ec);
    fs::create_directories(dest.parent_path());
    fs::rename(staging, dest, ec);
    if (ec) {
        fs::remove_all(staging, ec);
        throw InternalError("npm provider: failed to move install into place: " + ec.message());
    }

    // Build <dest>/bin after the move so the absolute symlinks point at the
    // final location, not the staging path.
    materialise_bin(dest);

    InstallResult result;
    result.resolved_version = resolved;
    return result;
}

void NodeProvider::uninstall(const Config& config, std::string_view name) {
    auto pkg_root = provider_root(config) / std::string(name);
    std::error_code ec;
    fs::remove_all(pkg_root, ec);
}

std::vector<InstalledPackage> NodeProvider::list_installed(const Config& config) const {
    std::vector<InstalledPackage> result;
    auto root = provider_root(config);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return result;
    }
    for (const auto& name_entry : fs::directory_iterator(root, ec)) {
        if (!name_entry.is_directory(ec)) {
            continue;
        }
        for (const auto& ver_entry : fs::directory_iterator(name_entry.path(), ec)) {
            if (!ver_entry.is_directory(ec)) {
                continue;
            }
            auto version = ver_entry.path().filename().string();
            if (version.starts_with('.')) {
                continue;
            }
            result.push_back({name_entry.path().filename().string(), version, ver_entry.path()});
        }
    }
    return result;
}

fs::path NodeProvider::package_root(const Config& config, std::string_view name,
                                    std::string_view version) const {
    return pkg_versioned_root(config, name, version);
}

std::vector<fs::path> NodeProvider::binary_paths(const Config& /*config*/,
                                                 const InstalledPackage& pkg) const {
    std::vector<fs::path> paths;
    auto bin = pkg.path / "bin";
    if (fs::is_directory(bin)) {
        paths.push_back(bin);
    }
    return paths;
}

} // namespace den

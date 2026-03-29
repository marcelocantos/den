// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "migrate.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <unordered_set>

namespace den {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Return true if path looks like a version directory (starts with a digit).
bool looks_like_version(const std::string& name) {
    return !name.empty() && (std::isdigit(static_cast<unsigned char>(name[0]))
                             || name[0] == 'v');
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::vector<HomebrewKeg> scan_homebrew_cellar(const fs::path& cellar) {
    std::vector<HomebrewKeg> kegs;

    if (!fs::is_directory(cellar)) {
        SPDLOG_DEBUG("Cellar not found or not a directory: {}", cellar.string());
        return kegs;
    }

    // Each subdirectory of cellar is a formula name.
    for (const auto& formula_entry : fs::directory_iterator(cellar)) {
        if (!formula_entry.is_directory()) {
            continue;
        }
        const std::string name = formula_entry.path().filename().string();

        // Each subdirectory of the formula directory is a version.
        for (const auto& version_entry :
             fs::directory_iterator(formula_entry.path())) {
            if (!version_entry.is_directory()) {
                continue;
            }
            const std::string version =
                version_entry.path().filename().string();
            if (!looks_like_version(version)) {
                continue;
            }
            kegs.push_back({name, version, version_entry.path()});
        }
    }

    // Stable sort: by name, then version descending (newest first).
    std::sort(kegs.begin(), kegs.end(), [](const HomebrewKeg& a,
                                           const HomebrewKeg& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.version > b.version; // newest first within same name
    });

    return kegs;
}

std::optional<Tab> read_tab(const fs::path& keg_path) {
    const fs::path receipt = keg_path / "INSTALL_RECEIPT.json";
    if (!fs::is_regular_file(receipt)) {
        return std::nullopt;
    }

    std::ifstream f(receipt);
    if (!f) {
        return std::nullopt;
    }

    try {
        const auto j = nlohmann::json::parse(f);

        Tab tab;
        tab.installed_on_request =
            j.value("installed_on_request", true);

        if (j.contains("runtime_dependencies")
            && j["runtime_dependencies"].is_array()) {
            for (const auto& dep : j["runtime_dependencies"]) {
                if (dep.contains("full_name")
                    && dep["full_name"].is_string()) {
                    tab.runtime_deps.push_back(dep["full_name"].get<std::string>());
                }
            }
        }

        return tab;
    } catch (const nlohmann::json::exception& e) {
        SPDLOG_WARN("Failed to parse INSTALL_RECEIPT.json at {}: {}",
                    receipt.string(), e.what());
        return std::nullopt;
    }
}

void migrate_from_homebrew(const Config& config,
                           const std::vector<std::string>& names)
{
    SPDLOG_INFO("Scanning Homebrew Cellar at {}",
                config.homebrew_cellar.string());

    const auto filter = std::unordered_set<std::string>(names.begin(),
                                                         names.end());
    const bool filter_all = filter.empty();

    auto kegs = scan_homebrew_cellar(config.homebrew_cellar);

    // Filter to requested names if provided.
    if (!filter_all) {
        kegs.erase(
            std::remove_if(kegs.begin(), kegs.end(),
                           [&](const HomebrewKeg& k) {
                               return filter.find(k.name) == filter.end();
                           }),
            kegs.end());
    }

    const int found = static_cast<int>(kegs.size());
    SPDLOG_INFO("Found {} kegs in Cellar", found);

    // Load den's root manifest (JSON at den_home/manifests/ROOT.json).
    // We use a simple flat JSON structure: { "packages": { "name": "version" } }
    const fs::path manifest_dir = config.den_home / "manifests";
    const fs::path manifest_path = manifest_dir / "ROOT.json";

    nlohmann::json manifest;
    if (fs::is_regular_file(manifest_path)) {
        std::ifstream f(manifest_path);
        if (f) {
            try {
                manifest = nlohmann::json::parse(f);
            } catch (const nlohmann::json::exception& e) {
                SPDLOG_WARN("Could not parse existing root manifest: {}",
                            e.what());
            }
        }
    }

    if (!manifest.contains("packages") || !manifest["packages"].is_object()) {
        manifest["packages"] = nlohmann::json::object();
    }
    if (!manifest.contains("auto") || !manifest["auto"].is_array()) {
        manifest["auto"] = nlohmann::json::array();
    }

    auto& pkgs = manifest["packages"];
    auto& auto_pkgs = manifest["auto"];

    // Build a set of already-auto packages for O(1) lookup.
    std::set<std::string> auto_set;
    for (const auto& a : auto_pkgs) {
        if (a.is_string()) {
            auto_set.insert(a.get<std::string>());
        }
    }

    int added = 0;
    int skipped = 0;
    std::set<std::string> seen;

    // Kegs are sorted newest-first per name; take the first occurrence.
    for (const auto& keg : kegs) {
        if (seen.count(keg.name)) {
            continue; // already processed a newer version of this formula
        }
        seen.insert(keg.name);

        if (pkgs.contains(keg.name)) {
            SPDLOG_DEBUG("Skipping {} — already tracked", keg.name);
            ++skipped;
            continue;
        }

        const auto tab = read_tab(keg.path);
        const bool on_request = tab ? tab->installed_on_request : true;

        pkgs[keg.name] = keg.version;
        if (!on_request && auto_set.find(keg.name) == auto_set.end()) {
            auto_pkgs.push_back(keg.name);
            auto_set.insert(keg.name);
        }
        ++added;
        SPDLOG_DEBUG("Added {}: {}{}", keg.name, keg.version,
                     on_request ? "" : " [auto]");
    }

    // Write manifest back.
    if (added > 0) {
        fs::create_directories(manifest_dir);
        std::ofstream out(manifest_path);
        if (!out) {
            SPDLOG_ERROR("Failed to write root manifest to {}",
                         manifest_path.string());
        } else {
            out << manifest.dump(2) << "\n";
            SPDLOG_DEBUG("Wrote manifest to {}", manifest_path.string());
        }
    }

    SPDLOG_INFO("Migration complete: {} packages found, {} added, {} skipped",
                found, added, skipped);
    std::printf("==> Migration complete: %d found, %d added, %d skipped\n",
                found, added, skipped);
}

} // namespace den

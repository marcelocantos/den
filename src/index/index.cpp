// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "index.h"

#include "../download/http.h"

#include "../core/error.h"

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include <fstream>

namespace den {

using json = nlohmann::json;

namespace {

constexpr const char* kFormulaUrl = "https://formulae.brew.sh/api/formula.json";
constexpr const char* kCaskUrl = "https://formulae.brew.sh/api/cask.json";

/// Map Homebrew's cellar string to our RelocationType.
RelocationType parse_relocation(const std::string& cellar) {
    if (cellar == ":any_skip_relocation") {
        return RelocationType::Portable;
    }
    if (cellar == ":any") {
        return RelocationType::NeedsSubstitution;
    }
    // Anything else (an actual path) means a hardcoded prefix.
    return RelocationType::HardcodedPrefix;
}

/// Serialize a RelocationType to a string for JSON persistence.
std::string relocation_to_string(RelocationType r) {
    switch (r) {
    case RelocationType::Portable:
        return "portable";
    case RelocationType::NeedsSubstitution:
        return "needs_substitution";
    case RelocationType::HardcodedPrefix:
        return "hardcoded_prefix";
    }
    return "portable";
}

/// Deserialize a RelocationType from a JSON string.
RelocationType relocation_from_string(const std::string& s) {
    if (s == "needs_substitution")
        return RelocationType::NeedsSubstitution;
    if (s == "hardcoded_prefix")
        return RelocationType::HardcodedPrefix;
    return RelocationType::Portable;
}

/// Safely extract a string from a JSON value, returning fallback for null/missing/wrong type.
std::string json_string(const json& j, const char* key, const std::string& fallback = "") {
    if (j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return fallback;
}

/// Transform a single Homebrew formula JSON object into a Package.
Package transform_formula(const json& f) {
    Package pkg;
    pkg.name = json_string(f, "name");
    pkg.description = json_string(f, "desc");
    pkg.homepage = json_string(f, "homepage");
    pkg.license = json_string(f, "license");
    pkg.artifact_type = ArtifactType::Binary;
    pkg.deprecated =
        f.contains("deprecated") && f["deprecated"].is_boolean() && f["deprecated"].get<bool>();
    pkg.disabled =
        f.contains("disabled") && f["disabled"].is_boolean() && f["disabled"].get<bool>();

    // Stability rating (🎯T30). Prefer an explicit rating if the upstream
    // metadata carries one; otherwise derive a conservative default from the
    // deprecated/disabled flags. A live, non-deprecated formula on Homebrew's
    // stable channel is Stable.
    if (f.contains("stability") && f["stability"].is_string()) {
        pkg.stability = sat::parse_stability(f["stability"].get<std::string>());
    } else if (pkg.disabled) {
        pkg.stability = Stability::Insecure;
    } else if (pkg.deprecated) {
        pkg.stability = Stability::Buggy;
    } else {
        pkg.stability = Stability::Stable;
    }

    // Keg-only status.
    pkg.keg_only =
        f.contains("keg_only") && f["keg_only"].is_boolean() && f["keg_only"].get<bool>();
    if (f.contains("keg_only_reason") && f["keg_only_reason"].is_object()) {
        pkg.keg_only_reason = json_string(f["keg_only_reason"], "reason");
    }

    // Conflict declarations.
    if (f.contains("conflicts_with") && f["conflicts_with"].is_array()) {
        for (const auto& c : f["conflicts_with"]) {
            if (c.is_string()) {
                pkg.conflicts_with.push_back(c.get<std::string>());
            }
        }
    }

    // Version: prefer versions.stable.
    if (f.contains("versions") && f["versions"].contains("stable")) {
        pkg.version = f["versions"]["stable"].get<std::string>();
    }

    // Dependencies.
    if (f.contains("dependencies") && f["dependencies"].is_array()) {
        for (const auto& dep : f["dependencies"]) {
            pkg.dependencies.push_back(dep.get<std::string>());
        }
    }
    if (f.contains("build_dependencies") && f["build_dependencies"].is_array()) {
        for (const auto& dep : f["build_dependencies"]) {
            pkg.build_dependencies.push_back(dep.get<std::string>());
        }
    }

    // Bottle archives — extract per-platform info.
    if (f.contains("bottle") && f["bottle"].contains("stable") &&
        f["bottle"]["stable"].contains("files")) {
        const auto& files = f["bottle"]["stable"]["files"];
        std::string cellar_default;
        if (f["bottle"]["stable"].contains("cellar")) {
            auto& c = f["bottle"]["stable"]["cellar"];
            if (c.is_string()) {
                cellar_default = c.get<std::string>();
            }
        }

        for (auto it = files.begin(); it != files.end(); ++it) {
            Package::Archive ar;
            ar.url = it.value().value("url", "");
            ar.sha256 = it.value().value("sha256", "");

            std::string cellar_str = cellar_default;
            if (it.value().contains("cellar")) {
                auto& c = it.value()["cellar"];
                if (c.is_string()) {
                    cellar_str = c.get<std::string>();
                }
            }
            ar.relocation = parse_relocation(cellar_str);

            pkg.archives[it.key()] = std::move(ar);
        }
    }

    // Ruby source path (for fetching formula without brew cat).
    if (f.contains("ruby_source_path") && f["ruby_source_path"].is_string()) {
        pkg.ruby_source_path = f["ruby_source_path"].get<std::string>();
    }

    // Source URL.
    if (f.contains("urls") && f["urls"].contains("stable") && f["urls"]["stable"].contains("url")) {
        pkg.source_url = f["urls"]["stable"]["url"].get<std::string>();
        if (f["urls"]["stable"].contains("checksum")) {
            auto& cs = f["urls"]["stable"]["checksum"];
            if (cs.is_string()) {
                pkg.source_sha256 = cs.get<std::string>();
            }
        }
    }

    return pkg;
}

/// Transform a single Homebrew cask JSON object into a Package.
/// Transform a single Homebrew cask JSON object into a Package.
Package transform_cask(const json& c) {
    Package pkg;
    pkg.name = json_string(c, "token");
    pkg.description = json_string(c, "desc");
    pkg.homepage = json_string(c, "homepage");
    pkg.artifact_type = ArtifactType::App;
    pkg.deprecated =
        c.contains("deprecated") && c["deprecated"].is_boolean() && c["deprecated"].get<bool>();
    pkg.disabled =
        c.contains("disabled") && c["disabled"].is_boolean() && c["disabled"].get<bool>();

    if (c.contains("version") && c["version"].is_string()) {
        pkg.version = c["version"].get<std::string>();
    }

    // Cask URL — direct download URL for the .dmg/.zip/.pkg.
    if (c.contains("url") && c["url"].is_string()) {
        Package::Archive ar;
        ar.url = c["url"].get<std::string>();
        if (c.contains("sha256") && c["sha256"].is_string()) {
            ar.sha256 = c["sha256"].get<std::string>();
        }
        ar.relocation = RelocationType::Portable;
        pkg.archives["default"] = std::move(ar);
    }

    return pkg;
}

/// Serialize a Package to JSON.
json package_to_json(const Package& pkg) {
    json j;
    j["name"] = pkg.name;
    j["version"] = pkg.version;
    j["description"] = pkg.description;
    j["homepage"] = pkg.homepage;
    j["license"] = pkg.license;
    j["artifact_type"] = pkg.artifact_type == ArtifactType::App ? "app" : "binary";
    j["stability"] = sat::stability_label(pkg.stability);
    j["deprecated"] = pkg.deprecated;
    j["disabled"] = pkg.disabled;

    j["dependencies"] = pkg.dependencies;
    j["build_dependencies"] = pkg.build_dependencies;

    json archives_json = json::object();
    for (const auto& [platform, ar] : pkg.archives) {
        json aj;
        aj["url"] = ar.url;
        aj["sha256"] = ar.sha256;
        aj["relocation"] = relocation_to_string(ar.relocation);
        archives_json[platform] = std::move(aj);
    }
    j["archives"] = std::move(archives_json);

    if (pkg.source_url)
        j["source_url"] = *pkg.source_url;
    if (pkg.source_sha256)
        j["source_sha256"] = *pkg.source_sha256;
    if (pkg.ruby_source_path)
        j["ruby_source_path"] = *pkg.ruby_source_path;

    return j;
}

/// Deserialize a Package from JSON.
Package package_from_json(const json& j) {
    Package pkg;
    pkg.name = j.value("name", "");
    pkg.version = j.value("version", "");
    pkg.description = j.value("description", "");
    pkg.homepage = j.value("homepage", "");
    pkg.license = j.value("license", "");

    std::string at = j.value("artifact_type", "binary");
    pkg.artifact_type = at == "app" ? ArtifactType::App : ArtifactType::Binary;

    pkg.stability = sat::parse_stability(j.value("stability", "stable"));
    pkg.deprecated = j.value("deprecated", false);
    pkg.disabled = j.value("disabled", false);

    if (j.contains("dependencies") && j["dependencies"].is_array()) {
        for (const auto& d : j["dependencies"]) {
            pkg.dependencies.push_back(d.get<std::string>());
        }
    }
    if (j.contains("build_dependencies") && j["build_dependencies"].is_array()) {
        for (const auto& d : j["build_dependencies"]) {
            pkg.build_dependencies.push_back(d.get<std::string>());
        }
    }

    if (j.contains("archives") && j["archives"].is_object()) {
        for (auto it = j["archives"].begin(); it != j["archives"].end(); ++it) {
            Package::Archive ar;
            ar.url = it.value().value("url", "");
            ar.sha256 = it.value().value("sha256", "");
            ar.relocation = relocation_from_string(it.value().value("relocation", "portable"));
            pkg.archives[it.key()] = std::move(ar);
        }
    }

    if (j.contains("source_url") && j["source_url"].is_string()) {
        pkg.source_url = j["source_url"].get<std::string>();
    }
    if (j.contains("source_sha256") && j["source_sha256"].is_string()) {
        pkg.source_sha256 = j["source_sha256"].get<std::string>();
    }
    if (j.contains("ruby_source_path") && j["ruby_source_path"].is_string()) {
        pkg.ruby_source_path = j["ruby_source_path"].get<std::string>();
    }

    return pkg;
}

} // namespace

PackageIndex load_index(const fs::path& cache_path) {
    PackageIndex idx;

    if (!fs::exists(cache_path)) {
        SPDLOG_INFO("no cached index at {}", cache_path.string());
        return idx;
    }

    std::ifstream file(cache_path);
    if (!file) {
        SPDLOG_WARN("cannot open index cache: {}", cache_path.string());
        return idx;
    }

    try {
        json j = json::parse(file);
        if (j.contains("packages") && j["packages"].is_array()) {
            for (const auto& pj : j["packages"]) {
                Package pkg = package_from_json(pj);
                std::string name = pkg.name;
                idx.packages.emplace(std::move(name), std::move(pkg));
            }
        }
    } catch (const json::exception& e) {
        SPDLOG_WARN("failed to parse index cache: {}", e.what());
        return PackageIndex{};
    }

    SPDLOG_INFO("loaded {} packages from cache", idx.packages.size());
    return idx;
}

void save_index(const PackageIndex& idx, const fs::path& cache_path) {
    fs::create_directories(cache_path.parent_path());

    json j;
    json packages_array = json::array();
    for (const auto& [name, pkg] : idx.packages) {
        packages_array.push_back(package_to_json(pkg));
    }
    j["packages"] = std::move(packages_array);

    // Atomic write: write to .tmp, then rename.
    fs::path tmp = cache_path;
    tmp += ".tmp";

    {
        std::ofstream out(tmp);
        if (!out) {
            throw Error("cannot write index cache: " + tmp.string());
        }
        out << j.dump();
        if (!out) {
            throw Error("write failed for index cache: " + tmp.string());
        }
    }

    fs::rename(tmp, cache_path);
    SPDLOG_INFO("saved {} packages to cache", idx.packages.size());
}

PackageIndex fetch_and_transform(const Config& /*config*/) {
    PackageIndex idx;

    // Fetch formulae.
    SPDLOG_INFO("fetching Homebrew formula index");
    std::string formula_data = fetch_url(kFormulaUrl);

    try {
        json formulae = json::parse(formula_data);
        if (formulae.is_array()) {
            for (const auto& f : formulae) {
                Package pkg = transform_formula(f);
                std::string name = pkg.name;
                idx.packages.emplace(std::move(name), std::move(pkg));
            }
        }
    } catch (const json::exception& e) {
        throw DownloadError(std::string("failed to parse formula.json: ") + e.what());
    }

    SPDLOG_INFO("indexed {} formulae", idx.packages.size());

    // Fetch casks.
    SPDLOG_INFO("fetching Homebrew cask index");
    std::string cask_data = fetch_url(kCaskUrl);

    size_t formula_count = idx.packages.size();

    try {
        json casks = json::parse(cask_data);
        if (casks.is_array()) {
            for (const auto& c : casks) {
                Package pkg = transform_cask(c);
                std::string name = pkg.name;
                // Casks might collide with formula names; cask gets a
                // "cask/" prefix if there's a conflict.
                if (idx.packages.contains(name)) {
                    name = "cask/" + name;
                    pkg.name = name;
                }
                idx.packages.emplace(std::move(name), std::move(pkg));
            }
        }
    } catch (const json::exception& e) {
        throw DownloadError(std::string("failed to parse cask.json: ") + e.what());
    }

    SPDLOG_INFO("indexed {} casks ({} total packages)", idx.packages.size() - formula_count,
                idx.packages.size());

    return idx;
}

} // namespace den

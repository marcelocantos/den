// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "index.h"
#include "cask_uninstall_json.h"

#include "index_cache.h"

#include "../download/http.h"

#include "../core/error.h"
#include "../platform/platform.h"

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <utility>
#include <vector>

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

    // Version: prefer versions.stable, then append Homebrew's revision to
    // form pkg_version ("1.25.0" revision 1 -> "1.25.0_1"). Bottles are
    // built into, and unpack as, Cellar/<name>/<pkg_version>, and every
    // dependent bottle references that directory by name. The bottle
    // rebuild number does not affect the directory and is ignored here.
    if (f.contains("versions") && f["versions"].contains("stable")) {
        pkg.version = f["versions"]["stable"].get<std::string>();
    }
    if (f.contains("revision") && f["revision"].is_number_integer()) {
        pkg.revision = std::max(0, f["revision"].get<int>());
    }
    if (pkg.revision > 0 && !pkg.version.empty()) {
        pkg.version += "_" + std::to_string(pkg.revision);
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

/// Parse cask artifacts of kinds den can install (app, binary, pkg).
/// Unsupported kinds (installer scripts, preflight, zap-only, …) are ignored;
/// install_cask will error if nothing supported remains.
void parse_cask_artifacts(const json& arts, std::vector<CaskArtifact>& out) {
    if (!arts.is_array()) {
        return;
    }
    for (const auto& a : arts) {
        if (!a.is_object()) {
            continue;
        }
        if (a.contains("app")) {
            CaskArtifact art;
            art.kind = CaskArtifactKind::App;
            const auto& app = a["app"];
            if (app.is_array() && !app.empty() && app[0].is_string()) {
                art.source = app[0].get<std::string>();
            } else if (app.is_string()) {
                art.source = app.get<std::string>();
            }
            if (a.contains("target") && a["target"].is_string()) {
                art.target = a["target"].get<std::string>();
            }
            if (!art.source.empty()) {
                out.push_back(std::move(art));
            }
        }
        if (a.contains("binary")) {
            CaskArtifact art;
            art.kind = CaskArtifactKind::Binary;
            const auto& bin = a["binary"];
            if (bin.is_array() && !bin.empty()) {
                if (bin[0].is_string()) {
                    art.source = bin[0].get<std::string>();
                }
                // Optional nested {target: "name"} as second element.
                if (bin.size() > 1 && bin[1].is_object() && bin[1].contains("target") &&
                    bin[1]["target"].is_string()) {
                    art.target = bin[1]["target"].get<std::string>();
                }
            } else if (bin.is_string()) {
                art.source = bin.get<std::string>();
            }
            if (a.contains("target") && a["target"].is_string()) {
                // Full path target e.g. $HOMEBREW_PREFIX/bin/foo
                if (art.target.empty()) {
                    art.target = a["target"].get<std::string>();
                }
            }
            if (!art.source.empty()) {
                out.push_back(std::move(art));
            }
        }
        if (a.contains("pkg")) {
            CaskArtifact art;
            art.kind = CaskArtifactKind::Pkg;
            const auto& pkg = a["pkg"];
            if (pkg.is_array() && !pkg.empty() && pkg[0].is_string()) {
                art.source = pkg[0].get<std::string>();
            } else if (pkg.is_string()) {
                art.source = pkg.get<std::string>();
            }
            if (!art.source.empty()) {
                out.push_back(std::move(art));
            }
        }
    }
}

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
    auto take_url = [](const json& node, Package::Archive& ar) {
        if (node.contains("url") && node["url"].is_string()) {
            ar.url = node["url"].get<std::string>();
        }
        if (node.contains("sha256") && node["sha256"].is_string()) {
            ar.sha256 = node["sha256"].get<std::string>();
            if (ar.sha256 == "no_check") {
                ar.sha256.clear();
            }
        }
        ar.relocation = RelocationType::Portable;
    };

    if (c.contains("url") && c["url"].is_string()) {
        Package::Archive ar;
        ar.url = c["url"].get<std::string>();
        if (c.contains("sha256") && c["sha256"].is_string()) {
            ar.sha256 = c["sha256"].get<std::string>();
            if (ar.sha256 == "no_check") {
                ar.sha256.clear();
            }
        }
        ar.relocation = RelocationType::Portable;
        pkg.archives["default"] = std::move(ar);
    }

    // Variations may override url/sha per macOS/arch tag. Store them under
    // their tag names so best_archive_tag can prefer a native bottle-style tag
    // before falling back to "default".
    if (c.contains("variations") && c["variations"].is_object()) {
        for (auto it = c["variations"].begin(); it != c["variations"].end(); ++it) {
            if (!it.value().is_object()) {
                continue;
            }
            Package::Archive ar =
                pkg.archives.count("default") ? pkg.archives["default"] : Package::Archive{};
            take_url(it.value(), ar);
            if (!ar.url.empty()) {
                pkg.archives[it.key()] = std::move(ar);
            }
        }
    }

    if (c.contains("artifacts")) {
        parse_cask_artifacts(c["artifacts"], pkg.cask_artifacts);
        parse_cask_uninstall(c["artifacts"], pkg.cask_uninstall);
    }

    // Binary-only casks are still ArtifactType::App at the type level today
    // (install dispatches on cask_artifacts / the cask index path). Callers
    // treat anything from cask.json as a cask install.

    return pkg;
}

/// Serialize a Package to JSON.
json package_to_json(const Package& pkg) {
    json j;
    j["name"] = pkg.name;
    j["version"] = pkg.version;
    if (pkg.revision > 0) {
        j["revision"] = pkg.revision;
    }
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

    if (!pkg.cask_artifacts.empty()) {
        json arts = json::array();
        for (const auto& a : pkg.cask_artifacts) {
            json aj;
            switch (a.kind) {
            case CaskArtifactKind::App:
                aj["kind"] = "app";
                break;
            case CaskArtifactKind::Binary:
                aj["kind"] = "binary";
                break;
            case CaskArtifactKind::Pkg:
                aj["kind"] = "pkg";
                break;
            }
            aj["source"] = a.source;
            if (!a.target.empty()) {
                aj["target"] = a.target;
            }
            arts.push_back(std::move(aj));
        }
        j["cask_artifacts"] = std::move(arts);
    }
    if (!pkg.cask_uninstall.empty()) {
        j["cask_uninstall"] = uninstall_to_json(pkg.cask_uninstall);
    }

    return j;
}

/// Deserialize a Package from JSON.
Package package_from_json(const json& j) {
    Package pkg;
    pkg.name = j.value("name", "");
    pkg.version = j.value("version", "");
    if (j.contains("revision") && j["revision"].is_number_integer()) {
        pkg.revision = std::max(0, j["revision"].get<int>());
    }
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

    if (j.contains("cask_artifacts") && j["cask_artifacts"].is_array()) {
        for (const auto& aj : j["cask_artifacts"]) {
            if (!aj.is_object()) {
                continue;
            }
            CaskArtifact a;
            const std::string kind = aj.value("kind", "app");
            if (kind == "binary") {
                a.kind = CaskArtifactKind::Binary;
            } else if (kind == "pkg") {
                a.kind = CaskArtifactKind::Pkg;
            } else {
                a.kind = CaskArtifactKind::App;
            }
            a.source = aj.value("source", "");
            a.target = aj.value("target", "");
            if (!a.source.empty()) {
                pkg.cask_artifacts.push_back(std::move(a));
            }
        }
    }
    if (j.contains("cask_uninstall")) {
        merge_uninstall_directives(j["cask_uninstall"], pkg.cask_uninstall);
    }

    return pkg;
}

} // namespace

PackageIndex load_index(const fs::path& cache_path) {
    if (!fs::exists(cache_path)) {
        SPDLOG_INFO("no cached index at {}", cache_path.string());
        return PackageIndex{};
    }
    const bool use_cache = !index_cache::disabled_by_env();
    if (use_cache) {
        if (auto backing = index_cache::open(cache_path)) {
            PackageIndex idx;
            idx.backing = std::move(backing);
            SPDLOG_INFO("opened index lookup cache ({} packages)", idx.backing->size());
            return idx;
        }
    }
    auto idx = load_index_json(cache_path);
    if (use_cache && !idx.packages.empty()) {
        index_cache::write(idx, cache_path);
    }
    return idx;
}

PackageIndex load_index_json(const fs::path& cache_path) {
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

    // The lookup cache must reproduce exactly what parsing index.json yields,
    // so build it from the JSON round-trip rather than from `idx` (the JSON
    // form does not carry every in-memory field).
    PackageIndex persisted;
    for (const auto& pj : packages_array) {
        Package pkg = package_from_json(pj);
        std::string name = pkg.name;
        persisted.packages.emplace(std::move(name), std::move(pkg));
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

    if (!index_cache::disabled_by_env()) {
        index_cache::write(persisted, cache_path);
    } else {
        std::error_code ec;
        fs::remove(index_cache::cache_path_for(cache_path), ec);
    }
}

namespace {

/// Every distinct bottle-tag candidate list den can produce on macOS, per
/// arch. Derived from bottle_tag_candidates() so it tracks codename changes.
const std::vector<std::pair<Arch, std::optional<MacOsVersion>>>& macos_platforms() {
    static const auto platforms = [] {
        std::vector<std::pair<Arch, std::optional<MacOsVersion>>> out;
        for (Arch arch : {Arch::Arm64, Arch::X86_64}) {
            std::vector<std::vector<std::string>> seen;
            // Darwin 20+ maps to macOS 11+ (see detect_macos_version); scan
            // well past the newest known codename so future majors are covered.
            for (int major = 11; major <= 60; ++major) {
                MacOsVersion v{major, 0};
                auto cands = bottle_tag_candidates(arch, v);
                if (std::find(seen.begin(), seen.end(), cands) == seen.end()) {
                    seen.push_back(std::move(cands));
                    out.emplace_back(arch, v);
                }
            }
        }
        return out;
    }();
    return platforms;
}

/// The (url, sha256) den would download on each macOS platform; nullopt where
/// no archive is selectable.
std::vector<std::optional<std::pair<std::string, std::string>>>
cask_selection(const std::map<std::string, Package::Archive>& archives) {
    std::vector<std::string> tags;
    tags.reserve(archives.size());
    for (const auto& [tag, _] : archives) {
        tags.push_back(tag);
    }
    std::vector<std::optional<std::pair<std::string, std::string>>> out;
    for (const auto& [arch, macos] : macos_platforms()) {
        auto best = best_archive_tag(arch, macos, tags);
        if (best) {
            const auto& ar = archives.at(*best);
            out.emplace_back(std::make_pair(ar.url, ar.sha256));
        } else {
            out.emplace_back(std::nullopt);
        }
    }
    return out;
}

} // namespace

void trim_cask_archives(Package& pkg) {
    if (pkg.archives.size() <= 1) {
        return;
    }

    const auto original = cask_selection(pkg.archives);
    const bool any_selectable =
        std::any_of(original.begin(), original.end(), [](const auto& s) { return s.has_value(); });
    if (!any_selectable) {
        return; // keep the listing intact for `den info`
    }

    // 1. Casks never install on Linux.
    for (auto it = pkg.archives.begin(); it != pkg.archives.end();) {
        if (it->first.ends_with("_linux")) {
            it = pkg.archives.erase(it);
        } else {
            ++it;
        }
    }

    // 2. Greedily drop variants that are redundant on every macOS platform.
    //    "default" is the universal fallback, so it is never a candidate.
    std::vector<std::string> variant_tags;
    for (const auto& [tag, _] : pkg.archives) {
        if (tag != "default") {
            variant_tags.push_back(tag);
        }
    }
    for (const auto& tag : variant_tags) {
        auto node = pkg.archives.extract(tag);
        if (cask_selection(pkg.archives) != original) {
            pkg.archives.insert(std::move(node));
        }
    }
}

PackageIndex transform_homebrew(const std::string& formula_data, const std::string& cask_data) {
    PackageIndex idx;

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

    size_t formula_count = idx.packages.size();
    size_t variants_before = 0;
    size_t variants_after = 0;

    try {
        json casks = json::parse(cask_data);
        if (casks.is_array()) {
            for (const auto& c : casks) {
                Package pkg = transform_cask(c);
                variants_before += pkg.archives.size();
                trim_cask_archives(pkg);
                variants_after += pkg.archives.size();
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

    SPDLOG_INFO("indexed {} casks ({} total packages); cask archives {} -> {}",
                idx.packages.size() - formula_count, idx.packages.size(), variants_before,
                variants_after);

    return idx;
}

PackageIndex fetch_and_transform(const Config& /*config*/) {
    SPDLOG_INFO("fetching Homebrew formula index");
    std::string formula_data = fetch_url(kFormulaUrl);
    SPDLOG_INFO("fetching Homebrew cask index");
    std::string cask_data = fetch_url(kCaskUrl);
    return transform_homebrew(formula_data, cask_data);
}

} // namespace den

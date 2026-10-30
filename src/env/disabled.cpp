// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "disabled.h"

#include "../store/store.h"

#include <deque>
#include <map>
#include <set>

namespace den {

namespace {

/// Homebrew's wording for its disable!/deprecate! reason symbols
/// (DeprecateDisable::FORMULA_DEPRECATE_DISABLE_REASONS and
/// CASK_DEPRECATE_DISABLE_REASONS in brew's deprecate_disable.rb), so den's
/// refusal reads like brew's "has been disabled because it ...".
std::string disable_reason_phrase(const std::string& reason, bool cask) {
    static const std::map<std::string, std::string> formula_phrases = {
        {"does_not_build", "does not build"},
        {"no_license", "has no license"},
        {"repo_archived", "has an archived upstream repository"},
        {"repo_removed", "has a removed upstream repository"},
        {"unmaintained", "is not maintained upstream"},
        {"unreachable", "is no longer reliably reachable upstream"},
        {"unsupported", "is not supported upstream"},
        {"deprecated_upstream", "is deprecated upstream"},
        {"versioned_formula", "is a versioned formula"},
        {"checksum_mismatch", "was built from a source file whose checksum has since changed"},
    };
    static const std::map<std::string, std::string> cask_phrases = {
        {"discontinued", "is discontinued upstream"},
        {"moved_to_mas", "is now exclusively distributed on the Mac App Store"},
        {"no_longer_available", "is no longer available upstream"},
        {"no_longer_meets_criteria", "no longer meets the criteria for acceptable casks"},
        {"unmaintained", "is not maintained upstream"},
        {"fails_gatekeeper_check", "does not pass the macOS Gatekeeper check"},
        {"unreachable", "is no longer reliably reachable upstream"},
    };
    const auto& phrases = cask ? cask_phrases : formula_phrases;
    if (auto it = phrases.find(reason); it != phrases.end())
        return it->second;
    return reason;
}

} // namespace

std::string disabled_package_message(const Package& pkg) {
    std::string msg = "'" + pkg.name + "' is disabled upstream";
    if (!pkg.disable_reason.empty())
        msg += ": it " + disable_reason_phrase(pkg.disable_reason, is_cask_package(pkg));
    std::string details;
    if (!pkg.disable_date.empty())
        details = "disabled on " + pkg.disable_date;
    if (!pkg.disable_replacement.empty()) {
        if (!details.empty())
            details += "; ";
        details += "replacement: " + pkg.disable_replacement;
    }
    if (!details.empty())
        msg += " (" + details + ")";
    return msg;
}

bool is_cask_package(const Package& pkg) {
    return pkg.artifact_type == ArtifactType::App || !pkg.cask_artifacts.empty();
}

std::string disabled_dependency_message(const std::string& dependent, const Package& dep) {
    // "'bar' is disabled upstream: ..." -> "'foo' needs 'bar', which is ..."
    const auto msg = disabled_package_message(dep);
    const auto head = "'" + dep.name + "' is ";
    return "'" + dependent + "' needs '" + dep.name + "', which is " + msg.substr(head.size());
}

std::vector<const Package*> disabled_dependencies(const Config& config,
                                                  const PackageProvider& provider,
                                                  const PackageIndex& idx,
                                                  const std::string& name) {
    // Every runtime dependency is poured unless its index version is already
    // installed (HomebrewProvider::install), and a satisfied one's own
    // dependencies are still checked, so walk the whole closure.
    std::vector<const Package*> found;
    std::set<std::string> seen{keg_name(name)};
    std::deque<std::string> todo;
    if (const auto* root = idx.find(name))
        todo.insert(todo.end(), root->dependencies.begin(), root->dependencies.end());
    while (!todo.empty()) {
        auto dep_name = std::move(todo.front());
        todo.pop_front();
        if (!seen.insert(keg_name(dep_name)).second)
            continue;
        const auto* dep = idx.find(dep_name);
        if (!dep)
            continue;
        todo.insert(todo.end(), dep->dependencies.begin(), dep->dependencies.end());
        if (dep->disabled && !provider.is_installed(config, keg_name(dep->name), dep->version))
            found.push_back(dep);
    }
    return found;
}

std::vector<DisabledPour> disabled_pours(const Config& config, const PackageProvider& provider,
                                         const PackageIndex& idx, const std::string& name,
                                         bool include_self) {
    std::vector<DisabledPour> pours;
    if (include_self)
        if (const auto* pkg = idx.find(name); pkg && pkg->disabled)
            pours.push_back({pkg, disabled_package_message(*pkg)});
    for (const auto* dep : disabled_dependencies(config, provider, idx, name))
        pours.push_back({dep, disabled_dependency_message(name, *dep)});
    return pours;
}

} // namespace den

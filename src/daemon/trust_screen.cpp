// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "trust_screen.h"

#include "../provider/homebrew_provider.h"
#include "../store/store.h"
#include "../trust/degraded_installs.h"

#include <set>

namespace den {

std::vector<std::string> packages_to_install(const Config& config, const PackageIndex& idx,
                                             const std::string& name) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    std::vector<std::string> stack{name};
    while (!stack.empty()) {
        std::string n = stack.back();
        stack.pop_back();
        if (!seen.insert(n).second)
            continue;
        const Package* pkg = idx.find(n);
        if (!pkg)
            continue;
        if (n != name && is_installed(config.store, keg_name(pkg->name), pkg->version))
            continue;
        out.push_back(n);
        for (const auto& d : pkg->dependencies)
            stack.push_back(d);
    }
    return out;
}

std::map<std::string, std::string>
screen_unattended_upgrades(const Config& config, const PackageIndex& idx,
                           const std::vector<PendingUpgrade>& pending, const TrustPolicy& policy) {
    auto to_install = [&config, &idx](const std::string& name) {
        return packages_to_install(config, idx, name);
    };
    auto refusal = [&config, &idx, &policy](const std::string& name) -> std::optional<std::string> {
        const Package* pkg = idx.find(name);
        if (!pkg)
            return std::nullopt; // the upgrade itself reports the missing package
        auto trust = evaluate_bottle_trust(config, *pkg);
        if (!trust)
            return std::nullopt;
        bool accepted = trust->decision == TrustDecision::WarnProceed &&
                        is_install_accepted(config, pkg->name, pkg->version);
        switch (bottle_action(policy, trust->decision, accepted)) {
        case BottleAction::Install:
        case BottleAction::InstallDegraded:
            return std::nullopt;
        case BottleAction::HoldBack:
        case BottleAction::Refuse: {
            auto entry = trust_hold_entry(*pkg, *trust, pkg->archives.at(trust->bottle_tag).sha256);
            record_refused_install(config, entry);
            if (entry.reason == "single-source")
                return entry.reason + " (" + trust->reason_tag + ")";
            return entry.reason + ": " + trust->message;
        }
        }
        return std::nullopt;
    };
    auto held = screen_pending_upgrades(pending, to_install, refusal);
    // Dependents held back for a dependency's sake go in the ledger too.
    for (const auto& [name, reason] : held) {
        if (!reason.starts_with("depends on "))
            continue;
        if (const Package* pkg = idx.find(name)) {
            DegradedInstall dep{pkg->name, pkg->version, reason, {}, {}};
            record_refused_install(config, dep);
        }
    }
    return held;
}

std::map<std::string, std::string> screen_pending_upgrades(
    const std::vector<PendingUpgrade>& pending,
    const std::function<std::vector<std::string>(const std::string&)>& to_install,
    const std::function<std::optional<std::string>(const std::string&)>& refusal) {
    std::map<std::string, std::optional<std::string>> memo;
    auto refused = [&](const std::string& pkg) -> const std::optional<std::string>& {
        auto it = memo.find(pkg);
        if (it == memo.end())
            it = memo.emplace(pkg, refusal(pkg)).first;
        return it->second;
    };
    std::map<std::string, std::string> held;
    for (const auto& pu : pending) {
        // The package itself first, so its own reason wins over a dependency's.
        if (const auto& why = refused(pu.name)) {
            held[pu.name] = *why;
            continue;
        }
        for (const auto& pkg : to_install(pu.name)) {
            if (pkg == pu.name)
                continue;
            if (const auto& why = refused(pkg)) {
                held[pu.name] = "depends on " + pkg + ": " + *why;
                break;
            }
        }
    }
    return held;
}

std::function<std::map<std::string, std::string>(const std::vector<PendingUpgrade>&)>
live_trust_screen(const Config& config, const PackageIndex& idx) {
    return [&config, &idx](const std::vector<PendingUpgrade>& pending) {
        return screen_unattended_upgrades(config, idx, pending, current_trust_policy(config));
    };
}

std::string held_back_notification(const std::vector<std::string>& names) {
    std::string list;
    for (const auto& n : names)
        list += (list.empty() ? "" : ", ") + n;
    return "den held back " + std::to_string(names.size()) +
           (names.size() == 1 ? " upgrade" : " upgrades") + " (unattended trust policy): " + list +
           ". See `den doctor`.";
}

} // namespace den

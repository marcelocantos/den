// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "trust_screen.h"

namespace den {

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

std::string held_back_notification(const std::vector<std::string>& names) {
    std::string list;
    for (const auto& n : names)
        list += (list.empty() ? "" : ", ") + n;
    return "den held back " + std::to_string(names.size()) +
           (names.size() == 1 ? " upgrade" : " upgrades") + " (unattended trust policy): " + list +
           ". See `den doctor`.";
}

} // namespace den

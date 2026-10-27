// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "pin.h"

#include "../core/error.h"
#include "../env/environment.h"
#include "../env/manifest.h"
#include "../store/pins.h"

#include <CLI11.hpp>

#include <iostream>
#include <memory>

namespace den {

int run_pin(const Config& cfg, const std::vector<std::string>& names, std::ostream& out,
            std::ostream& err) {
    const auto resolved = resolve(cfg.den_home, active_env_path(cfg.den_home));
    int failures = 0;
    for (const auto& name : names) {
        auto it = resolved.find(name);
        if (it == resolved.end()) {
            // brew: "No such keg" — you can only pin what is installed.
            err << "error: '" << name << "' is not installed\n";
            ++failures;
            continue;
        }
        switch (pin_package(cfg.den_home, name, it->second)) {
        case PinOutcome::Pinned:
            out << "Pinned " << name << " at " << it->second << "\n";
            break;
        case PinOutcome::AlreadyPinned:
            out << name << " is already pinned at " << read_pins(cfg.den_home).at(name) << "\n";
            break;
        }
    }
    return failures;
}

int run_unpin(const Config& cfg, const std::vector<std::string>& names, std::ostream& out,
              std::ostream& err) {
    int failures = 0;
    for (const auto& name : names) {
        if (unpin_package(cfg.den_home, name)) {
            out << "Unpinned " << name << "\n";
        } else {
            err << "warning: '" << name << "' is not pinned\n";
            ++failures;
        }
    }
    return failures;
}

void list_pins(const Config& cfg, std::ostream& out) {
    const auto pins = read_pins(cfg.den_home);
    const auto resolved = resolve(cfg.den_home, active_env_path(cfg.den_home));
    for (const auto& [name, version] : pins) {
        out << name << " " << version;
        auto it = resolved.find(name);
        if (it == resolved.end()) {
            out << "  (not installed)";
        } else if (it->second != version) {
            out << "  (active: " << it->second << ")";
        }
        out << "\n";
    }
}

void register_pin_commands(CLI::App& app) {
    auto pin_names = std::make_shared<std::vector<std::string>>();
    auto pin_list = std::make_shared<bool>(false);
    auto* pin =
        app.add_subcommand("pin", "Pin packages at their current version so upgrades skip them");
    pin->add_option("names", *pin_names, "Installed packages to pin (lists pins if empty)");
    pin->add_flag("--list", *pin_list, "List pinned packages");
    pin->callback([pin_names, pin_list] {
        auto cfg = Config::detect();
        if (*pin_list || pin_names->empty()) {
            list_pins(cfg, std::cout);
            return;
        }
        if (int failed = run_pin(cfg, *pin_names, std::cout, std::cerr); failed > 0) {
            throw UserError(std::to_string(failed) + " package(s) could not be pinned");
        }
    });

    auto unpin_names = std::make_shared<std::vector<std::string>>();
    auto* unpin = app.add_subcommand("unpin", "Unpin packages so upgrades include them again");
    unpin->add_option("names", *unpin_names, "Pinned packages to unpin")->required();
    unpin->callback([unpin_names] {
        auto cfg = Config::detect();
        // brew warns (and still exits 0) for names that were not pinned.
        run_unpin(cfg, *unpin_names, std::cout, std::cerr);
    });
}

} // namespace den

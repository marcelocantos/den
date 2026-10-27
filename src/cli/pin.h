// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"

#include <ostream>
#include <string>
#include <vector>

namespace CLI {
class App;
}

namespace den {

/// Register `den pin` and `den unpin` on the top-level app.
void register_pin_commands(CLI::App& app);

/// `den pin <names...>`: pin each installed package at its active version.
/// Returns the number of names that could not be pinned (not installed).
int run_pin(const Config& cfg, const std::vector<std::string>& names, std::ostream& out,
            std::ostream& err);

/// `den unpin <names...>`. Returns the number of names that were not pinned.
int run_unpin(const Config& cfg, const std::vector<std::string>& names, std::ostream& out,
              std::ostream& err);

/// `den pin --list` (or bare `den pin`): one "name version" line per pin,
/// flagging pins whose package is no longer in the active environment.
void list_pins(const Config& cfg, std::ostream& out);

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"

#include <string>

namespace den {

/// Print shell initialisation code for `eval "$(den shell-init <shell>)"`.
/// Supported shells: "zsh", "bash", "fish".
/// Throws std::invalid_argument for unknown shells.
void print_shell_init(const Config& config, const std::string& shell);

/// Print `export VAR=value` lines (or fish `set -gx`) to switch the active
/// den environment to `env_slug`.  Intended to be eval'd by the shell
/// wrapper injected by print_shell_init.
void print_env_switch(const Config& config, const std::string& env_slug);

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "supervisor.h"

#include <filesystem>
#include <optional>
#include <string>

namespace den {

namespace fs = std::filesystem;

/// Parse a Homebrew-style launchd plist (`homebrew.mxcl.<name>.plist`) into
/// a ServiceSpec. Reads ProgramArguments → argv, EnvironmentVariables → env,
/// WorkingDirectory → working_dir, KeepAlive → restart policy.
///
/// Returns nullopt if the file cannot be opened or no ProgramArguments
/// array is found.
///
/// The parser is intentionally minimal — it handles the structural shapes
/// Homebrew formula plists use (dict / array / string / true / false) and
/// ignores fields den does not consume (integer / data / date /
/// RunAtLoad / StandardOutPath / StandardErrorPath).
std::optional<ServiceSpec> parse_homebrew_plist(const fs::path& path, const std::string& name);

} // namespace den

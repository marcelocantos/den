// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <filesystem>
#include <optional>

namespace den {

/// Path of the running den executable as the OS reports it (on macOS this may
/// be a symlink such as <prefix>/bin/den; on Linux /proc/self/exe is already
/// resolved). nullopt when it cannot be determined.
std::optional<std::filesystem::path> current_executable_path();

} // namespace den

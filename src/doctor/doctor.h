// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"

#include <string>
#include <vector>

namespace den {

enum class Severity {
    Warning,
    Error,
};

struct Finding {
    Severity severity;
    std::string message;
};

/// The prefix Homebrew bottles are built for on this host: /opt/homebrew
/// (macOS arm64), /usr/local (macOS x86_64), /home/linuxbrew/.linuxbrew (Linux).
std::string standard_bottle_prefix(Arch arch);

/// Mach-O header-space risk. Relocating a bottle rewrites load commands
/// (LC_ID_DYLIB, LC_LOAD_DYLIB, LC_RPATH) in place, so a path that grows can
/// only use the binary's spare header padding. A prefix longer than the one
/// the bottle was built for can therefore fail to relocate. Returns a warning
/// message when `prefix` is longer than `standard` on a Mach-O platform, or an
/// empty string when it fits (or when `macho` is false: ELF is rewritten by
/// patchelf, which can grow the dynamic section).
std::string prefix_header_space_warning(const std::string& prefix, const std::string& standard,
                                        bool macho);

/// Run all health checks and print a summary.
/// Returns the list of findings so callers can set a non-zero exit code
/// when there are errors.
std::vector<Finding> doctor(const Config& config);

} // namespace den

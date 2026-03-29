// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "source_build.h"

#include <string>
#include <vector>

namespace den {

/// Parse a Homebrew formula's install method from `brew cat` output
/// and extract the build commands as concrete shell commands.
///
/// Handles:
/// - system "cmd", "arg1", "arg2"
/// - *std_configure_args, *std_cmake_args, *std_meson_args
/// - #{prefix}, #{lib}, #{include}, #{bin}, #{share}
/// - Formula["dep"].opt_prefix references
/// - ENV.append/prepend for CPPFLAGS, LDFLAGS, etc.
///
/// The prefix parameter is substituted for #{prefix} etc.
struct ParsedFormula {
    std::string source_url;
    std::string source_sha256;
    std::vector<std::string> env_settings; // "KEY=value" strings
    std::vector<std::string> build_commands; // shell commands to run
};

ParsedFormula parse_formula(const std::string& brew_cat_output, const std::string& prefix,
                            const std::string& name);

} // namespace den

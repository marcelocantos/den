// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>
#include <vector>

namespace den {

/// Parse a Homebrew formula's install method from its Ruby source and
/// extract the build commands as concrete shell commands.
///
/// Handles (on the SIMPLE fast path):
/// - system "cmd", "arg1", "arg2"
/// - *std_configure_args, *std_cmake_args, *std_meson_args
/// - #{prefix}, #{lib}, #{include}, #{bin}, #{share}
/// - Formula["dep"].opt_prefix references
/// - ENV.append/prepend for CPPFLAGS, LDFLAGS, etc.
///
/// The prefix parameter is substituted for #{prefix} etc.
///
/// Anything else — `if`/`unless`/`case`, `on_macos`/`on_linux`/`on_arm`,
/// `resource`, `patch`, `inreplace`, `bottle`, generic `do` blocks, or any
/// DSL method the parser does not explicitly handle — causes the formula
/// to be classified as COMPLEX. The parser must never silently skip an
/// unhandled construct: every such token appends to `complexity_markers`
/// and the final verdict is `Complex`. Callers must consult `complexity`
/// before using `build_commands`/`env_settings` — those fields are only
/// valid on a `Simple` verdict.
enum class FormulaComplexity {
    Simple,     ///< Fast path: all constructs are handled, build is extractable.
    Complex,    ///< A known-but-unhandled DSL construct was seen; use Ruby.
    Unsupported ///< A malformed or missing formula; neither path can proceed.
};

struct ComplexityMarker {
    std::string construct; ///< e.g. "if", "resource", "on_macos", "do"
    int line;              ///< 1-based line within the install body (0 if unknown)
    std::string detail;    ///< the offending line, trimmed
};

struct ParsedFormula {
    FormulaComplexity complexity = FormulaComplexity::Unsupported;
    std::vector<ComplexityMarker> complexity_markers;
    std::string source_url;
    std::string source_sha256;
    std::vector<std::string> env_settings; // "KEY=value" strings
    std::vector<std::string> build_commands; // shell commands to run
};

ParsedFormula parse_formula(const std::string& brew_cat_output, const std::string& prefix,
                            const std::string& name);

/// Human-readable name for a complexity verdict.
const char* to_string(FormulaComplexity c);

} // namespace den

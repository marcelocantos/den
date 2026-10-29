// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"

#include <filesystem>
#include <iosfwd>
#include <map>
#include <string>
#include <string_view>

namespace den {

namespace fs = std::filesystem;

using EnvMap = std::map<std::string, std::string>;

/// Parse a KEY=VALUE snapshot (the bundle's homebrew_env.txt). Blank lines,
/// comments and lines without '=' (continuations of multi-line values) are
/// skipped.
EnvMap parse_env_snapshot(std::istream& in);
EnvMap read_env_snapshot(const fs::path& file);

/// Drop PATH entries under a Homebrew prefix that is not den's own prefix
/// (/opt/homebrew, /home/linuxbrew/.linuxbrew, config.homebrew_prefix), so a
/// build into a separate prefix never picks up tools from a live Homebrew.
std::string sanitize_build_path(const Config& config, const std::string& path);

/// The environment for the bundled-Ruby source build (den_build.rb).
///
/// The bundle's homebrew_env.txt was captured from a real `brew` on the
/// machine that built the bundle, so every path in it names that machine's
/// /opt/homebrew and home directory. Only its non-path values (versions,
/// API domains, user agent, ...) are kept; every path the Homebrew library
/// reads is derived from den's configuration instead: HOMEBREW_PREFIX is
/// den's prefix, HOMEBREW_CELLAR den's Cellar, caches, logs and config under
/// den's home, HOMEBREW_LIBRARY the bundled library (ruby_dir/Library). This holds for both the
/// default layout (<den_home>/prefix) and the migrate layout (den's prefix is
/// Homebrew's).
EnvMap bundled_ruby_env(const Config& config, const fs::path& ruby_dir, const EnvMap& snapshot,
                        const std::string& path);

/// The den_build.rb this den was built with (compiled in from
/// src/ruby/den_build.rb).
std::string_view embedded_den_build_rb();

/// Rewrite ruby_dir/den_build.rb when it differs from the embedded copy, so
/// an unpacked bundle never runs a stale build script. Returns true if the
/// file was (re)written.
bool refresh_den_build_script(const fs::path& ruby_dir);

/// Give the unpacked bundle Homebrew's own layout: ruby_dir/Library/Homebrew
/// is a relative link to ruby_dir/homebrew, and HOMEBREW_LIBRARY names
/// ruby_dir/Library. Homebrew builds paths as HOMEBREW_LIBRARY/"Homebrew/..."
/// (HOMEBREW_SHIMS_PATH, the macOS system .pc files formulae such as pkgconf
/// bake into their search path), which otherwise name nothing. Returns true
/// if the link was (re)created.
bool ensure_bundle_library_link(const fs::path& ruby_dir);

} // namespace den

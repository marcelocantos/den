// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"
#include "../index/package.h"
#include "../platform/platform.h"

#include <filesystem>
#include <optional>

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

/// Mach-O header-space note. Relocating a bottle rewrites load commands
/// (LC_ID_DYLIB, LC_LOAD_DYLIB, LC_RPATH) in place, so a path that grows can
/// only use the binary's spare header padding. Like Homebrew, den pours
/// :any / :any_skip_relocation bottles into any prefix, refuses fixed-cellar
/// bottles that don't match its Cellar, and fails an install cleanly if a
/// rewrite runs out of header space. So a longer prefix is informational, not
/// a problem. Returns the note when `prefix` is longer than `standard` on a
/// Mach-O platform, or an empty string when it fits (or when `macho` is
/// false: ELF is rewritten by patchelf, which can grow the dynamic section).
std::string prefix_header_space_note(const std::string& prefix, const std::string& standard,
                                     bool macho);

/// Kegs already in `store` whose bottle (the index's best archive for this
/// host) was built for a fixed Cellar other than `store`. den now refuses to
/// pour these; any that are present predate that and likely don't work.
/// Returns "name/version (bottle Cellar /path)" entries, sorted.
std::vector<std::string> kegs_with_mismatched_cellar(const fs::path& store, const PackageIndex& idx,
                                                     Arch arch,
                                                     const std::optional<MacOsVersion>& macos);

/// The root environment is linked into the prefix, so `<prefix>/bin` and
/// `<prefix>/sbin` must lead PATH (den's own bin dir may sit in front of
/// them). Returns a description of the problem, or an empty string when
/// `path_env` is in the right order.
std::string root_path_order_problem(const std::string& path_env, const fs::path& prefix,
                                    const fs::path& den_bin);

/// Plain-words pointer printed after the broken-symlink findings. With a
/// prefix shared with Homebrew (`shared`), links usually break because brew
/// upgraded or removed kegs, and `den migrate-prefix` is the fix. Doctor
/// never repairs links.
std::string broken_symlinks_hint(int broken, bool shared);

/// The doctor warning for a den home that still installs into Homebrew's
/// prefix (a legacy shared Cellar), pointing at `den migrate-prefix`.
std::string shared_prefix_hint();

/// A file mode as four octal digits, e.g. 0644 -> "0644".
std::string format_file_mode(unsigned mode);

/// Problem description when `mode` gives group or other any access to the
/// sensitive file `p` (config.json, *.key, *.pem), or an empty string when it
/// is private. The mode is shown in octal.
std::string sensitive_file_mode_problem(const fs::path& p, unsigned mode);

/// One thing den's own prefix still takes from Homebrew's.
struct BrewDependency {
    std::string keg;     // "openssl@3 3.6.3" (kept with brew) or "libssh2/1.11.1_7" (den keg)
    std::string detail;  // how it reaches Homebrew's prefix
    bool broken = false; // what it reaches no longer exists
};

/// What den's prefix still depends on in Homebrew's (`config.homebrew_prefix`):
/// manifest kegs den left with brew (their `<prefix>/opt/<name>` link points
/// into Homebrew's prefix, as `den migrate-prefix` makes for bottles that only
/// pour into Homebrew's Cellar), then kegs in den's store whose Mach-O load
/// commands reach into Homebrew's prefix, literally or through such a link.
/// Empty when den shares Homebrew's prefix (everything is brew's then).
std::vector<BrewDependency> remaining_brew_dependencies(const Config& config);

/// The doctor text for `deps`: "remaining brew dependencies: N" and one
/// indented line each.
std::string format_brew_dependencies(const std::vector<BrewDependency>& deps);

/// Run all health checks and print a summary.
/// Returns the list of findings so callers can set a non-zero exit code
/// when there are errors.
std::vector<Finding> doctor(const Config& config);

} // namespace den

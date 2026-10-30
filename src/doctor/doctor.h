// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"
#include "../daemon/daemon.h"
#include "../index/package.h"
#include "../platform/platform.h"
#include "../store/store.h"

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

/// A keg den built from source, from its receipt.
struct SourceBuiltKeg {
    std::string name;
    std::string version;
    SourceBuildReceipt receipt;
    /// Why the keg should be rebuilt, or empty: the index's formula for the
    /// same version now names a source whose sha256 differs from the one the
    /// keg was built from.
    std::string pending_rebuild;
};

/// Kegs in `store` whose receipt says den built them from source, sorted by
/// name then version. With `idx`, each is checked against the index's
/// current formula for a pending rebuild.
std::vector<SourceBuiltKeg> source_built_kegs(const fs::path& store, const PackageIndex* idx);

/// The doctor note listing `kegs` ("source-built kegs: N" and one indented
/// line each), or an empty string when there are none.
std::string format_source_built_kegs(const std::vector<SourceBuiltKeg>& kegs);

/// How den's own kegs bind the dependencies they load (one scan of their
/// Mach-O and ELF files).
struct KegLinkage {
    /// Kegs still pinned to a dependency's versioned keg
    /// (`<Cellar>/<dep>/<version>/...`): they keep loading that version after
    /// the dependency upgrades. "app 1.0 -> depa 1.0 (2 load paths, e.g.
    /// bin/app)", sorted.
    std::vector<std::string> pinned;
    /// Libraries kegs load through `<prefix>/opt/<dep>` where that link is
    /// missing or dangling, or (for an exact Mach-O load path) the keg it
    /// names lacks the file. "app 1.0: bin/app loads <path>: <why>", sorted.
    std::vector<std::string> broken_opt;
};

/// Scan every den-owned keg in `config.store`.
KegLinkage scan_keg_linkage(const Config& config);

/// The doctor note for `pinned` ("kegs pinned to versioned dependency kegs:
/// N" and one indented line each), or empty. In a prefix shared with
/// Homebrew (`shared`) pins are expected and only the count is shown.
std::string format_pinned_kegs(const std::vector<std::string>& pinned, bool shared);

/// Installed packages the upgrade plan holds (core/version.h
/// VersionDelta::Held): the index's only version for the formula is lower
/// than the installed one but carries a higher version_scheme (openssl@3
/// 3.6.3 installed, index pins 3.5.9 with version_scheme 1), so there is no
/// newer version in the installed line and den never downgrades on its own.
/// Computed now from `idx` and the active environment's homebrew bucket with
/// the daemon's compute_pending, the plan `den upgrade` and the maintenance
/// cycle use, rather than read back from daemon_state.json. Sorted by name.
std::vector<DeferredUpgrade> held_packages(const Config& config, const PackageIndex& idx);

/// The doctor warning text for `held` ("N package(s) held ..." and, per
/// package, installed vs available, the reason, and `den use <name>
/// <version>` to downgrade deliberately), or an empty string when none.
std::string format_held_packages(const std::vector<DeferredUpgrade>& held);

/// The doctor warning text for upgrades the daemon did not apply because
/// they would pour a package disabled upstream (DaemonState::disabled_blocked,
/// one disabled_blocked_message per upgrade, and what to do about it), or an
/// empty string when none.
std::string format_disabled_blocked(const std::vector<DisabledBlocked>& blocked);

/// Run all health checks and print a summary.
/// Returns the list of findings so callers can set a non-zero exit code
/// when there are errors.
std::vector<Finding> doctor(const Config& config);

} // namespace den

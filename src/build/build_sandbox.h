// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Source-build sandbox (docs/source-builds.md, "Build sandbox").
//
// On macOS every configure, build and install command a source build runs
// executes under a sandbox-exec (Seatbelt) profile that denies writes to
// den's prefix and to $DEN_HOME, except the package's build directory, its
// staging DESTDIR and its cache temp directory. An install step that ignores
// DESTDIR therefore fails instead of writing the live opt path -- or, when
// the opt link already names an older keg, through it into that keg.
//
// The profile mirrors Homebrew's (Library/Homebrew/sandbox.rb): a
// `(debug deny)` header, paths resolved through symlinks (Seatbelt matches
// real paths), Homebrew's quoting, `/usr/bin/sandbox-exec -p <profile>`, and
// a refusal to nest inside another sandbox. Unlike Homebrew's it is a deny
// list: den's own trees plus the system roots a build must never modify
// (system_deny_roots). Everything else stays as writable as it is for den
// itself.
//
// Linux has no equivalent yet (Landlock is allow-list only, and bwrap is not
// generally installed), so there build commands run unsandboxed and
// OptPathGuard remains the only check.

#include "../core/config.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

struct BuildSandbox {
    /// Trees build commands may not write: den's prefix (and its store, if
    /// that resolves elsewhere), $DEN_HOME and system_deny_roots().
    std::vector<fs::path> deny_write;
    /// The exceptions: build directory, staging DESTDIR and cache temp (in
    /// that order), then ~/Library/Developer (Xcode's caches, which Homebrew
    /// also leaves writable).
    std::vector<fs::path> allow_write;
    /// The cache temp directory, `<cache>/tmp/<name>-<version>`: TMPDIR for
    /// build commands.
    fs::path temp_dir;
};

/// System roots no build command may write, whatever den's own layout:
/// Homebrew's default prefixes /opt/homebrew and /usr/local (Cellar
/// included), the detected Homebrew prefix and Cellar
/// (config.homebrew_prefix / homebrew_cellar) where they differ, and
/// $HOME/Library. Resolved through symlinks, de-duplicated.
///
/// Test hook: when DEN_BUILD_SANDBOX_SYSTEM_ROOT is set, the fixed roots
/// (/opt/homebrew, /usr/local and $HOME/Library) are rebased under it, so
/// tests exercise them without touching the real system paths.
std::vector<fs::path> system_deny_roots(const Config& config);

/// The sandbox for building <name> <version>. Allowed trees are
/// `<cache>/build/<name>-<version>`, `<cache>/build/<name>-<version>.destdir`
/// and `<cache>/tmp/<name>-<version>`. Every path is resolved through
/// symlinks (as far as it exists), since Seatbelt matches real paths.
BuildSandbox build_sandbox_for(const Config& config, const std::string& name,
                               const std::string& version);

/// A Seatbelt string literal: `"` and `\` escaped, as Homebrew quotes.
std::string seatbelt_quote(const std::string& s);

/// The Seatbelt profile for a sandbox.
std::string seatbelt_profile(const BuildSandbox& sandbox);

/// True where build commands run sandboxed: macOS with an executable
/// /usr/bin/sandbox-exec.
bool build_sandbox_supported();

/// True when this process already runs inside a macOS sandbox, where
/// sandbox-exec cannot apply another profile (Homebrew's nested_sandbox?).
bool inside_macos_sandbox();

/// Throws UserError when the sandbox is supported but cannot be applied
/// because den itself is sandboxed.
void require_build_sandbox();

/// A shell command that runs `cmd` under the sandbox:
/// `/usr/bin/sandbox-exec -p '<profile>' /bin/sh -c '<cmd>'`. Unchanged where
/// the sandbox is not supported.
std::string sandboxed_shell_command(const BuildSandbox& sandbox, const std::string& cmd);

/// Language tool caches for build commands, all under sandbox.temp_dir:
/// GOCACHE, GOMODCACHE, GOPATH, PIP_CACHE_DIR, npm_config_cache and
/// CARGO_HOME. Their defaults sit under $HOME/Library/Caches (which the
/// sandbox denies) or in the home directory (~/go, ~/.npm, ~/.cargo, which a
/// build should not leave state in); the temp directory is writable under the
/// profile and is removed after the build.
std::map<std::string, std::string> build_cache_env(const BuildSandbox& sandbox);

} // namespace den

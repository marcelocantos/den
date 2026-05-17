// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../store/store.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace den {

namespace fs = std::filesystem;

struct Config;

/// Outcome of a single `install` call.
///
/// `resolved_version` is what the provider actually installed — may differ
/// from a user-supplied hint (e.g. hint "1.x" resolves to "1.4.2").
/// `auto_deps` lists transitive dependencies the provider pulled in alongside
/// the requested package; the CLI records these in the manifest's auto_deps
/// set so subsequent `autoremove` can prune them when nothing else needs
/// them.
struct InstallResult {
    std::string resolved_version;
    std::vector<std::string> auto_deps;
};

/// A package provider is the install/uninstall/list/binary-paths surface for
/// one packaging ecosystem (Homebrew, pip, npm, cargo, go, …). Providers are
/// the unit of pluggability: adding a new ecosystem means writing a new
/// implementation, registering it, and adding a manifest schema key — nothing
/// in CLI dispatch, manifest layout, or environment composition is
/// ecosystem-specific.
///
/// All paths are absolute. All methods are synchronous. Errors are reported
/// via thrown exceptions deriving from `den::Error` (see `core/error.h`);
/// the CLI layer catches them and reports a user-friendly message.
///
/// Threading: providers may be called from multiple threads serially, but a
/// single provider call is not required to be reentrant. Provider
/// implementations may rely on the calling code to serialise.
class PackageProvider {
   public:
    virtual ~PackageProvider() = default;

    /// Short, stable identifier — "homebrew", "pip", "npm", "cargo", "go".
    /// Used as the manifest key for this provider's entries and as the
    /// argument to `--provider`. Must match the regex `[a-z][a-z0-9_-]*`.
    virtual std::string_view provider_name() const = 0;

    /// Does this provider claim ownership of `name`? Used by the resolution
    /// dispatch when no explicit `--provider` flag and no manifest hint
    /// exists. The default-fallback provider (Homebrew today) returns true
    /// for any name as a last resort; specialised providers return true only
    /// for names that match their ecosystem's naming conventions
    /// (e.g. pip recognises PyPI package names).
    ///
    /// Multiple providers may claim a single name. The resolution dispatch
    /// resolves this via documented precedence (explicit flag > manifest
    /// hint > first-claim-in-registration-order).
    virtual bool accepts(std::string_view name) const = 0;

    /// Install `name` into the active environment. `version_hint` is a
    /// best-effort hint from the user or manifest — providers MAY ignore it
    /// or resolve it to a different concrete version. Returns metadata
    /// describing what was actually installed.
    ///
    /// The provider is responsible for: resolving dependencies (transitive),
    /// downloading artifacts, placing files into its own storage area, and
    /// updating any provider-private bookkeeping. The provider is NOT
    /// responsible for updating the den manifest or re-materialising the
    /// environment — the CLI layer owns those.
    virtual InstallResult install(const Config& config, std::string_view name,
                                  std::string_view version_hint) = 0;

    /// Remove `name` from the active environment's bookkeeping for this
    /// provider. Whether the underlying files are deleted from the provider's
    /// storage is provider-defined; some keep them for fast reinstall
    /// (Homebrew shared Cellar), others reclaim immediately (pip).
    virtual void uninstall(const Config& config, std::string_view name) = 0;

    /// All packages this provider considers installed on the host. The
    /// returned `InstalledPackage::path` MUST be the absolute path to the
    /// keg-equivalent root — `binary_paths()` resolves relative paths
    /// against it.
    virtual std::vector<InstalledPackage> list_installed(const Config& config) const = 0;

    /// Is a specific (name, version) installed?
    /// Default impl scans `list_installed()`; providers SHOULD override with
    /// a cheaper direct check when feasible.
    virtual bool is_installed(const Config& config, std::string_view name,
                              std::string_view version) const;

    /// Binary paths to surface in the environment's PATH for `pkg`. Returned
    /// paths are absolute and must exist; the env materialiser symlinks them
    /// (or their executable contents) into the env's `bin/`.
    ///
    /// Returning an empty vector means "this package has no PATH entries"
    /// (e.g. a library-only package). It is not an error.
    virtual std::vector<fs::path> binary_paths(const Config& config,
                                               const InstalledPackage& pkg) const = 0;
};

/// Type-erased smart pointer used throughout the codebase.
using PackageProviderPtr = std::shared_ptr<PackageProvider>;

}  // namespace den

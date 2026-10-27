// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/types.h"
#include "sat_solver.h"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace den {

// Stability rating for a package version (🎯T30).  Reuses the solver's
// enum so the index, solver, and CLI share a single ordered vocabulary.
using Stability = sat::Stability;

/// Kind of cask artifact den knows how to place.
enum class CaskArtifactKind {
    App,    // .app bundle → appdir (/Applications or DEN_APPDIR)
    Binary, // bare executable → env bin symlink
    Pkg,    // macOS installer package
    Font,   // font file → user font dir (~/Library/Fonts or DEN_FONTDIR)
    // command_wrapper: den writes a small shell script and links it like a
    // binary. `source` holds the script template (placeholders such as
    // $APPDIR and $HOMEBREW_PREFIX are substituted at install time) and
    // `target` the command name (often "$HOMEBREW_PREFIX/bin/foo").
    Wrapper,
};

/// One installable artifact declared by a Homebrew cask.
struct CaskArtifact {
    CaskArtifactKind kind = CaskArtifactKind::App;
    std::string source; // Path inside the container (e.g. "Foo.app", "bin/foo")
    std::string target; // Optional install target (app path or $HOMEBREW_PREFIX/bin/foo)
};

/// A cask's `uninstall` stanza (🎯T12). Directive values are kept exactly
/// as the cask declares them; safety checks happen when they run
/// (src/cask/uninstall_stanza.cpp).
struct CaskUninstall {
    std::vector<std::string> quit;                           // bundle ids
    std::vector<std::pair<std::string, std::string>> signal; // (signal, bundle id)
    std::vector<std::string> launchctl;                      // launchd labels
    std::vector<std::string> pkgutil;                        // pkg receipt ids / patterns
    std::vector<std::string> delete_paths;
    std::vector<std::string> trash;
    std::vector<std::string> rmdir;
    std::vector<std::string> unsupported; // directive names den does not run (script, kext, …)

    bool empty() const {
        return quit.empty() && signal.empty() && launchctl.empty() && pkgutil.empty() &&
               delete_paths.empty() && trash.empty() && rmdir.empty() && unsupported.empty();
    }
};

// A package in den's index. Unified model — no formula/cask distinction.
// The artifact_type field controls install behaviour internally.
struct Package {
    std::string name; // "ffmpeg" or "marcelocantos/jevon"

    // Installed version, matching the keg directory name. For Homebrew
    // formulae this is Homebrew's pkg_version: versions.stable, plus
    // "_<revision>" when the formula's revision is non-zero (e.g. "1.25.0_1").
    // Bottles unpack as <name>/<pkg_version>/ and dependents' load commands
    // and wrapper scripts name that exact directory, so the keg must use it.
    std::string version;

    // Homebrew formula revision (0 when absent). Already folded into
    // `version`; kept separately so the upstream version can be recovered.
    int revision = 0;
    std::string description;
    std::string homepage;
    std::string license;
    ArtifactType artifact_type = ArtifactType::Binary;

    // Stability rating (🎯T30). Defaults to Stable when the index carries no
    // explicit rating — Homebrew's stable channel is the common case.
    Stability stability = Stability::Stable;

    std::vector<std::string> dependencies;
    std::vector<std::string> build_dependencies;

    // Pre-built archive info, keyed by platform tag (e.g. "arm64_sequoia").
    // Casks use the synthetic tag "default" (and optional variation tags).
    struct Archive {
        std::string url;
        std::string sha256;
        RelocationType relocation = RelocationType::Portable;
        // Homebrew's `cellar` for this bottle when it is a fixed path (e.g.
        // /opt/homebrew/Cellar): the bottle only works poured there. Empty
        // for :any / :any_skip_relocation, and for index entries written
        // before den recorded it (see required_bottle_cellar()).
        std::string cellar;
    };
    std::map<std::string, Archive> archives;

    // Cask artifacts (app / binary / pkg). Empty for formulae.
    std::vector<CaskArtifact> cask_artifacts;

    // Cask `uninstall` stanza (merged across all uninstall blocks).
    CaskUninstall cask_uninstall;

    // Tap formula that installs a prebuilt download (url + sha256 for this
    // platform, plus a whitelisted `def install` recipe) instead of a bottle.
    // Only set for tap formulae, at merge time; never serialised. See
    // src/tap/url_formula.h.
    struct UrlInstall {
        std::string url;
        std::string sha256;
        std::vector<std::string> install_lines; // raw `def install` body, joined statements
        std::string refusal; // non-empty: why den cannot install this formula natively
    };
    std::optional<UrlInstall> url_install;

    // Source build info.
    std::optional<std::string> source_url;
    std::optional<std::string> source_sha256;
    std::optional<std::string> ruby_source_path; // e.g. "Formula/t/tree.rb"

    // Names this package was previously published under (Homebrew cask
    // "old_tokens", formula "oldnames"), so `den install <old>` follows the
    // rename the way brew does.
    std::vector<std::string> old_names;

    // Linking control.
    bool keg_only = false;
    std::string keg_only_reason; // e.g. ":versioned_formula", ":provided_by_macos", or custom
    std::vector<std::string> conflicts_with;

    // Metadata.
    bool deprecated = false;
    bool disabled = false;

    /// The upstream version without Homebrew's "_<revision>" suffix (what
    /// Homebrew calls versions.stable). Equal to `version` when revision is 0.
    std::string upstream_version() const {
        if (revision <= 0) {
            return version;
        }
        const std::string suffix = "_" + std::to_string(revision);
        if (version.size() > suffix.size() &&
            version.compare(version.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return version.substr(0, version.size() - suffix.size());
        }
        return version;
    }

    // Is this a "bare" name (Homebrew-sourced) or "owner/repo" (GitHub)?
    bool is_github_package() const { return name.find('/') != std::string::npos; }
};

/// Read-only, lazily-decoded backing store for a PackageIndex (see
/// index_cache.h). Lets index-backed commands look up a handful of packages
/// without parsing the whole index.json.
class IndexBacking {
  public:
    virtual ~IndexBacking() = default;

    /// Decode one package by exact name, or nullopt if absent.
    virtual std::optional<Package> lookup(const std::string& name) const = 0;

    /// Number of packages in the backing store.
    virtual size_t size() const = 0;

    /// Decode every package into `out` (existing keys are left untouched).
    virtual void load_all(std::map<std::string, Package>& out) const = 0;
};

// The full package index.
//
// `packages` holds every package that has been materialised so far. When the
// index was opened from the binary lookup cache, `backing` serves the rest on
// demand: find() decodes a single entry and memoises it into `packages`.
// Entries already in `packages` (e.g. merged taps) take precedence over the
// backing store. Code that needs to *iterate* every package must call
// materialise_all() first.
struct PackageIndex {
    mutable std::map<std::string, Package> packages;
    std::shared_ptr<const IndexBacking> backing;

    const Package* find(const std::string& name) const {
        if (!backing) {
            auto it = packages.find(name);
            return it != packages.end() ? &it->second : nullptr;
        }
        // Backed indexes memoise into `packages` from a const method, so
        // serialise lookups. Pointers into std::map stay valid across inserts.
        std::lock_guard lock(memo_mutex());
        auto it = packages.find(name);
        if (it != packages.end()) {
            return &it->second;
        }
        auto pkg = backing->lookup(name);
        if (!pkg) {
            return nullptr;
        }
        return &packages.emplace(name, std::move(*pkg)).first->second;
    }

    /// The current package that lists `old_name` among its old names, or
    /// nullptr. Only meaningful after an exact find() miss (a live package
    /// always wins, as in brew); it scans the whole index, so keep it off hot
    /// paths.
    const Package* find_renamed(const std::string& old_name) const {
        if (find(old_name)) {
            return nullptr;
        }
        auto scan = [&](const std::map<std::string, Package>& m) -> std::string {
            for (const auto& [key, pkg] : m) {
                for (const auto& o : pkg.old_names) {
                    if (o == old_name) {
                        return key;
                    }
                }
            }
            return {};
        };
        std::string target;
        if (backing) {
            std::map<std::string, Package> all;
            backing->load_all(all);
            target = scan(all);
        }
        if (target.empty()) {
            target = scan(packages);
        }
        return target.empty() ? nullptr : find(target);
    }

    /// True when the index holds no packages at all (materialised or backed).
    bool empty() const { return packages.empty() && (!backing || backing->size() == 0); }

    /// Decode every backed package into `packages` and drop the backing, so
    /// that iterating `packages` sees the complete index.
    void materialise_all() {
        if (backing) {
            backing->load_all(packages);
            backing.reset();
        }
    }

  private:
    static std::mutex& memo_mutex() {
        static std::mutex m;
        return m;
    }
};

} // namespace den

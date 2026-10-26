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
};

/// One installable artifact declared by a Homebrew cask.
struct CaskArtifact {
    CaskArtifactKind kind = CaskArtifactKind::App;
    std::string source; // Path inside the container (e.g. "Foo.app", "bin/foo")
    std::string target; // Optional install target (app path or $HOMEBREW_PREFIX/bin/foo)
};

// A package in den's index. Unified model — no formula/cask distinction.
// The artifact_type field controls install behaviour internally.
struct Package {
    std::string name; // "ffmpeg" or "marcelocantos/jevon"
    std::string version;
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
    };
    std::map<std::string, Archive> archives;

    // Cask artifacts (app / binary / pkg). Empty for formulae.
    std::vector<CaskArtifact> cask_artifacts;

    // Source build info.
    std::optional<std::string> source_url;
    std::optional<std::string> source_sha256;
    std::optional<std::string> ruby_source_path; // e.g. "Formula/t/tree.rb"

    // Linking control.
    bool keg_only = false;
    std::string keg_only_reason; // e.g. ":versioned_formula", ":provided_by_macos", or custom
    std::vector<std::string> conflicts_with;

    // Metadata.
    bool deprecated = false;
    bool disabled = false;

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

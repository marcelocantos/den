// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/types.h"
#include "sat_solver.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace den {

// Stability rating for a package version (🎯T30).  Reuses the solver's
// enum so the index, solver, and CLI share a single ordered vocabulary.
using Stability = sat::Stability;

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
    struct Archive {
        std::string url;
        std::string sha256;
        RelocationType relocation = RelocationType::Portable;
    };
    std::map<std::string, Archive> archives;

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

// The full package index.
struct PackageIndex {
    std::map<std::string, Package> packages;

    const Package* find(const std::string& name) const {
        auto it = packages.find(name);
        return it != packages.end() ? &it->second : nullptr;
    }
};

} // namespace den

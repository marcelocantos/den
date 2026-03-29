// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

struct Manifest {
    /// Explicitly requested packages: name -> version constraint.
    std::map<std::string, std::string> packages;

    /// Packages pulled in as transitive dependencies.
    std::set<std::string> auto_deps;

    /// Parent environment path (for hierarchical resolution).
    std::optional<std::string> origin;
};

/// Encode an environment path to a filesystem-safe slug.
/// "/" -> "ROOT", "/ml" -> "ml", "/work/legacy" -> "work%2Dlegacy"
std::string env_slug(const std::string& env_path);

/// Decode a slug back to an environment path.
std::string slug_to_path(const std::string& slug);

/// Read a manifest from den_home/manifests/<slug>/manifest.json.
/// Returns an empty manifest if the file does not exist.
Manifest read_manifest(const fs::path& den_home, const std::string& env_path);

/// Write a manifest atomically (temp file + rename).
void write_manifest(const fs::path& den_home, const std::string& env_path, const Manifest& m);

/// Lock the manifest file, read it, pass to fn for modification, then
/// write back atomically. Uses flock for concurrency safety.
template <typename F>
auto with_manifest(const fs::path& den_home, const std::string& env_path, F&& fn)
    -> decltype(fn(std::declval<Manifest&>()));

/// Resolve the effective package set by walking the parent chain.
/// Child entries override parent entries.
std::map<std::string, std::string> resolve(const fs::path& den_home, const std::string& env_path);

/// List all environment paths that have manifests.
std::vector<std::string> list_all(const fs::path& den_home);

} // namespace den

// Include the template implementation.
#include "manifest_impl.h"

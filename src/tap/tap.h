// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"
#include "../index/package.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

// Third-party Homebrew tap support (🎯T67).
//
// A tap is a Git repository of formulae named "user/repo" (Homebrew's
// convention). den stores tapped repos under $DEN_HOME/taps/<user>/<repo>/
// and records the registration in config.json so the set survives restarts.
//
// Formulae in a tap are resolved by scanning the tap's Formula/ directory and
// merging the parsed metadata into the package index. The same formula is made
// available under both its bare name ("hello-tap") and its fully-qualified name
// ("user/repo/hello-tap").

// A registered tap.
struct Tap {
    std::string name; // "user/repo"
    std::string url;  // clone source: a git URL or a local path
    fs::path path;    // $DEN_HOME/taps/<user>/<repo>
};

// Validate that `name` is a well-formed "user/repo" tap name. Rejects empty
// components, path-traversal segments, and absolute paths — tap names come from
// untrusted CLI input and are used to build filesystem paths.
bool is_valid_tap_name(const std::string& name);

// Filesystem location of a tap, given its "user/repo" name.
// Returns nullopt for an invalid name.
std::optional<fs::path> tap_dir(const Config& config, const std::string& name);

// Add a tap: clone (git URL) or copy (local path) `source` into the tap dir
// and register it in config.json. Returns the populated Tap on success.
// Throws UserError on a bad name, a missing source, or a clone/copy failure.
Tap tap_add(const Config& config, const std::string& name, const std::string& source);

// List all registered taps, sorted by name.
std::vector<Tap> tap_list(const Config& config);

// Remove a tap: delete its directory and deregister it from config.json.
// Throws UserError if the tap was never added.
void tap_remove(const Config& config, const std::string& name);

// Scan every registered tap's Formula/ directory, parse each formula's
// metadata, and merge the resulting packages into `idx`. Each formula is added
// under both its bare name and its "user/repo/name" qualified name, with
// ruby_source_path pointing at the absolute local .rb file so the source-build
// path reads it without a network fetch. Core packages already in the index are
// never overwritten.
void merge_taps_into_index(const Config& config, PackageIndex& idx);

} // namespace den

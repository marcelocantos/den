// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Read-only Mach-O load-command scan: which paths a binary's dylib load
// commands and rpaths name, and which of a keg's binaries reach into
// Homebrew's prefix. Parses the headers directly (thin and fat files, either
// byte order), so it is cheap enough for `den doctor` to run over a whole
// Cellar and works the same on any host.

#include <filesystem>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

/// The paths named by `file`'s LC_ID_DYLIB, LC_LOAD_DYLIB (and the weak,
/// re-export, lazy and upward variants) and LC_RPATH load commands, across
/// every slice of a fat file, in file order and without duplicates. Empty
/// when `file` is not a readable Mach-O (Java class files, which share the
/// fat magic, included).
std::vector<std::string> macho_load_paths(const fs::path& file);

/// One load path in a keg that reaches into Homebrew's prefix.
struct BrewLinkRef {
    fs::path file;         // the Mach-O file inside the keg
    std::string load;      // the load path as written in the load command
    std::string brew_path; // where it lands under Homebrew's prefix
    std::string formula;   // the Homebrew keg it reaches (opt/<f> or Cellar/<f>), or empty
};

/// Every absolute load path in the Mach-O files under `keg` that lies under
/// `brew_prefix`, either literally or after resolving symlinks (e.g. a
/// `<den prefix>/opt/<f>` link that points at `<brew_prefix>/opt/<f>`).
/// Symlinked files are skipped (their targets are scanned where they live).
std::vector<BrewLinkRef> keg_brew_links(const fs::path& keg, const fs::path& brew_prefix);

/// The distinct `formula` names in `refs` (sorted), for one-line summaries.
std::vector<std::string> brew_link_formulae(const std::vector<BrewLinkRef>& refs);

} // namespace den

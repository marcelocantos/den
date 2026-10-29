// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// One-time relink of kegs den poured before bottles bound their dependencies
// through `<prefix>/opt/<dep>`: Mach-O load commands and rpaths that name a
// dependency's versioned keg (`<Cellar>/<dep>/<version>/...`) are rewritten
// to `<prefix>/opt/<dep>/...`, so a later upgrade of the dependency reaches
// them. `den update` runs it; a second run finds nothing to do.

#include "core/config.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

struct RelinkReport {
    /// "app 1.0 (bin/app: depa 1.0)": kegs whose pins now load through opt.
    std::vector<std::string> relinked;
    /// "app 1.0 -> depa 1.0: <why>": pins left as they were because opt/<dep>
    /// is not den's or does not name the pinned version.
    std::vector<std::string> left_pinned;
    /// "app 1.0: bin/app: <why>": files that could not be rewritten (header
    /// space, signing, verification). The file is unchanged; `den reinstall`
    /// is the fallback.
    std::vector<std::string> failed;
    std::size_t files = 0; ///< Mach-O files rewritten
};

/// Why `<prefix>/opt/<dep>` cannot replace a pin to `<store>/<dep>/<version>`,
/// or empty when it can: the link must be a symlink to exactly that keg, and
/// den must own the keg. Never moves a dependent to another version.
std::string relink_blocker(const fs::path& store, const std::string& dep,
                           const std::string& version);

/// Relink every den-owned keg in config.store whose Mach-O files are pinned
/// to a dependency's versioned keg, where relink_blocker allows it. Each
/// file is rewritten in a copy (install_name_tool, ad-hoc re-sign, checked
/// that every rewritten load resolves and `codesign -v` passes) and renamed
/// over the original, so a running process keeps the old inode and a failure
/// leaves the file untouched. Does nothing when den shares Homebrew's prefix
/// (pins are the guard there) or off macOS (ELF already loads through opt).
/// `install_name_tool` overrides the tool (tests); empty uses the toolchain's.
RelinkReport relink_pinned_kegs(const Config& config, const std::string& install_name_tool = {});

} // namespace den

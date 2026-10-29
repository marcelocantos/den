// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// How a keg's binaries reach the kegs they depend on: through
// `<prefix>/opt/<dep>` (the link a dependency upgrade moves) or pinned to a
// versioned `<Cellar>/<dep>/<version>` path. Mach-O load commands and ELF
// DT_NEEDED/RPATH, parsed directly, so it runs on any host.

#include <filesystem>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

/// A library a keg's binary loads through `<opt_dir>/<dep>`.
struct OptLoad {
    fs::path file;    ///< the binary in the keg
    std::string dep;  ///< the `<dep>` of `<opt_dir>/<dep>`
    std::string load; ///< as written: the Mach-O load path, or the ELF rpath + "/" + DT_NEEDED
    fs::path tail;    ///< where the library has to be inside `<dep>`'s keg
    /// The binary names exactly this path (a Mach-O absolute load), so it
    /// fails to load without it. An `@rpath` or ELF search entry may also be
    /// satisfied by another search directory.
    bool exact = false;
};

/// Every library the binaries under `keg` load through `<opt_dir>/<dep>`:
/// Mach-O dylib loads naming such a path (directly or as `@rpath/...` with an
/// LC_RPATH there), and for ELF each DT_NEEDED paired with each RPATH/RUNPATH
/// there (the caller checks which of the dirs actually held it). Symlinked
/// files are skipped.
std::vector<OptLoad> keg_opt_loads(const fs::path& keg, const fs::path& opt_dir);

/// A load path in a keg's binary pinned to another keg's versioned path.
struct PinnedLoad {
    fs::path file;
    std::string dep;     ///< `<dep>` of `<store>/<dep>/<version>`
    std::string version; ///< `<version>`
    std::string load;    ///< the load path or rpath as written
};

/// Every dylib load and rpath (Mach-O) or RPATH/RUNPATH (ELF) under `keg`
/// that names `<store>/<dep>/<version>/...` for a `<dep>` other than
/// `self_name`.
std::vector<PinnedLoad> keg_pinned_loads(const fs::path& keg, const fs::path& store,
                                         const std::string& self_name);

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Mach-O linkage fix-up for kegs den builds from source.
//
// A source build is configured for <prefix>/opt/<name> and staged into the
// versioned keg <store>/<name>/<version>, so its Mach-O files can name the
// keg through the opt link (which may later point at another version), the
// versioned keg path itself, or a dependency's versioned keg. The fix-up
// walks every Mach-O file in the keg (lib, lib/engines-3, lib/ossl-modules,
// libexec/**, ...) and rewrites:
//
//  - references into the keg (versioned or via its own opt link) to
//    @loader_path-relative paths, so the keg keeps loading its own libraries
//    when the opt link moves to another version;
//  - references to a dependency's versioned keg to <prefix>/opt/<dep>/...;
//  - absolute dylib ids to <prefix>/opt/<name>/..., the stable name that
//    dependents link against;
//  - rpaths the same way, adding an @loader_path rpath where an @rpath load
//    would not otherwise resolve to a library in the keg.
//
// Modified files are re-signed ad hoc and the keg is then verified: every
// non-weak dylib load resolves, no load command names a versioned keg path,
// and every Mach-O file passes `codesign -v`.

#pragma once

#include "macho_scan.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace den {

namespace fs = std::filesystem;

struct MachoFixContext {
    fs::path keg;   ///< <store>/<name>/<version>
    fs::path opt;   ///< <prefix>/opt/<name>
    fs::path store; ///< <prefix>/Cellar (den's store)
    std::string name;
};

/// The context for keg `name` `version` in `store`.
MachoFixContext macho_fix_context(const fs::path& store, const std::string& name,
                                  const std::string& version);

/// The install_name_tool edits one file needs.
struct MachoFixPlan {
    fs::path file;
    std::string new_id;                                             ///< empty: keep
    std::vector<std::pair<std::string, std::string>> changes;       ///< load: old -> new
    std::vector<std::pair<std::string, std::string>> rpath_changes; ///< rpath: old -> new
    std::vector<std::string> delete_rpaths;
    std::vector<std::string> add_rpaths;

    bool empty() const {
        return new_id.empty() && changes.empty() && rpath_changes.empty() &&
               delete_rpaths.empty() && add_rpaths.empty();
    }
};

/// The keg-independent form of an absolute load path or rpath named by
/// `file` (a file inside ctx.keg): @loader_path-relative for the keg itself,
/// <prefix>/opt/<dep>/... for a dependency's versioned keg. Anything else
/// (system paths, @rpath/..., @loader_path/...) is returned unchanged.
std::string fixup_load_path(const std::string& path, const fs::path& file,
                            const MachoFixContext& ctx);

/// What to change in `file` (scanned as `info`).
MachoFixPlan plan_macho_fixup(const fs::path& file, const MachoInfo& info,
                              const MachoFixContext& ctx);

/// Every regular (non-symlink) executable, dylib or bundle Mach-O file under
/// `keg`, sorted.
std::vector<fs::path> keg_macho_files(const fs::path& keg);

/// One verification failure.
struct MachoProblem {
    fs::path file;
    std::string detail;
};

/// Check every Mach-O file in ctx.keg: each non-weak dylib load resolves
/// (dyld shared-cache paths under /usr/lib and /System are taken as
/// resolved), and no load command, id or rpath names a path in ctx.store.
/// With `check_signatures`, also run `codesign -v` on each file.
std::vector<MachoProblem> verify_keg_macho(const MachoFixContext& ctx, bool check_signatures);

/// Plan, apply (install_name_tool), re-sign (codesign -f -s -) and verify
/// every Mach-O file in ctx.keg. Returns how many files were modified.
/// Throws UserError when a rewrite does not fit or verification fails.
/// macOS only: elsewhere it does nothing and returns 0.
std::size_t fixup_keg_macho(const MachoFixContext& ctx);

} // namespace den

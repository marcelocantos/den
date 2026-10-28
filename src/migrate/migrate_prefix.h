// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// `den migrate-prefix`: move a den home that still installs into Homebrew's
// prefix (the pre-isolation layout) into a prefix of its own.
//
// Dry run by default: classify every keg the env manifests own, measure the
// exact bytes the move writes and the free space at the destination, and
// list what else changes (den service specs, the dev.den.daemon plist PATH).
// `--apply` then runs, journalled in <den_home>/migrate-prefix.json:
//
//   1. pour  - pour every keg into <to>/Cellar (DEN_RECEIPT written)
//   2. link  - opt links and cask binary links in <to>
//   3. materialise - every env, in a staging den home, so the root env links
//                    into <to> without touching den's live link ledger
//   4. switch - atomically set store.prefix = <to> (the single switch point)
//   5. finish - move the staged ledger in, update drifted manifest versions,
//               rematerialise named envs, rewrite service specs and the
//               dev.den.daemon plist PATH
//
// A failure before step 4 leaves den exactly as it was. Homebrew's prefix is
// never written: kegs are re-poured, not moved, so brew keeps everything it
// owns there.

#include "../core/config.h"
#include "../index/package.h"
#include "../store/pins.h"
#include "../supervisor/supervisor.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

struct MigratePrefixOptions {
    fs::path to; // empty: <den_home>/prefix (or the journal's target)
    bool apply = false;
    bool resume = false;
    bool rollback = false;
    bool stop_services = false;
    std::set<std::string> only; // pour only these kegs this run
    fs::path home;              // $HOME: LaunchAgents and shell rc files
};

/// Who owns the keg den's manifest names in the old prefix.
enum class KegOwner {
    Den,  // DEN_RECEIPT.json: den poured it
    Brew, // INSTALL_RECEIPT.json: brew poured it
    Unreceipted,
    Missing, // the manifest version is gone (brew upgraded or removed it)
};

enum class KegAction {
    Reinstall, // pour the manifest version into <to>
    Drift,     // manifest version not pourable; pour the index version (newer)
    Pinned,    // pinned; pour exactly the pinned version
    NewDep,    // a dependency the target version needs that no manifest lists
    Done,      // already poured into <to> by an earlier run
    Blocked,   // cannot be moved safely; the switch refuses while any is blocked
};

struct KegPlan {
    std::string entry; // manifest entry (may be tap-qualified)
    std::string name;  // keg name
    std::set<std::string> manifest_versions;
    std::set<std::string> envs; // env paths whose manifests list it
    std::string target;         // version poured into <to>
    int target_scheme = 0;
    KegOwner owner = KegOwner::Missing;
    KegAction action = KegAction::Reinstall;
    std::string reason;            // for Blocked (and Drift detail)
    bool new_dep = false;          // not in any manifest: the target needs it
    std::optional<uint64_t> bytes; // bytes written into <to>; nullopt = unknown
    std::string bytes_source;      // "old keg", "bottle", "already poured"
};

struct CaskRelink {
    std::string token;
    fs::path link;   // new link in <to>/bin
    fs::path target; // caskroom target
};

struct ServiceRewrite {
    std::string name;
    bool changes = false; // argv/env/working_dir name paths under the old prefix
};

struct MigratePrefixPlan {
    fs::path from; // old prefix (Homebrew's)
    fs::path to;
    std::vector<KegPlan> kegs;
    std::vector<std::string> casks; // manifest casks (stay under den's caskroom)
    std::vector<CaskRelink> cask_relinks;
    std::vector<ServiceRewrite> services;
    std::vector<std::string> brew_plists; // homebrew.mxcl.* labels (report only)
    std::optional<fs::path> daemon_plist;
    std::string daemon_path_before;
    std::string daemon_path_after;
    std::vector<std::pair<fs::path, int>> shell_rc_init_lines; // files with >1 `den init`
    uint64_t total_bytes = 0;
    bool total_known = true;
    std::optional<uint64_t> free_bytes;
    fs::path free_at;

    std::size_t count(KegAction a) const;
};

/// Persistent state of an --apply run (<den_home>/migrate-prefix.json).
struct MigratePrefixJournal {
    struct ManifestChange {
        std::string env;
        std::string entry;
        std::string from; // empty: the entry was absent (a new dependency)
        std::string to;
    };
    std::string to;
    std::string from;
    std::string phase;               // pour, link, materialise, switched, done, rolled_back
    std::vector<std::string> done;   // "name/version" poured with a receipt
    std::vector<std::string> failed; // "phase: item: error"
    bool previous_prefix_recorded = false;
    std::optional<std::string> previous_prefix; // store.prefix before the switch
    std::vector<ManifestChange> manifest_changes;
    bool ledger_swapped = false;
    std::vector<std::string> specs_rewritten;
    bool plist_rewritten = false;
};

fs::path migrate_prefix_journal_path(const fs::path& den_home);
std::optional<MigratePrefixJournal> read_migrate_prefix_journal(const fs::path& den_home);
void write_migrate_prefix_journal(const fs::path& den_home, const MigratePrefixJournal& j);

/// Classify and measure. `measure` downloads bottles that are neither in
/// den's download cache nor present at the same version in the old Cellar,
/// so their exact unpacked size is known (into the cache only).
MigratePrefixPlan plan_migrate_prefix(const Config& cfg, const PackageIndex& idx,
                                      const fs::path& to, const fs::path& home, bool measure);

void print_migrate_prefix_plan(std::ostream& out, const MigratePrefixPlan& plan);

/// Run the command. Returns the process exit code.
int run_migrate_prefix(const Config& cfg, const MigratePrefixOptions& opts, std::ostream& out);

// ---- Helpers exposed for tests -------------------------------------------

/// `s` with a leading `from` path (whole components) replaced by `to`.
/// Handles colon-separated lists and `--flag=<path>` values.
std::string rebase_prefix_paths(const std::string& s, const fs::path& from, const fs::path& to);

/// The spec with every path under `from` rebased onto `to`.
ServiceSpec rebase_service_spec(const ServiceSpec& spec, const fs::path& from, const fs::path& to);

/// The plist text with `<to>/bin` put at the front of its PATH value, or
/// nullopt when it has no PATH. Idempotent.
std::optional<std::string> plist_with_prefix_path(const std::string& plist, const fs::path& to);

/// Uncompressed size of the regular files in a bottle archive.
uint64_t bottle_unpacked_size(const fs::path& archive);

/// Free bytes at `p` (or its nearest existing ancestor, returned in `at`).
/// DEN_MIGRATE_PREFIX_FREE_BYTES overrides the answer (tests).
std::optional<uint64_t> free_bytes_at(const fs::path& p, fs::path* at = nullptr);

} // namespace den

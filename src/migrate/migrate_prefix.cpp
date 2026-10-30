// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "migrate_prefix.h"

#include "../build/macho_scan.h"
#include "../cask/cask.h"
#include "../core/error.h"
#include "../core/version.h"
#include "../daemon/daemon.h"
#include "../env/environment.h"
#include "../env/manifest.h"
#include "../index/bottle_cellar.h"
#include "../index/index.h"
#include "../platform/platform.h"
#include "../provider/homebrew_provider.h"
#include "../selfupdate/daemon_restart.h"
#include "../selfupdate/launchd_restart.h"
#include "../settings/settings.h"
#include "../store/link.h"
#include "../store/store.h"
#include "migrate.h"

#include <archive.h>
#include <archive_entry.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <thread>

namespace den {

namespace {

using json = nlohmann::json;

constexpr const char* kJournalFile = "migrate-prefix.json";
constexpr const char* kWorkDir = "migrate-prefix";
constexpr const char* kDaemonPlist = "dev.den.daemon.plist";
const std::vector<std::string> kLedgerSubdirs = {"prefix-links", "prefix-linked"};

std::string human_bytes(uint64_t bytes) {
    constexpr uint64_t kKiB = 1024;
    if (bytes < kKiB) {
        return std::to_string(bytes) + " B";
    }
    constexpr const char* kUnits[] = {"KB", "MB", "GB", "TB", "PB"};
    double value = static_cast<double>(bytes);
    int unit = -1;
    do {
        value /= static_cast<double>(kKiB);
        ++unit;
    } while (value >= static_cast<double>(kKiB) && unit + 1 < static_cast<int>(std::size(kUnits)));
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value << " " << kUnits[unit];
    return out.str();
}

std::string bytes_label(uint64_t bytes) {
    return human_bytes(bytes) + " (" + std::to_string(bytes) + " bytes)";
}

/// Test-only fault injection: DEN_MIGRATE_PREFIX_FAULT names a stage
/// ("pour:<keg>", "link", "materialise", "switch", "finish") to fail at.
void maybe_fault(const std::string& stage) {
    if (const char* f = std::getenv("DEN_MIGRATE_PREFIX_FAULT"); f && stage == f) {
        throw Error("injected migrate-prefix fault at '" + stage + "'");
    }
}

bool path_within(const fs::path& p, const fs::path& root) {
    auto a = p.lexically_normal();
    auto b = root.lexically_normal();
    auto ai = a.begin();
    for (auto bi = b.begin(); bi != b.end(); ++bi) {
        if (bi->empty()) {
            continue; // trailing slash
        }
        if (ai == a.end() || *ai != *bi) {
            return false;
        }
        ++ai;
    }
    return true;
}

fs::path normalised(const fs::path& p) {
    std::error_code ec;
    auto w = fs::weakly_canonical(fs::absolute(p), ec);
    return (ec ? fs::absolute(p) : w).lexically_normal();
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void write_text_atomic(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << text;
        f.flush();
        if (!f) {
            throw Error("cannot write " + tmp.string());
        }
    }
    fs::rename(tmp, p);
}

bool is_cask_package(const Package& p) {
    return p.artifact_type == ArtifactType::App || !p.cask_artifacts.empty();
}

const char* owner_label(KegOwner o) {
    switch (o) {
    case KegOwner::Den:
        return "den-poured";
    case KegOwner::Brew:
        return "brew receipt";
    case KegOwner::Unreceipted:
        return "unreceipted";
    case KegOwner::Missing:
        return "missing";
    }
    return "?";
}

KegOwner keg_owner(const fs::path& keg) {
    std::error_code ec;
    if (!fs::is_directory(keg, ec)) {
        return KegOwner::Missing;
    }
    if (keg_owned_by_den(keg)) {
        return KegOwner::Den;
    }
    if (fs::exists(keg / "INSTALL_RECEIPT.json", ec)) {
        return KegOwner::Brew;
    }
    return KegOwner::Unreceipted;
}

fs::path daemon_plist_path(const fs::path& home) {
    return home / "Library" / "LaunchAgents" / kDaemonPlist;
}

std::string plist_path_value(const std::string& plist) {
    auto key = plist.find("<key>PATH</key>");
    if (key == std::string::npos) {
        return {};
    }
    auto open = plist.find("<string>", key);
    auto close = open == std::string::npos ? open : plist.find("</string>", open);
    if (close == std::string::npos) {
        return {};
    }
    open += std::string_view("<string>").size();
    return plist.substr(open, close - open);
}

std::vector<std::string> spec_names(const Config& cfg) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(services_root(cfg), ec)) {
        if (fs::is_regular_file(e.path() / "spec.json", ec)) {
            names.push_back(e.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool spec_changes(const ServiceSpec& a, const ServiceSpec& b) {
    return a.argv != b.argv || a.env != b.env || a.working_dir != b.working_dir;
}

int count_den_init_lines(const fs::path& rc) {
    std::ifstream f(rc);
    std::string line;
    int n = 0;
    while (std::getline(f, line)) {
        if (profile_has_den_init(line + "\n")) {
            ++n;
        }
    }
    return n;
}

/// Version strings the manifests name for each keg, and where.
struct Owned {
    std::string entry;
    std::set<std::string> versions;
    std::set<std::string> envs;
};

std::map<std::string, Owned> owned_kegs(const fs::path& den_home) {
    std::map<std::string, Owned> out;
    for (const auto& env : list_all(den_home)) {
        auto m = read_manifest(den_home, env);
        auto bucket = m.packages["homebrew"];
        for (const auto& [entry, ver] : bucket) {
            auto& o = out[keg_name(entry)];
            o.entry = entry;
            o.versions.insert(ver);
            o.envs.insert(env);
        }
        for (const auto& entry : m.auto_deps["homebrew"]) {
            if (!bucket.count(entry)) {
                auto& o = out[keg_name(entry)];
                o.entry = entry;
                o.envs.insert(env);
            }
        }
    }
    return out;
}

/// The fixed Cellar `pkg`'s best bottle was built for when that is why it
/// cannot pour into `store`, else empty.
std::string fixed_cellar_for(const Config& cfg, const Package& pkg, const fs::path& store) {
    std::vector<std::string> tags;
    for (const auto& [tag, _] : pkg.archives) {
        tags.push_back(tag);
    }
    auto best = best_archive_tag(cfg.arch, cfg.macos_version, tags);
    if (!best) {
        return {};
    }
    const auto& archive = pkg.archives.at(*best);
    if (bottle_pours_into(*best, archive, store)) {
        return {};
    }
    return required_bottle_cellar(*best, archive);
}

std::string normal_dir(const fs::path& p) {
    auto s = p.lexically_normal().string();
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

/// A keg whose bottle only pours into `cellar` keeps using Homebrew's copy
/// when that is Homebrew's Cellar here, brew is installed, and brew's Cellar
/// holds a keg of it brew owns. Sets `k` to BrewOwned, or extends its
/// Blocked reason with why not (never a silent dependency).
void keep_with_brew(const Config& cfg, const std::string& cellar, KegPlan& k) {
    std::error_code ec;
    const fs::path brew_cellar =
        cfg.homebrew_cellar.empty() ? cfg.homebrew_prefix / "Cellar" : cfg.homebrew_cellar;
    if (cfg.homebrew_prefix.empty() || normal_dir(cellar) != normal_dir(brew_cellar)) {
        k.reason += " (not Homebrew's Cellar here, so brew cannot keep it)";
        return;
    }
    const auto brew = cfg.homebrew_prefix / "bin" / "brew";
    if (!fs::exists(brew, ec)) {
        k.reason +=
            "; Homebrew is not installed (no " + brew.string() + "), so nothing would provide it";
        return;
    }
    // Prefer the manifest version brew's opt link selects, then any manifest
    // version brew still has, then whatever brew's opt link selects.
    std::string opt_version;
    if (auto opt = fs::canonical(cfg.homebrew_prefix / "opt" / k.name, ec); !ec) {
        std::error_code cec;
        auto canon_cellar = fs::canonical(brew_cellar / k.name, cec);
        if (!cec && opt.parent_path() == canon_cellar) {
            opt_version = opt.filename().string();
        }
    }
    std::vector<std::string> candidates;
    if (!opt_version.empty() && k.manifest_versions.count(opt_version)) {
        candidates.push_back(opt_version);
    }
    for (auto it = k.manifest_versions.rbegin(); it != k.manifest_versions.rend(); ++it) {
        candidates.push_back(*it);
    }
    if (!opt_version.empty()) {
        candidates.push_back(opt_version);
    }
    bool den_poured = false;
    for (const auto& v : candidates) {
        const auto keg = brew_cellar / k.name / v;
        const auto owner = keg_owner(keg);
        if (owner == KegOwner::Missing) {
            continue;
        }
        if (owner == KegOwner::Den) {
            den_poured = true; // `den cleanup --prefix` reclaims these: not brew's to keep
            continue;
        }
        k.action = KegAction::BrewOwned;
        k.target = v;
        k.owner = owner;
        k.brew_keg = keg;
        k.reason = "bottle only pours into " + cellar + "; stays with brew";
        return;
    }
    k.reason += den_poured ? "; the only " + k.name + " keg in " + brew_cellar.string() +
                                 " was poured by den, so brew does not own it"
                           : "; " + brew_cellar.string() + " has no " + k.name +
                                 " keg for brew to keep providing";
}

/// Why `pkg` cannot be poured into `store`, or empty.
std::string pour_problem(const Config& cfg, const Package& pkg, const fs::path& store) {
    if (pkg.archives.empty()) {
        if (pkg.url_install && pkg.url_install->refusal.empty()) {
            return {};
        }
        return "no bottle in den's index (built from source); reinstall it with "
               "`den install -s " +
               pkg.name + "` after the move, or remove it first";
    }
    std::vector<std::string> tags;
    for (const auto& [tag, _] : pkg.archives) {
        tags.push_back(tag);
    }
    auto best = best_archive_tag(cfg.arch, cfg.macos_version, tags);
    if (!best) {
        return "no bottle for this platform";
    }
    const auto& archive = pkg.archives.at(*best);
    if (!bottle_pours_into(*best, archive, store)) {
        return "its bottle only pours into " + required_bottle_cellar(*best, archive);
    }
    return {};
}

std::optional<std::pair<uint64_t, std::string>> measure_keg(const Config& cfg_to,
                                                            const Package& pkg,
                                                            const fs::path& old_same_version,
                                                            bool measure) {
    std::error_code ec;
    if (fs::is_directory(old_same_version, ec)) {
        return std::make_pair(keg_disk_usage(old_same_version), std::string("old keg"));
    }
    if (!measure) {
        return std::nullopt;
    }
    try {
        if (auto archive = prefetch_bottle(cfg_to, pkg)) {
            return std::make_pair(bottle_unpacked_size(*archive), std::string("bottle"));
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("could not measure {}: {}", pkg.name, e.what());
    }
    return std::nullopt;
}

fs::path work_dir(const fs::path& den_home) {
    return den_home / kWorkDir;
}
fs::path stage_home(const fs::path& den_home) {
    return work_dir(den_home) / "stage";
}

Config config_for(const Config& cfg, const fs::path& prefix) {
    Config c = cfg;
    c.set_prefix(prefix, PrefixSource::Settings);
    return c;
}

// ---- Services -------------------------------------------------------------

struct RunningSet {
    std::vector<std::string> services;
    bool daemon = false;
    bool any() const { return daemon || !services.empty(); }
};

RunningSet running_services(const Config& cfg) {
    RunningSet r;
    for (const auto& s : list_services(cfg)) {
        if (s.running) {
            r.services.push_back(s.name);
        }
    }
    r.daemon = is_daemon_running(cfg);
    return r;
}

constexpr const char* kDaemonLabel = "dev.den.daemon";

LaunchdOps daemon_ops(const MigratePrefixOptions& opts, const fs::path& plist) {
    return opts.daemon_launchd_ops ? opts.daemon_launchd_ops(plist)
                                   : real_launchd_ops(kDaemonLabel, plist);
}

struct Stopped {
    std::vector<std::string> services;
    bool daemon = false;
    bool daemon_launchd = false;
};

Stopped stop_running(const Config& cfg, const RunningSet& running, const MigratePrefixOptions& opts,
                     std::ostream& out) {
    Stopped s;
    if (running.daemon) {
        // Refuse before touching anything: booting out dev.den.daemon from
        // inside its own job would kill this process mid-migration.
        std::error_code ec;
        if (fs::is_regular_file(daemon_plist_path(opts.home), ec) &&
            running_inside_launchd_job(kDaemonLabel)) {
            throw UserError("migrate-prefix --stop-services cannot stop dev.den.daemon from "
                            "inside its own launchd job; run it from a shell. Nothing was "
                            "changed");
        }
    }
    if (!running.services.empty()) {
        const auto stopped = stop_services_ordered(cfg, running.services);
        bool all = true;
        for (const auto& name : running.services) {
            if (std::find(stopped.begin(), stopped.end(), name) != stopped.end()) {
                s.services.push_back(name);
                out << "stopped    service " << name << "\n";
            } else {
                all = false;
            }
        }
        if (!all) {
            throw UserError("could not stop service(s) before migrating; nothing was changed");
        }
    }
    if (running.daemon) {
        const auto plist = daemon_plist_path(opts.home);
        std::error_code ec;
        if (fs::is_regular_file(plist, ec)) {
            bootout_migrated_daemon(daemon_ops(opts, plist));
            s.daemon_launchd = true;
        } else {
            stop_daemon(cfg);
        }
        for (int i = 0; i < 100 && is_daemon_running(cfg); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (is_daemon_running(cfg)) {
            throw UserError("the den daemon did not stop; nothing was changed");
        }
        s.daemon = true;
        out << "stopped    dev.den.daemon\n";
    }
    return s;
}

void restart_stopped(const Config& cfg, const Stopped& s, const MigratePrefixOptions& opts,
                     std::ostream& out) {
    for (const auto& name : s.services) {
        auto spec = resolve_service_spec(cfg, name);
        if (!spec) {
            spec = load_spec(cfg, name);
        }
        if (spec && start_service(cfg, *spec)) {
            out << "started    service " << name << "\n";
        } else {
            out << "warning: could not restart service " << name << "; run `den services start "
                << name << "`\n";
        }
    }
    if (s.daemon) {
        if (s.daemon_launchd) {
            (void)restart_migrated_daemon(daemon_ops(opts, daemon_plist_path(opts.home)), out);
        } else {
            out << "note: the den daemon was stopped; start it again with `den daemon run`\n";
        }
    }
}

} // namespace

void bootout_migrated_daemon(const LaunchdOps& ops) {
    (void)ops.launchctl({"bootout", ops.domain + "/" + ops.label});
}

bool restart_migrated_daemon(const LaunchdOps& ops, std::ostream& out) {
    // The job was booted out by stop_running, so there is no old pid: any
    // running pid confirms the restart.
    std::ostringstream detail;
    const auto r = restart_launchd_job(ops, std::nullopt, detail);
    out << detail.str();
    if (r == LaunchdRestartResult::Failed) {
        out << "warning: could not restart dev.den.daemon; run `den daemon install`\n";
        return false;
    }
    out << "started    dev.den.daemon\n";
    return true;
}

namespace {

// ---- Journal (de)serialisation ---------------------------------------------

json to_json(const MigratePrefixJournal& j) {
    json o;
    o["to"] = j.to;
    o["from"] = j.from;
    o["phase"] = j.phase;
    o["done"] = j.done;
    o["failed"] = j.failed;
    if (j.previous_prefix_recorded) {
        o["previous_prefix"] = j.previous_prefix ? json(*j.previous_prefix) : json(nullptr);
    }
    json changes = json::array();
    for (const auto& c : j.manifest_changes) {
        changes.push_back({{"env", c.env}, {"entry", c.entry}, {"from", c.from}, {"to", c.to}});
    }
    o["manifest_changes"] = changes;
    o["ledger_swapped"] = j.ledger_swapped;
    o["pouring"] = j.pouring;
    o["specs_rewritten"] = j.specs_rewritten;
    o["plist_rewritten"] = j.plist_rewritten;
    return o;
}

// ---- Planned manifest changes ---------------------------------------------

std::vector<MigratePrefixJournal::ManifestChange>
planned_manifest_changes(const fs::path& den_home, const MigratePrefixPlan& plan) {
    std::map<std::string, const KegPlan*> by_name;
    for (const auto& k : plan.kegs) {
        by_name[k.name] = &k;
    }
    std::vector<MigratePrefixJournal::ManifestChange> out;
    for (const auto& env : list_all(den_home)) {
        auto m = read_manifest(den_home, env);
        auto& bucket = m.packages["homebrew"];
        for (const auto& [entry, ver] : bucket) {
            auto it = by_name.find(keg_name(entry));
            if (it != by_name.end() && it->second->action != KegAction::BrewOwned &&
                it->second->target != ver) {
                out.push_back({env, entry, ver, it->second->target});
            }
        }
        for (const auto& k : plan.kegs) {
            if (k.new_dep && k.action != KegAction::BrewOwned && k.envs.count(env) &&
                !bucket.count(k.entry)) {
                out.push_back({env, k.entry, "", k.target});
            }
        }
    }
    return out;
}

void apply_manifest_changes(const fs::path& den_home,
                            const std::vector<MigratePrefixJournal::ManifestChange>& changes,
                            bool forward) {
    std::map<std::string, std::vector<const MigratePrefixJournal::ManifestChange*>> per_env;
    for (const auto& c : changes) {
        per_env[c.env].push_back(&c);
    }
    for (const auto& [env, list] : per_env) {
        with_manifest(den_home, env, [&](Manifest& m) {
            auto& bucket = m.packages["homebrew"];
            auto& autos = m.auto_deps["homebrew"];
            for (const auto* c : list) {
                if (forward) {
                    bucket[c->entry] = c->to;
                    if (c->from.empty()) {
                        autos.insert(c->entry);
                    }
                } else if (c->from.empty()) {
                    bucket.erase(c->entry);
                    autos.erase(c->entry);
                } else {
                    bucket[c->entry] = c->from;
                }
            }
        });
    }
}

/// Copy the manifests into the staging den home with the planned changes.
void build_stage(const fs::path& den_home,
                 const std::vector<MigratePrefixJournal::ManifestChange>& changes) {
    const auto stage = stage_home(den_home);
    std::error_code ec;
    fs::remove_all(stage, ec);
    fs::create_directories(stage / "manifests");
    fs::copy(den_home / "manifests", stage / "manifests",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    apply_manifest_changes(stage, changes, true);
}

void copy_tree(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::remove_all(dst, ec);
    if (!fs::exists(src, ec)) {
        return;
    }
    fs::create_directories(dst.parent_path());
    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
}

void swap_ledger_in(const fs::path& den_home) {
    const auto live = den_home / "envs" / "ROOT" / ".den";
    const auto backup = work_dir(den_home) / "ledger-old";
    const auto staged = stage_home(den_home) / "envs" / "ROOT" / ".den";
    std::error_code ec;
    fs::create_directories(backup);
    for (const auto& sub : kLedgerSubdirs) {
        if (!fs::exists(backup / sub, ec)) {
            if (fs::exists(live / sub, ec)) {
                fs::rename(live / sub, backup / sub);
            } else {
                fs::create_directories(backup / sub); // records "was empty"
            }
        }
        copy_tree(staged / sub, live / sub);
    }
}

void swap_ledger_out(const fs::path& den_home) {
    const auto live = den_home / "envs" / "ROOT" / ".den";
    const auto backup = work_dir(den_home) / "ledger-old";
    std::error_code ec;
    for (const auto& sub : kLedgerSubdirs) {
        if (!fs::exists(backup / sub, ec)) {
            continue;
        }
        fs::remove_all(live / sub, ec);
        fs::create_directories(live);
        fs::rename(backup / sub, live / sub);
    }
}

/// Point envs/ROOT's compat links (bin -> <prefix>/bin, ...) from `from` to `to`.
void repoint_root_shims(const fs::path& den_home, const fs::path& from, const fs::path& to) {
    const auto dir = den_home / "envs" / "ROOT";
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!fs::is_symlink(e.symlink_status(ec))) {
            continue;
        }
        auto target = fs::read_symlink(e.path(), ec);
        if (ec || !path_within(target, from)) {
            continue;
        }
        auto rel = target.lexically_normal().lexically_relative(from.lexically_normal());
        auto replacement = (to / rel).lexically_normal();
        fs::remove(e.path(), ec);
        fs::create_directory_symlink(replacement, e.path(), ec);
    }
}

void print_failure(std::ostream& out, const std::string& phase, const std::string& what,
                   const fs::path& from, bool switched) {
    out << "\nmigrate-prefix failed during " << phase << ": " << what << "\n";
    if (switched) {
        out << "den already uses the new prefix; finish with `den migrate-prefix --resume`, or "
               "undo with `den migrate-prefix --rollback`.\n";
    } else {
        out << "den is unchanged and still uses " << from.string()
            << ". Fix the problem and continue with `den migrate-prefix --resume`, or undo "
               "with `den migrate-prefix --rollback`.\n";
    }
}

} // namespace

// ---- Public helpers ---------------------------------------------------------

std::size_t MigratePrefixPlan::count(KegAction a) const {
    return static_cast<std::size_t>(
        std::count_if(kegs.begin(), kegs.end(), [a](const KegPlan& k) { return k.action == a; }));
}

fs::path migrate_prefix_journal_path(const fs::path& den_home) {
    return den_home / kJournalFile;
}

std::optional<MigratePrefixJournal> read_migrate_prefix_journal(const fs::path& den_home) {
    const auto p = migrate_prefix_journal_path(den_home);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
        return std::nullopt;
    }
    std::ifstream f(p);
    auto o = json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (!o.is_object()) {
        throw UserError(p.string() + " is corrupt; fix or remove it before running "
                                     "`den migrate-prefix` again");
    }
    MigratePrefixJournal j;
    j.to = o.value("to", "");
    j.from = o.value("from", "");
    j.phase = o.value("phase", "");
    j.done = o.value("done", std::vector<std::string>{});
    j.failed = o.value("failed", std::vector<std::string>{});
    if (o.contains("previous_prefix")) {
        j.previous_prefix_recorded = true;
        if (o["previous_prefix"].is_string()) {
            j.previous_prefix = o["previous_prefix"].get<std::string>();
        }
    }
    if (o.contains("manifest_changes") && o["manifest_changes"].is_array()) {
        for (const auto& c : o["manifest_changes"]) {
            j.manifest_changes.push_back(
                {c.value("env", ""), c.value("entry", ""), c.value("from", ""), c.value("to", "")});
        }
    }
    j.ledger_swapped = o.value("ledger_swapped", false);
    j.pouring = o.value("pouring", std::string{});
    j.specs_rewritten = o.value("specs_rewritten", std::vector<std::string>{});
    j.plist_rewritten = o.value("plist_rewritten", false);
    return j;
}

void write_migrate_prefix_journal(const fs::path& den_home, const MigratePrefixJournal& j) {
    write_text_atomic(migrate_prefix_journal_path(den_home), to_json(j).dump(2) + "\n");
}

std::string rebase_prefix_paths(const std::string& s, const fs::path& from, const fs::path& to) {
    const std::string f = from.lexically_normal().string();
    const std::string t = to.lexically_normal().string();
    auto one = [&](const std::string& seg) -> std::string {
        auto rebase = [&](const std::string& v) -> std::string {
            if (v == f) {
                return t;
            }
            if (v.size() > f.size() && v.compare(0, f.size(), f) == 0 && v[f.size()] == '/') {
                return t + v.substr(f.size());
            }
            return v;
        };
        auto eq = seg.find('=');
        if (eq != std::string::npos && !seg.empty() && seg[0] != '/') {
            return seg.substr(0, eq + 1) + rebase(seg.substr(eq + 1));
        }
        return rebase(seg);
    };
    std::string out;
    std::size_t start = 0;
    while (true) {
        auto colon = s.find(':', start);
        out += one(s.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
        if (colon == std::string::npos) {
            break;
        }
        out += ':';
        start = colon + 1;
    }
    return out;
}

ServiceSpec rebase_service_spec(const ServiceSpec& spec, const fs::path& from, const fs::path& to) {
    ServiceSpec out = spec;
    for (auto& a : out.argv) {
        a = rebase_prefix_paths(a, from, to);
    }
    for (auto& [k, v] : out.env) {
        v = rebase_prefix_paths(v, from, to);
    }
    if (!out.working_dir.empty()) {
        out.working_dir = rebase_prefix_paths(out.working_dir, from, to);
    }
    return out;
}

std::optional<std::string> plist_with_prefix_path(const std::string& plist, const fs::path& to) {
    auto key = plist.find("<key>PATH</key>");
    if (key == std::string::npos) {
        return std::nullopt;
    }
    auto open = plist.find("<string>", key);
    if (open == std::string::npos) {
        return std::nullopt;
    }
    open += std::string_view("<string>").size();
    auto close = plist.find("</string>", open);
    if (close == std::string::npos) {
        return std::nullopt;
    }
    const std::string value = plist.substr(open, close - open);
    const std::string bin = (to / "bin").string();
    if (value == bin || value.starts_with(bin + ":")) {
        return plist;
    }
    return plist.substr(0, open) + bin + (value.empty() ? "" : ":" + value) + plist.substr(close);
}

uint64_t bottle_unpacked_size(const fs::path& archive_path) {
    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, archive_path.c_str(), 64 * 1024) != ARCHIVE_OK) {
        std::string err = archive_error_string(a) ? archive_error_string(a) : "unknown error";
        archive_read_free(a);
        throw Error("cannot read " + archive_path.string() + ": " + err);
    }
    uint64_t total = 0;
    struct archive_entry* e = nullptr;
    while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
        if (archive_entry_filetype(e) == AE_IFREG && archive_entry_size_is_set(e)) {
            total += static_cast<uint64_t>(archive_entry_size(e));
        }
        archive_read_data_skip(a);
    }
    archive_read_free(a);
    return total;
}

std::optional<uint64_t> free_bytes_at(const fs::path& p, fs::path* at) {
    fs::path probe = p;
    std::error_code ec;
    while (!probe.empty() && !fs::exists(probe, ec)) {
        auto parent = probe.parent_path();
        if (parent == probe) {
            break;
        }
        probe = parent;
    }
    if (at) {
        *at = probe;
    }
    if (const char* o = std::getenv("DEN_MIGRATE_PREFIX_FREE_BYTES"); o && *o) {
        return std::stoull(o);
    }
    struct statvfs st {};
    if (::statvfs(probe.c_str(), &st) != 0) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(st.f_bavail) * static_cast<uint64_t>(st.f_frsize);
}

// ---- Plan -------------------------------------------------------------------

namespace {

// A keg in <to> counts as moved only when den's receipt names it, it holds
// more than that receipt, and <to>/opt/<name> resolves to it. Anything less
// is a half-finished pour that must be redone, never trusted.
std::string keg_problem(const fs::path& store, const std::string& name,
                        const std::string& version) {
    const auto keg = store / name / version;
    std::error_code ec;
    if (!keg_owned_by_den(keg)) {
        return keg.string() + " has no matching " + std::string(kDenReceiptFilename);
    }
    bool content = false;
    for (const auto& e : fs::directory_iterator(keg, ec)) {
        if (e.path().filename() != kDenReceiptFilename) {
            content = true;
            break;
        }
    }
    if (!content) {
        return keg.string() + " is empty";
    }
    const auto opt = store.parent_path() / "opt" / name;
    if (!fs::is_symlink(fs::symlink_status(opt, ec)) || !fs::exists(opt, ec) ||
        !fs::equivalent(opt, keg, ec)) {
        return opt.string() + " does not point at " + keg.string();
    }
    return {};
}

// Undo one keg's pour inside <to>: the keg, its opt link (only if it points
// at this keg) and the rack if that leaves it empty. <to> is not live before
// the switch, so nothing den uses can depend on what this removes.
void discard_keg(const fs::path& store, const std::string& name, const std::string& version) {
    const auto keg = store / name / version;
    std::error_code ec;
    const auto opt = store.parent_path() / "opt" / name;
    if (fs::is_symlink(fs::symlink_status(opt, ec))) {
        auto target = fs::read_symlink(opt, ec);
        if (!ec) {
            auto resolved = target.is_absolute() ? target : (opt.parent_path() / target);
            if (resolved.lexically_normal() == keg.lexically_normal() ||
                (fs::exists(keg, ec) && fs::equivalent(resolved, keg, ec))) {
                fs::remove(opt, ec);
            }
        }
    }
    fs::remove_all(keg, ec);
    if (fs::is_empty(store / name, ec)) {
        fs::remove(store / name, ec);
    }
}

/// `<to>/opt/<name>` -> `<brew_prefix>/opt/<name>`, so kegs den pours that
/// load a brew-owned dependency through den's opt link reach brew's copy (and
/// follow brew's upgrades of it). Only ever writes inside <to>; an existing
/// entry is left alone.
void link_brew_owned(const fs::path& to, const fs::path& brew_prefix, const std::string& name,
                     std::ostream& out) {
    std::error_code ec;
    const auto link = to / "opt" / name;
    const auto target = brew_prefix / "opt" / name;
    const auto st = fs::symlink_status(link, ec);
    if (!fs::exists(st)) {
        fs::create_directories(link.parent_path());
        fs::create_symlink(target, link);
        out << "linked     " << link.string() << " -> " << target.string() << " (brew-owned)\n";
    } else if (!fs::is_symlink(st) || fs::read_symlink(link, ec) != target) {
        out << "kept       " << link.string() << " (already there; not repointed at "
            << target.string() << ")\n";
    }
}

} // namespace

std::string keg_display_version(const KegPlan& k) {
    std::string v;
    for (const auto& s : k.manifest_versions) {
        v += (v.empty() ? "" : ",") + s;
    }
    if (v.empty()) {
        v = k.target;
    }
    return v.empty() ? "(no version recorded)" : v;
}

MigratePrefixPlan plan_migrate_prefix(const Config& cfg, const PackageIndex& idx,
                                      const fs::path& to, const fs::path& home, bool measure) {
    MigratePrefixPlan plan;
    plan.from = cfg.prefix;
    plan.to = to;
    plan.brew_prefix = cfg.homebrew_prefix;
    const auto from_cellar = cfg.prefix / "Cellar";
    const auto to_cellar = to / "Cellar";
    const Config cfg_to = config_for(cfg, to);
    const PinMap pins = read_pins(cfg.den_home); // throws on a corrupt file: fail closed

    auto owned = owned_kegs(cfg.den_home);
    std::map<std::string, KegPlan> kegs;
    std::string pouring; // a keg an interrupted run was pouring is never "done"
    if (auto jr = read_migrate_prefix_journal(cfg.den_home); jr && jr->to == to.string()) {
        pouring = jr->pouring;
    }

    auto classify = [&](const std::string& name, const Owned& o, bool new_dep) {
        KegPlan k;
        k.entry = o.entry;
        k.name = name;
        k.manifest_versions = o.versions;
        k.envs = o.envs;
        k.new_dep = new_dep;
        const Package* pkg = idx.find(o.entry);
        if (!pkg) {
            k.action = KegAction::Blocked;
            k.reason = "not in den's index; run `den update`";
            k.owner = KegOwner::Missing;
            for (const auto& v : o.versions) {
                if (keg_owner(from_cellar / name / v) != KegOwner::Missing) {
                    k.owner = keg_owner(from_cellar / name / v);
                }
            }
            kegs[name] = k;
            return;
        }
        k.target = pkg->version;
        k.target_scheme = pkg->version_scheme;
        k.owner = KegOwner::Missing;
        for (const auto& v : o.versions) {
            auto owner = keg_owner(from_cellar / name / v);
            if (owner != KegOwner::Missing) {
                k.owner = owner;
                break;
            }
        }

        k.action = new_dep ? KegAction::NewDep : KegAction::Reinstall;
        if (auto pin = pins.find(name); pin != pins.end()) {
            if (pin->second != k.target) {
                k.action = KegAction::Blocked;
                k.reason = "pinned at " + pin->second + " but den's index only offers " + k.target +
                           "; pins are kept, so unpin it (`den unpin " + name +
                           "`) or wait for the index";
            } else {
                k.action = KegAction::Pinned;
            }
        }
        if (k.action == KegAction::Reinstall) {
            for (const auto& v : o.versions) {
                if (v == k.target) {
                    continue;
                }
                const auto old_keg = from_cellar / name / v;
                const int scheme =
                    fs::is_directory(old_keg) ? installed_version_scheme(old_keg) : 0;
                const auto delta = version_delta(v, scheme, k.target, k.target_scheme);
                if (delta == VersionDelta::Older) {
                    k.action = KegAction::Blocked;
                    k.reason = "den's index offers " + k.target + ", older than the installed " +
                               v + "; den never downgrades";
                    break;
                }
                if (delta == VersionDelta::Held) {
                    k.action = KegAction::Blocked;
                    k.reason = version_hold_reason(v, k.target, k.target_scheme) +
                               "; den never downgrades";
                    break;
                }
                k.action = KegAction::Drift;
            }
        }
        if (k.action != KegAction::Blocked) {
            if (auto problem = pour_problem(cfg, *pkg, to_cellar); !problem.empty()) {
                k.action = KegAction::Blocked;
                k.reason = problem;
                if (auto cellar = fixed_cellar_for(cfg, *pkg, to_cellar);
                    !cellar.empty() && !pins.count(name)) {
                    keep_with_brew(cfg, cellar, k);
                }
            }
        }
        if (k.action == KegAction::BrewOwned) {
            kegs[name] = k;
            return;
        }
        if (k.action != KegAction::Blocked && name + "/" + k.target != pouring &&
            keg_problem(to_cellar, name, k.target).empty()) {
            k.action = KegAction::Done;
            k.bytes = 0;
            k.bytes_source = "already poured";
        }
        if (k.action != KegAction::Blocked && k.action != KegAction::Done) {
            if (auto m = measure_keg(cfg_to, *pkg, from_cellar / name / k.target, measure)) {
                k.bytes = m->first;
                k.bytes_source = m->second;
            }
        }
        kegs[name] = k;
    };

    for (const auto& [name, o] : owned) {
        const Package* pkg = idx.find(o.entry);
        if (pkg && is_cask_package(*pkg)) {
            plan.casks.push_back(o.entry);
            continue;
        }
        classify(name, o, false);
    }
    // Dependencies the target versions need that no manifest lists (a drifted
    // keg can pick up a new dependency).
    std::vector<std::string> queue;
    for (const auto& [name, k] : kegs) {
        queue.push_back(name);
    }
    while (!queue.empty()) {
        auto name = queue.back();
        queue.pop_back();
        const auto parent = kegs.at(name);
        const Package* pkg = idx.find(parent.entry);
        if (!pkg || parent.action == KegAction::Blocked || parent.action == KegAction::BrewOwned) {
            continue; // brew provides a brew-owned keg's own dependencies
        }
        for (const auto& dep : pkg->dependencies) {
            const auto dep_name = keg_name(dep);
            if (kegs.count(dep_name)) {
                for (const auto& env : parent.envs) {
                    kegs[dep_name].envs.insert(env);
                }
                continue;
            }
            const Package* dp = idx.find(dep);
            if (!dp || is_cask_package(*dp)) {
                continue;
            }
            Owned o;
            o.entry = dep;
            o.envs = parent.envs;
            classify(dep_name, o, true);
            queue.push_back(dep_name);
        }
    }
    // Which kegs den pours (or poured) will load a brew-owned keg from
    // Homebrew's prefix.
    for (auto& [name, k] : kegs) {
        if (k.action == KegAction::Blocked || k.action == KegAction::BrewOwned) {
            continue;
        }
        if (k.action == KegAction::Done) {
            k.brew_links_scanned = true;
            k.brew_links = brew_link_formulae(
                keg_brew_links(to_cellar / name / k.target, cfg.homebrew_prefix));
            continue;
        }
        const Package* pkg = idx.find(k.entry);
        if (!pkg) {
            continue;
        }
        std::set<std::string> deps;
        for (const auto& dep : pkg->dependencies) {
            auto it = kegs.find(keg_name(dep));
            if (it != kegs.end() && it->second.action == KegAction::BrewOwned) {
                deps.insert(it->first);
            }
        }
        k.brew_links.assign(deps.begin(), deps.end());
    }
    for (auto& [name, k] : kegs) {
        plan.kegs.push_back(std::move(k));
        if (plan.kegs.back().action == KegAction::Blocked ||
            plan.kegs.back().action == KegAction::BrewOwned) {
            continue;
        }
        if (plan.kegs.back().bytes) {
            plan.total_bytes += *plan.kegs.back().bytes;
        } else {
            plan.total_known = false;
        }
    }

    // Cask binaries den linked into the old prefix's bin (through envs/ROOT/bin).
    const auto root_bin = cfg.den_home / "envs" / "ROOT" / "bin";
    for (const auto& c : list_den_casks(cfg)) {
        auto receipt = read_cask_receipt(c.path);
        if (!receipt) {
            continue;
        }
        for (const auto& b : receipt->binaries) {
            if (path_within(b.link, root_bin) || path_within(b.link, cfg.prefix / "bin")) {
                plan.cask_relinks.push_back(
                    {receipt->token, to / "bin" / b.link.filename(), b.target});
            }
        }
    }

    for (const auto& name : spec_names(cfg)) {
        if (auto spec = load_spec(cfg, name)) {
            plan.services.push_back(
                {name, spec_changes(*spec, rebase_service_spec(*spec, cfg.prefix, to))});
        }
    }
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(home / "Library" / "LaunchAgents", ec)) {
        auto fn = e.path().filename().string();
        if (fn.starts_with("homebrew.mxcl.") && e.path().extension() == ".plist") {
            plan.brew_plists.push_back(e.path().stem().string());
        }
    }
    std::sort(plan.brew_plists.begin(), plan.brew_plists.end());
    const auto plist = daemon_plist_path(home);
    if (fs::is_regular_file(plist, ec)) {
        plan.daemon_plist = plist;
        auto text = read_text(plist);
        plan.daemon_path_before = plist_path_value(text);
        if (auto after = plist_with_prefix_path(text, to)) {
            plan.daemon_path_after = plist_path_value(*after);
        }
    }
    for (const char* rc : {".zshrc", ".zprofile", ".bashrc", ".bash_profile", ".profile",
                           ".config/fish/config.fish"}) {
        const auto p = home / rc;
        if (fs::is_regular_file(p, ec)) {
            if (int n = count_den_init_lines(p); n > 1) {
                plan.shell_rc_init_lines.emplace_back(p, n);
            }
        }
    }
    plan.free_bytes = free_bytes_at(to, &plan.free_at);
    return plan;
}

void print_migrate_prefix_plan(std::ostream& out, const MigratePrefixPlan& plan) {
    const auto from_cellar = plan.from / "Cellar";
    const std::string to = plan.to.string();
    auto versions = [](const KegPlan& k) { return keg_display_version(k); };
    for (const auto& k : plan.kegs) {
        const auto old_dir = (from_cellar / k.name).string();
        switch (k.action) {
        case KegAction::Reinstall:
            out << "reinstall  " << k.name << " " << k.target << " -> " << to << " ("
                << owner_label(k.owner);
            if (k.owner == KegOwner::Den) {
                out << "; old keg eligible for `den cleanup --prefix " << plan.from.string()
                    << "` later";
            } else {
                out << "; brew still owns " << old_dir;
            }
            out << ")\n";
            break;
        case KegAction::Drift:
            out << "reinstall  " << k.name << " " << versions(k)
                << (k.owner == KegOwner::Missing ? " MISSING" : "") << " -> " << k.target
                << " (manifest drift)\n";
            break;
        case KegAction::Pinned:
            out << "pin        " << k.name << " " << k.target
                << " (pinned, reinstall at pinned version)\n";
            break;
        case KegAction::NewDep:
            out << "reinstall  " << k.name << " " << k.target << " -> " << to
                << " (new dependency of a drifted keg)\n";
            break;
        case KegAction::Done:
            out << "done       " << k.name << " " << k.target << " (already in " << to << ")\n";
            break;
        case KegAction::Blocked:
            out << "blocked    " << k.name << " " << versions(k) << ": " << k.reason << "\n";
            break;
        case KegAction::BrewOwned:
            out << "brew-owned " << k.name << " " << k.target << " (" << k.reason << ")\n";
            break;
        }
    }
    std::size_t brew_linked = 0;
    for (const auto& k : plan.kegs) {
        if (k.brew_links.empty()) {
            continue;
        }
        ++brew_linked;
        std::string deps;
        for (const auto& d : k.brew_links) {
            deps += (deps.empty() ? "" : ", ") + d;
        }
        if (k.brew_links_scanned) {
            out << "brew-link  " << k.name << " " << k.target << " loads " << deps << " from "
                << plan.brew_prefix.string() << " (Mach-O load commands)\n";
        } else {
            out << "brew-link  " << k.name << " " << k.target << " will load " << deps << " from "
                << (plan.brew_prefix / "opt").string() << " through " << to
                << "/opt links (brew-owned dependency)\n";
        }
    }
    for (const auto& c : plan.casks) {
        out << "cask       " << c << " stays in den's caskroom\n";
    }
    for (const auto& r : plan.cask_relinks) {
        out << "relink     " << r.link.string() << " -> " << r.target.string() << " (cask "
            << r.token << ")\n";
    }
    for (const auto& s : plan.services) {
        out << "service    " << s.name
            << (s.changes ? ": spec paths " + plan.from.string() + "/... -> " + to + "/..."
                          : ": no paths under " + plan.from.string() + ", unchanged")
            << "\n";
    }
    for (const auto& b : plan.brew_plists) {
        out << "service    " << b << " -> brew-owned, untouched\n";
    }
    if (plan.daemon_plist) {
        out << "plist      dev.den.daemon PATH -> " << plan.daemon_path_after << "\n";
    } else {
        out << "plist      dev.den.daemon not installed, nothing to rewrite\n";
    }
    for (const auto& [rc, n] : plan.shell_rc_init_lines) {
        out << "shell      " << rc.string() << ": " << n << " `den init` lines (report only)\n";
    }
    out << "shell      PATH follows den's prefix through `den init`; open shells pick it up "
           "after `exec $SHELL`\n";

    for (const auto& k : plan.kegs) {
        if (k.action == KegAction::Blocked || k.action == KegAction::BrewOwned) {
            continue;
        }
        out << "disk       " << k.name << " " << k.target << ": "
            << (k.bytes ? bytes_label(*k.bytes) + " (" + k.bytes_source + ")"
                        : std::string("unknown (bottle not downloadable)"))
            << "\n";
    }
    out << "disk       total " << (plan.total_known ? "" : "at least ")
        << bytes_label(plan.total_bytes) << " to write into " << to << "; free at "
        << plan.free_at.string() << ": "
        << (plan.free_bytes ? bytes_label(*plan.free_bytes) : std::string("unknown")) << "\n";

    const auto pourable = plan.kegs.size() - plan.count(KegAction::Blocked) -
                          plan.count(KegAction::Done) - plan.count(KegAction::BrewOwned);
    out << plan.kegs.size() << " kegs, " << plan.count(KegAction::Drift) << " drift, "
        << plan.count(KegAction::Pinned) << " pinned, " << plan.count(KegAction::Blocked)
        << " blocked, " << plan.count(KegAction::BrewOwned) << " brew-owned, "
        << plan.count(KegAction::Done) << " already moved; " << pourable << " to reinstall";
    if (brew_linked) {
        out << "; " << brew_linked << " link into brew-owned kegs";
    }
    out << "; " << plan.from.string() << " writes: 0\n";
}

// ---- Run ----------------------------------------------------------------------

namespace {

void finish_after_switch(const Config& cfg_to, const PackageIndex& idx, const fs::path& home,
                         const fs::path& from, MigratePrefixJournal& j, std::ostream& out) {
    const auto& den_home = cfg_to.den_home;
    maybe_fault("finish");
    apply_manifest_changes(den_home, j.manifest_changes, true);
    if (!j.ledger_swapped) {
        swap_ledger_in(den_home);
        j.ledger_swapped = true;
        write_migrate_prefix_journal(den_home, j);
    }
    materialise(den_home, cfg_to.store, "/", &idx);
    out << "linked     every environment against " << cfg_to.prefix.string() << "\n";

    for (const auto& name : spec_names(cfg_to)) {
        if (std::find(j.specs_rewritten.begin(), j.specs_rewritten.end(), name) !=
            j.specs_rewritten.end()) {
            continue;
        }
        auto spec = load_spec(cfg_to, name);
        if (!spec) {
            continue;
        }
        auto rebased = rebase_service_spec(*spec, from, cfg_to.prefix);
        if (!spec_changes(*spec, rebased)) {
            continue;
        }
        const auto backup = work_dir(den_home) / "specs" / (name + ".json");
        fs::create_directories(backup.parent_path());
        fs::copy_file(service_dir(cfg_to, name) / "spec.json", backup,
                      fs::copy_options::overwrite_existing);
        save_spec(cfg_to, rebased);
        j.specs_rewritten.push_back(name);
        write_migrate_prefix_journal(den_home, j);
        out << "rewrote    service " << name << " spec\n";
    }

    const auto plist = daemon_plist_path(home);
    std::error_code ec;
    if (!j.plist_rewritten && fs::is_regular_file(plist, ec)) {
        const auto text = read_text(plist);
        if (auto updated = plist_with_prefix_path(text, cfg_to.prefix); updated) {
            const auto backup = work_dir(den_home) / kDaemonPlist;
            write_text_atomic(backup, text);
            write_text_atomic(plist, *updated);
            out << "rewrote    dev.den.daemon PATH -> " << plist_path_value(*updated) << "\n";
        }
        j.plist_rewritten = true;
        write_migrate_prefix_journal(den_home, j);
    }
    j.phase = "done";
    write_migrate_prefix_journal(den_home, j);
}

int run_rollback(const Config& cfg, const MigratePrefixOptions& opts, std::ostream& out) {
    auto journal = read_migrate_prefix_journal(cfg.den_home);
    if (!journal) {
        out << "Nothing to roll back: no " << migrate_prefix_journal_path(cfg.den_home).string()
            << ".\n";
        return 1;
    }
    auto j = *journal;
    if (j.phase == "rolled_back") {
        out << "Already rolled back; den uses " << cfg.prefix.string() << ".\n";
        return 0;
    }
    const auto settings = read_settings(cfg.den_home);
    const bool switched = j.phase == "switched" || j.phase == "done" || j.phase == "rolling_back" ||
                          (settings.store.prefix && *settings.store.prefix == j.to);
    if (!switched) {
        j.phase = "rolled_back";
        write_migrate_prefix_journal(cfg.den_home, j);
        out << "den was never switched to " << j.to << ", so there is nothing to undo; it still "
            << "uses " << cfg.prefix.string() << ". " << j.to
            << " is kept (delete it yourself, or reuse it with --apply).\n";
        return 0;
    }

    auto running = running_services(cfg);
    if (running.any() && !opts.stop_services) {
        out << "Refusing to roll back while den services are running:";
        for (const auto& s : running.services) {
            out << " " << s;
        }
        if (running.daemon) {
            out << " dev.den.daemon";
        }
        out << "\nStop them first, or pass --stop-services to stop them, roll back, and start "
               "them again.\n";
        return 1;
    }
    Stopped stopped;
    if (running.any()) {
        stopped = stop_running(cfg, running, opts, out);
    }

    const fs::path to = j.to;
    const fs::path from = j.from;
    j.phase = "rolling_back";
    write_migrate_prefix_journal(cfg.den_home, j);

    Settings s = read_settings(cfg.den_home);
    s.store.prefix = j.previous_prefix_recorded ? j.previous_prefix : std::nullopt;
    write_settings(cfg.den_home, s);
    out << "restored   store.prefix "
        << (s.store.prefix ? "= " + *s.store.prefix : std::string("(unset)")) << "\n";

    apply_manifest_changes(cfg.den_home, j.manifest_changes, false);
    if (j.ledger_swapped) {
        swap_ledger_out(cfg.den_home);
        j.ledger_swapped = false;
    }
    repoint_root_shims(cfg.den_home, to, from);
    const auto old_store = from / "Cellar";
    std::optional<PackageIndex> idx;
    try {
        idx = load_index(cfg.cache / "index.json");
    } catch (const std::exception&) {
    }
    for (const auto& env : list_all(cfg.den_home)) {
        if (env_slug(env) == "ROOT") {
            continue; // the root env links live in the old prefix: never rewritten
        }
        try {
            materialise(cfg.den_home, old_store, env, idx ? &*idx : nullptr);
        } catch (const std::exception& e) {
            out << "warning: relinking " << env << ": " << e.what() << "\n";
        }
    }
    Config restored = Config::detect();
    for (const auto& name : j.specs_rewritten) {
        const auto backup = work_dir(cfg.den_home) / "specs" / (name + ".json");
        std::error_code ec;
        if (fs::is_regular_file(backup, ec)) {
            fs::copy_file(backup, service_dir(restored, name) / "spec.json",
                          fs::copy_options::overwrite_existing);
            out << "restored   service " << name << " spec\n";
        }
    }
    j.specs_rewritten.clear();
    if (j.plist_rewritten) {
        const auto backup = work_dir(cfg.den_home) / kDaemonPlist;
        std::error_code ec;
        if (fs::is_regular_file(backup, ec)) {
            write_text_atomic(daemon_plist_path(opts.home), read_text(backup));
            out << "restored   dev.den.daemon plist\n";
        }
        j.plist_rewritten = false;
    }
    j.phase = "rolled_back";
    write_migrate_prefix_journal(cfg.den_home, j);
    restart_stopped(restored, stopped, opts, out);
    out << "Rolled back: den uses " << restored.prefix.string() << " again. " << to.string()
        << " is kept (delete it yourself, or reuse it with --apply).\n";
    return 0;
}

} // namespace

int run_migrate_prefix(const Config& cfg, const MigratePrefixOptions& opts, std::ostream& out) {
    if (const char* dp = std::getenv("DEN_PREFIX"); dp && *dp) {
        out << "DEN_PREFIX is set (" << dp
            << "), and it overrides the prefix migrate-prefix would switch to. Unset it "
               "first.\n";
        return 1;
    }
    if (opts.rollback) {
        return run_rollback(cfg, opts, out);
    }

    auto journal = read_migrate_prefix_journal(cfg.den_home);
    const bool journal_open = journal && journal->phase != "rolled_back";
    fs::path to = !opts.to.empty() ? normalised(opts.to)
                  : journal_open   ? fs::path(journal->to)
                                   : normalised(cfg.den_home / "prefix");
    if (journal_open && journal->to != to.string()) {
        out << "An earlier migrate-prefix run targets " << journal->to
            << "; finish it with --resume or undo it with --rollback before choosing "
            << to.string() << ".\n";
        return 1;
    }
    const Settings settings = read_settings(cfg.den_home);
    const bool switched =
        journal_open && (journal->phase == "switched" || journal->phase == "done" ||
                         (settings.store.prefix && *settings.store.prefix == to));
    if (journal_open && journal->phase == "done") {
        out << "den already uses " << to.string() << " (migrated by an earlier run).\n";
        return 0;
    }
    if (!switched && !cfg.shares_homebrew_cellar()) {
        out << "den already has its own prefix (" << cfg.prefix.string() << ", from "
            << to_string(cfg.prefix_source) << "); nothing to migrate.\n";
        return 0;
    }
    const fs::path from = switched ? fs::path(journal->from) : cfg.prefix;

    // Target checks.
    if (path_within(to, normalised(cfg.homebrew_prefix)) || path_within(to, from)) {
        out << "Refusing: " << to.string() << " is inside Homebrew's prefix ("
            << cfg.homebrew_prefix.string() << "). Pick a folder of den's own.\n";
        return 1;
    }
    std::error_code ec;
    if (!journal_open && fs::is_directory(to, ec) && !fs::is_empty(to, ec) &&
        !(journal && journal->to == to.string())) {
        out << "Refusing: " << to.string()
            << " already exists and is not empty, and no earlier migrate-prefix run "
               "created it.\n";
        return 1;
    }

    PackageIndex idx;
    try {
        idx = load_index(cfg.cache / "index.json");
    } catch (const std::exception& e) {
        out << "den's package index is unavailable (" << e.what() << "); run `den update`.\n";
        return 1;
    }
    if (!switched && root_env_needs_migration(cfg.den_home)) {
        out << "The root environment still uses the old envs/ROOT link layout; run `den update` "
               "first, then `den migrate-prefix` again.\n";
        return 1;
    }

    if (switched) {
        if (!opts.resume && !opts.apply) {
            out << "den already switched to " << to.string()
                << " but the last run did not finish; run `den migrate-prefix --resume`.\n";
            return 0;
        }
        auto running = running_services(cfg);
        if (running.any() && !opts.stop_services) {
            out << "Refusing to continue while den services are running; stop them or pass "
                   "--stop-services.\n";
            return 1;
        }
        Stopped stopped;
        if (running.any()) {
            stopped = stop_running(cfg, running, opts, out);
        }
        auto j = *journal;
        int rc = 0;
        const Config cfg_to = config_for(cfg, to);
        try {
            j.phase = "switched";
            finish_after_switch(cfg_to, idx, opts.home, from, j, out);
            out << "Moved: den now uses " << to.string() << ".\n";
        } catch (const std::exception& e) {
            j.failed.push_back(std::string("finish: ") + e.what());
            write_migrate_prefix_journal(cfg.den_home, j);
            print_failure(out, "finish", e.what(), from, true);
            rc = 1;
        }
        restart_stopped(Config::detect(), stopped, opts, out);
        return rc;
    }

    const auto plan = plan_migrate_prefix(cfg, idx, to, opts.home, /*measure=*/true);
    print_migrate_prefix_plan(out, plan);
    if (journal_open) {
        out << "journal    earlier run stopped in phase '" << journal->phase << "' ("
            << journal->done.size() << " kegs poured)";
        if (!journal->failed.empty()) {
            out << "; last failure: " << journal->failed.back();
        }
        out << "\n";
    }

    if (!opts.apply && !opts.resume) {
        out << "\nDry run: nothing was changed. Run `den migrate-prefix --apply` to move den "
               "into "
            << to.string() << ".\n";
        return 0;
    }
    if (journal_open && !opts.resume) {
        out << "\nAn earlier run stopped in phase '" << journal->phase
            << "'. Continue it with `den migrate-prefix --resume`, or undo it with "
               "`den migrate-prefix --rollback`.\n";
        return 1;
    }
    if (opts.resume && !journal_open) {
        out << "\nNothing to resume. Use --apply to start.\n";
        return 1;
    }

    for (const auto& name : opts.only) {
        if (std::none_of(plan.kegs.begin(), plan.kegs.end(),
                         [&](const KegPlan& k) { return k.name == name; })) {
            out << "\n--only " << name << ": not a keg den's manifests own.\n";
            return 1;
        }
    }
    auto selected = [&](const KegPlan& k) { return opts.only.empty() || opts.only.count(k.name); };

    std::vector<const KegPlan*> blocked;
    uint64_t needed = 0;
    std::vector<std::string> unknown;
    for (const auto& k : plan.kegs) {
        if (!selected(k)) {
            continue;
        }
        if (k.action == KegAction::Blocked) {
            blocked.push_back(&k);
        } else if (k.action == KegAction::BrewOwned) {
            continue; // stays in Homebrew's Cellar: nothing to write
        } else if (!k.bytes) {
            unknown.push_back(k.name);
        } else {
            needed += *k.bytes;
        }
    }
    if (!blocked.empty()) {
        out << "\nRefusing: " << blocked.size() << " keg(s) cannot be moved safely:\n";
        for (const auto* k : blocked) {
            out << "  " << k->name << ": " << k->reason << "\n";
        }
        out << "Nothing was changed.\n";
        return 1;
    }
    if (!unknown.empty()) {
        out << "\nRefusing: cannot measure the disk cost of";
        for (const auto& n : unknown) {
            out << " " << n;
        }
        out << " (bottle not downloadable?). Nothing was changed.\n";
        return 1;
    }
    if (!plan.free_bytes || *plan.free_bytes < needed) {
        out << "\nRefusing: the move writes " << bytes_label(needed) << " into " << to.string()
            << " but only "
            << (plan.free_bytes ? bytes_label(*plan.free_bytes) : std::string("an unknown amount"))
            << " is free at " << plan.free_at.string() << ". Nothing was changed.\n";
        return 1;
    }

    auto running = running_services(cfg);
    if (running.any() && !opts.stop_services) {
        out << "\nRefusing: den services are running:";
        for (const auto& s : running.services) {
            out << " " << s;
        }
        if (running.daemon) {
            out << " dev.den.daemon";
        }
        out << "\nStop them first, or pass --stop-services to stop them, migrate, rewrite their "
               "specs and the daemon plist, and start them again. Nothing was changed.\n";
        return 1;
    }

    Stopped stopped;
    if (running.any()) {
        stopped = stop_running(cfg, running, opts, out);
    }

    MigratePrefixJournal j = journal_open ? *journal : MigratePrefixJournal{};
    j.to = to.string();
    j.from = from.string();
    j.failed.clear();
    if (!journal_open) {
        j.done.clear();
        j.manifest_changes.clear();
        j.specs_rewritten.clear();
        j.previous_prefix_recorded = false;
        j.ledger_swapped = false;
        j.plist_rewritten = false;
    }
    const Config cfg_to = config_for(cfg, to);
    std::string phase = "pour";
    std::string item;
    bool did_switch = false;
    int rc = 0;
    try {
        j.phase = "pour";
        write_migrate_prefix_journal(cfg.den_home, j);
        fs::create_directories(cfg_to.store);

        // 1. pour
        std::size_t remaining = 0;
        for (const auto& k : plan.kegs) {
            if (k.action == KegAction::BrewOwned) {
                continue;
            }
            const auto keg = cfg_to.store / k.name / k.target;
            const auto id = k.name + "/" + k.target;
            if (k.action == KegAction::Done && id != j.pouring &&
                keg_problem(cfg_to.store, k.name, k.target).empty()) {
                continue;
            }
            if (!selected(k)) {
                ++remaining;
                continue;
            }
            item = k.name;
            // Each keg is its own transaction: journal it as in flight, pour,
            // verify, then journal it done. Any failure discards exactly
            // this keg, so <to> never holds a keg the journal cannot vouch
            // for. A keg an interrupted run left in flight (or one that no
            // longer verifies) is discarded and poured again.
            discard_keg(cfg_to.store, k.name, k.target);
            j.done.erase(std::remove(j.done.begin(), j.done.end(), id), j.done.end());
            j.pouring = id;
            write_migrate_prefix_journal(cfg.den_home, j);
            try {
                maybe_fault("pour:" + k.name);
                const Package* pkg = idx.find(k.entry);
                pour_formula_keg(cfg_to, *pkg);
                ensure_prefix_opt_link(cfg_to.store, k.name, keg);
                maybe_fault("verify:" + k.name);
                if (auto problem = keg_problem(cfg_to.store, k.name, k.target); !problem.empty()) {
                    throw Error("poured " + k.name + " " + k.target + " but " + problem);
                }
            } catch (...) {
                discard_keg(cfg_to.store, k.name, k.target);
                j.pouring.clear();
                write_migrate_prefix_journal(cfg.den_home, j);
                throw;
            }
            j.pouring.clear();
            j.done.push_back(id);
            write_migrate_prefix_journal(cfg.den_home, j);
            out << "poured     " << k.name << " " << k.target << " -> " << keg.string() << "\n";
        }
        // Re-verify every receipt the journal claims (a resumed run trusts
        // nothing it did not just check).
        for (const auto& k : plan.kegs) {
            if (!selected(k) || k.action == KegAction::BrewOwned) {
                continue;
            }
            if (auto problem = keg_problem(cfg_to.store, k.name, k.target); !problem.empty()) {
                throw Error("verify " + k.name + " " + k.target + ": " + problem);
            }
        }
        if (remaining > 0) {
            out << "\n"
                << remaining << " keg(s) still to pour; den keeps using " << from.string()
                << " until every keg is in " << to.string()
                << ". Continue with `den migrate-prefix --resume`.\n";
            restart_stopped(cfg, stopped, opts, out);
            return 0;
        }

        // 2. link: opt links and cask binary links, all inside <to>
        phase = "link";
        item.clear();
        j.phase = "link";
        write_migrate_prefix_journal(cfg.den_home, j);
        maybe_fault("link");
        for (const auto& k : plan.kegs) {
            if (k.action == KegAction::BrewOwned) {
                link_brew_owned(to, plan.brew_prefix, k.name, out);
                continue;
            }
            ensure_prefix_opt_link(cfg_to.store, k.name, cfg_to.store / k.name / k.target);
        }
        for (const auto& r : plan.cask_relinks) {
            fs::create_directories(r.link.parent_path());
            if (!fs::exists(fs::symlink_status(r.link, ec))) {
                fs::create_symlink(r.target, r.link);
                out << "linked     " << r.link.string() << " -> " << r.target.string() << "\n";
            }
        }

        // 3. materialise every env in a staging den home
        phase = "materialise";
        j.phase = "materialise";
        j.manifest_changes = planned_manifest_changes(cfg.den_home, plan);
        write_migrate_prefix_journal(cfg.den_home, j);
        maybe_fault("materialise");
        build_stage(cfg.den_home, j.manifest_changes);
        materialise(stage_home(cfg.den_home), cfg_to.store, "/", &idx);
        out << "linked     root env into " << to.string() << " (staged)\n";

        // 4. switch, only once every keg verifies in <to>
        phase = "verify";
        for (const auto& k : plan.kegs) {
            if (k.action == KegAction::BrewOwned) {
                continue;
            }
            item = k.name;
            if (auto problem = keg_problem(cfg_to.store, k.name, k.target); !problem.empty()) {
                throw Error("before switching, " + k.name + " " + k.target + ": " + problem);
            }
            if (std::find(j.done.begin(), j.done.end(), k.name + "/" + k.target) == j.done.end()) {
                j.done.push_back(k.name + "/" + k.target);
            }
        }
        item.clear();
        phase = "switch";
        j.previous_prefix_recorded = true;
        j.previous_prefix = read_settings(cfg.den_home).store.prefix;
        write_migrate_prefix_journal(cfg.den_home, j);
        maybe_fault("switch");
        Settings s = read_settings(cfg.den_home);
        s.store.prefix = to.string();
        write_settings(cfg.den_home, s);
        did_switch = true;
        j.phase = "switched";
        write_migrate_prefix_journal(cfg.den_home, j);
        out << "switched   store.prefix = " << to.string() << "\n";

        // 5. finish
        phase = "finish";
        finish_after_switch(cfg_to, idx, opts.home, from, j, out);
        out << "\nMoved: den now uses " << to.string() << ". " << from.string()
            << " was not written; brew keeps everything it owns there.\n";
    } catch (const std::exception& e) {
        j.failed.push_back(phase + (item.empty() ? "" : ": " + item) + ": " + e.what());
        write_migrate_prefix_journal(cfg.den_home, j);
        print_failure(out, phase, e.what(), from, did_switch);
        rc = 1;
    }
    restart_stopped(did_switch ? Config::detect() : cfg, stopped, opts, out);
    return rc;
}

} // namespace den

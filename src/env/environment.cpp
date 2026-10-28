// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "environment.h"

#include "../core/config.h"
#include "../core/error.h"
#include "../provider/package_provider.h"
#include "../provider/registry.h"
#include "../store/link.h"
#include "../store/prefix_link.h"
#include "../store/store.h"
#include "manifest.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace den {

fs::path env_dir(const fs::path& den_home, const std::string& env_path) {
    return den_home / "envs" / env_slug(env_path);
}

namespace {

/// Extract the base name from a versioned formula name.
/// "python@3.12" -> "python", "ffmpeg" -> "ffmpeg"
std::string base_name(const std::string& name) {
    auto at = name.find('@');
    return at != std::string::npos ? name.substr(0, at) : name;
}

/// Map a provider's `binary_paths()` output to the subdir names the link
/// layer uses. Each returned path's filename becomes the env subdir.
std::vector<std::string> subdirs_from(const std::vector<fs::path>& paths) {
    std::vector<std::string> result;
    result.reserve(paths.size());
    for (const auto& p : paths) {
        result.push_back(p.filename().string());
    }
    return result;
}

/// Versioned-family dedup is a Homebrew concept: when both python@3.11 and
/// python@3.12 appear in the env, only one of them gets linked (preferring
/// explicit installs, breaking ties by highest version string). Returns
/// the set of names that should be skipped for full linking and get only
/// an opt/ symlink.
std::set<std::string>
compute_versioned_skips(const std::map<std::string, std::string>& homebrew_resolved,
                        const Manifest& manifest) {
    std::set<std::string> skip;

    std::map<std::string, std::vector<std::string>> families;
    for (const auto& [name, _] : homebrew_resolved) {
        if (name.find('@') != std::string::npos) {
            families[base_name(name)].push_back(name);
        }
    }

    const auto& homebrew_auto = manifest.auto_deps.count("homebrew")
                                    ? manifest.auto_deps.at("homebrew")
                                    : std::set<std::string>{};

    for (const auto& [base, members] : families) {
        if (members.size() <= 1) {
            continue;
        }

        std::vector<std::string> explicit_members;
        for (const auto& m : members) {
            if (!homebrew_auto.count(m)) {
                explicit_members.push_back(m);
            }
        }

        const auto& candidates = explicit_members.empty() ? members : explicit_members;
        auto winner = *std::max_element(candidates.begin(), candidates.end());

        for (const auto& m : members) {
            if (m != winner) {
                skip.insert(m);
                SPDLOG_INFO("{}  skipped (versioned family '{}', linking {} instead)", m, base,
                            winner);
            }
        }
    }
    return skip;
}

/// Create just the `opt/<name>` symlink for a package that's resolved but
/// not fully linked (versioned-family loser or keg-only). Returns 1 on
/// success, 0 otherwise.
uint32_t link_opt_only(const fs::path& pkg_path, const fs::path& env_dir, const std::string& name,
                       const std::string& version) {
    auto opt_dir = env_dir / "opt";
    fs::create_directories(opt_dir);
    auto opt_link = opt_dir / name;

    std::error_code ec;
    if (fs::is_symlink(opt_link, ec)) {
        fs::remove(opt_link, ec);
    }
    fs::create_symlink(pkg_path, opt_link, ec);
    record_linked_version(env_dir, name, version);
    return ec ? 0 : 1;
}

uint32_t materialise_one(const fs::path& den_home, const fs::path& store,
                         const std::string& env_path, const PackageIndex* idx,
                         const MaterialiseOptions& opts, std::vector<PrefixConflict>& conflicts);

} // namespace

std::string describe_conflicts(const fs::path& prefix,
                               const std::vector<PrefixConflict>& conflicts) {
    std::ostringstream out;
    std::set<std::string> pkgs;
    out << conflicts.size() << " path(s) in " << prefix.string()
        << " belong to something den does not manage, so den left them alone:\n";
    for (const auto& c : conflicts) {
        pkgs.insert(c.package);
        std::error_code ec;
        out << "  " << c.path.string();
        if (fs::is_symlink(c.path, ec)) {
            out << " -> " << fs::read_symlink(c.path, ec).string();
        } else if (fs::is_directory(c.path, ec)) {
            out << " (directory)";
        } else {
            out << " (file)";
        }
        out << "  [" << c.package << "]\n";
    }
    out << "Not linked:";
    for (const auto& p : pkgs) {
        out << " " << p;
    }
    out << "\nRemove those paths, or replace them with `den install --overwrite <package>` "
           "(like `brew link --overwrite`).";
    return out.str();
}

MaterialiseResult materialise_checked(const fs::path& den_home, const fs::path& store,
                                      const std::string& env_path, const PackageIndex* idx,
                                      const MaterialiseOptions& opts) {
    MaterialiseResult result;
    result.links = materialise_one(den_home, store, env_path, idx, opts, result.conflicts);
    // A change to this env's manifest changes the resolved set of every env
    // that inherits from it, so re-materialise descendants too. Their own
    // overrides win in resolve(), so overridden packages stay overridden.
    for (const auto& child : env_descendants(den_home, env_path)) {
        SPDLOG_DEBUG("materialise: refreshing descendant env '{}'", child);
        materialise_one(den_home, store, child, idx, opts, result.conflicts);
    }
    return result;
}

uint32_t materialise(const fs::path& den_home, const fs::path& store, const std::string& env_path,
                     const PackageIndex* idx, const MaterialiseOptions& opts) {
    auto result = materialise_checked(den_home, store, env_path, idx, opts);
    if (!result.conflicts.empty()) {
        throw UserError(describe_conflicts(store.parent_path(), result.conflicts));
    }
    return result.links;
}

namespace {

/// The compat links envs/ROOT keeps to the prefix, so anything that still
/// looks in envs/ROOT/bin (cask binaries, `den run`, open shells, scripts)
/// finds the same files.
std::vector<std::string> root_shim_dirs() {
    auto dirs = prefix_link_dirs();
    dirs.push_back("opt");
    return dirs;
}

void ensure_root_shims(const fs::path& dir, const fs::path& prefix) {
    for (const auto& sub : root_shim_dirs()) {
        const auto shim = dir / sub;
        const auto target = prefix / sub;
        std::error_code ec;
        if (!fs::is_directory(target, ec)) {
            continue;
        }
        const auto st = fs::symlink_status(shim, ec);
        if (st.type() == fs::file_type::directory) {
            if (!fs::is_empty(shim, ec)) {
                continue; // old layout with content: `den update` migrates it
            }
            fs::remove(shim, ec);
        } else if (st.type() == fs::file_type::symlink) {
            if (fs::read_symlink(shim, ec) == target) {
                continue;
            }
            fs::remove(shim, ec);
        } else if (st.type() != fs::file_type::not_found) {
            continue;
        }
        fs::create_directory_symlink(target, shim, ec);
        if (ec) {
            SPDLOG_WARN("failed to create {} -> {}: {}", shim.string(), target.string(),
                        ec.message());
        }
    }
}

bool is_cellar_keg(const fs::path& pkg_path, const std::string& name) {
    auto cellar = pkg_path.parent_path().parent_path();
    return !cellar.parent_path().empty() && cellar.filename() == "Cellar" &&
           pkg_path.parent_path().filename() == name;
}

/// `<prefix>/opt/<name>` for a root package. Homebrew kegs use the shared
/// opt-link logic (ensure_prefix_opt_link); other providers' packages get a
/// ledger-tracked link.
uint32_t root_opt_link(const PrefixLinkContext& ctx, const fs::path& pkg_path,
                       const std::string& name) {
    if (pkg_path.string().find("/caskroom/") != std::string::npos) {
        return 0; // brew keeps no opt link for casks
    }
    if (is_cellar_keg(pkg_path, name)) {
        auto action = ensure_prefix_opt_link(pkg_path.parent_path().parent_path(), name, pkg_path);
        return action == OptLinkAction::Created || action == OptLinkAction::Repointed ? 1 : 0;
    }
    return link_opt_into_prefix(ctx, pkg_path, name) ? 1 : 0;
}

uint32_t materialise_root(const fs::path& den_home, const fs::path& store, const PackageIndex* idx,
                          const MaterialiseOptions& opts, std::vector<PrefixConflict>& conflicts) {
    const std::string env_path = "/";
    auto per_provider = resolve_per_provider(den_home, env_path);
    auto manifest = read_manifest(den_home, env_path);
    auto dir = env_dir(den_home, env_path);
    auto ctx = make_prefix_link_context(den_home, store);
    fs::create_directories(ctx.state_dir);
    for (const char* sub : {"bin", "sbin", "opt"}) {
        std::error_code ec;
        fs::create_directories(ctx.prefix / sub, ec);
    }

    auto registry = make_default_registry(idx);
    Config cfg;
    cfg.den_home = den_home;
    cfg.store = store;

    std::set<std::string> skip_versioned;
    auto homebrew_it = per_provider.find("homebrew");
    if (homebrew_it != per_provider.end()) {
        skip_versioned = compute_versioned_skips(homebrew_it->second, manifest);
    }

    uint32_t total = 0;
    std::set<std::string> manifest_names;

    for (const auto& [provider_name, pkgs] : per_provider) {
        for (const auto& [entry, _] : pkgs) {
            manifest_names.insert(keg_name(entry));
        }
        auto* provider = registry.find(provider_name);
        if (!provider) {
            SPDLOG_WARN("env has packages under provider '{}' but no such provider is "
                        "registered — skipping",
                        provider_name);
            continue;
        }

        for (const auto& [entry, version] : pkgs) {
            // A tap-qualified entry ("user/repo/name") is looked up in the
            // index by its qualified form; its keg, prefix links and ledger
            // records all use the bare keg name.
            const std::string name = keg_name(entry);
            auto pkg_path = provider->package_root(cfg, name, version);
            if (!fs::exists(pkg_path)) {
                SPDLOG_WARN("package {}@{} not in store, skipping", name, version);
                continue;
            }
            InstalledPackage installed{name, version, pkg_path};
            auto subdirs = subdirs_from(provider->binary_paths(cfg, installed));
            auto current = prefix_linked_version(ctx, name);

            bool opt_only = skip_versioned.count(entry) > 0 || skip_versioned.count(name) > 0;
            if (provider_name == "homebrew" && idx) {
                if (const auto* info = idx->find(entry); info && info->keg_only) {
                    opt_only = true;
                }
            }

            if (opt_only) {
                if (!current || *current != version || prefix_ledger_has_tree_links(ctx, name)) {
                    unlink_from_prefix(ctx, name);
                    SPDLOG_INFO("{}@{} is keg-only or a versioned-family loser, linking opt/ only",
                                name, version);
                }
                total += root_opt_link(ctx, pkg_path, name);
                record_prefix_linked_version(ctx, name, version);
                continue;
            }

            if (current && *current == version) {
                SPDLOG_INFO("{}@{} already linked, skipping", name, version);
                total += root_opt_link(ctx, pkg_path, name);
                continue;
            }
            const bool overwrite =
                opts.overwrite.count(name) > 0 || opts.overwrite.count(entry) > 0;
            if (current) {
                // Check the new version links cleanly before taking the old
                // one down. A refused swap leaves the old version linked and
                // recorded, so a caller can put the manifest back and be
                // exactly where it started.
                auto blocked = prefix_link_conflicts(ctx, pkg_path, name, subdirs, overwrite);
                if (!blocked.empty()) {
                    SPDLOG_WARN("not switching {} {} -> {}: {} conflicting path(s) in {}", name,
                                *current, version, blocked.size(), ctx.prefix.string());
                    for (auto& path : blocked) {
                        conflicts.push_back({name, std::move(path)});
                    }
                    continue;
                }
                unlink_from_prefix(ctx, name);
            }

            auto result = link_into_prefix(ctx, pkg_path, name, subdirs, overwrite);
            if (!result.conflicts.empty()) {
                for (auto& path : result.conflicts) {
                    conflicts.push_back({name, std::move(path)});
                }
                continue; // no version record, so the next materialise retries
            }
            total += result.created;
            total += root_opt_link(ctx, pkg_path, name);
            record_prefix_linked_version(ctx, name, version);
        }
    }

    // Stale cleanup works from den's ledger, so it neither needs the keg to
    // still exist nor touches links den did not create.
    for (const auto& name : prefix_linked_names(ctx)) {
        if (manifest_names.count(name)) {
            continue;
        }
        unlink_from_prefix(ctx, name);
        SPDLOG_INFO("unlinked stale package '{}' from {}", name, ctx.prefix.string());
    }

    ensure_root_shims(dir, ctx.prefix);
    SPDLOG_INFO("materialised root env into {} ({} symlinks)", ctx.prefix.string(), total);
    return total;
}

uint32_t materialise_one(const fs::path& den_home, const fs::path& store,
                         const std::string& env_path, const PackageIndex* idx,
                         const MaterialiseOptions& opts, std::vector<PrefixConflict>& conflicts) {
    if (env_slug(env_path) == "ROOT") {
        return materialise_root(den_home, store, idx, opts, conflicts);
    }
    // The materialiser asks each provider for its package layout (root +
    // subdirs to surface) and dispatches the link layer accordingly. The
    // result is provider-agnostic — a Homebrew keg, a pip-installed
    // Python package, and a Cargo crate all flow through the same loop.
    auto per_provider = resolve_per_provider(den_home, env_path);
    auto manifest = read_manifest(den_home, env_path);
    auto dir = env_dir(den_home, env_path);
    fs::create_directories(dir);

    auto registry = make_default_registry(idx);

    // The link layer is path-agnostic — it just needs den_home/store to
    // be available on the synthetic Config we pass to provider methods.
    Config cfg;
    cfg.den_home = den_home;
    cfg.store = store;

    // Versioned-family dedup (Homebrew-only).
    std::set<std::string> skip_versioned;
    auto homebrew_it = per_provider.find("homebrew");
    if (homebrew_it != per_provider.end()) {
        skip_versioned = compute_versioned_skips(homebrew_it->second, manifest);
    }

    uint32_t total = 0;

    for (const auto& [provider_name, pkgs] : per_provider) {
        auto* provider = registry.find(provider_name);
        if (!provider) {
            SPDLOG_WARN("env has packages under provider '{}' but no such provider is "
                        "registered — skipping",
                        provider_name);
            continue;
        }

        for (const auto& [entry, version] : pkgs) {
            // A tap-qualified entry ("user/repo/name") is looked up in the
            // index by its qualified form but linked under its keg name.
            const std::string name = keg_name(entry);
            auto pkg_path = provider->package_root(cfg, name, version);
            if (!fs::exists(pkg_path)) {
                SPDLOG_WARN("package {}@{} not in store, skipping", name, version);
                continue;
            }

            InstalledPackage installed{name, version, pkg_path};
            auto exposed = provider->binary_paths(cfg, installed);
            auto subdirs = subdirs_from(exposed);

            // Versioned-family loser: only opt/ link.
            if (skip_versioned.count(entry) || skip_versioned.count(name)) {
                total += link_opt_only(pkg_path, dir, name, version);
                continue;
            }

            // Keg-only (Homebrew, index-driven): only opt/ link.
            if (provider_name == "homebrew" && idx) {
                if (const auto* pkg_info = idx->find(entry); pkg_info && pkg_info->keg_only) {
                    auto added = link_opt_only(pkg_path, dir, name, version);
                    if (added) {
                        SPDLOG_INFO("{}@{} is keg-only, linked opt/ only", name, version);
                    }
                    total += added;
                    continue;
                }
            }

            // Already linked at the requested version → nothing to do.
            auto current = linked_version(dir, name);
            if (current && *current == version) {
                SPDLOG_INFO("{}@{} already linked, skipping", name, version);
                continue;
            }

            // Replace older linked version, if any.
            if (current) {
                auto old_pkg = provider->package_root(cfg, name, *current);
                unlink_package(old_pkg, dir, name, subdirs);
            }

            total += link_package(pkg_path, dir, name, subdirs);
            record_linked_version(dir, name, version);
        }
    }

    // Stale-link cleanup: packages that were previously linked but are no
    // longer in the manifest (e.g. after `den uninstall`) must be removed.
    // Build the set of names currently in the manifest, then scan the
    // linked-version records for any name that's no longer present.
    std::set<std::string> manifest_names;
    for (const auto& [pn, pkgs] : per_provider) {
        for (const auto& [name, _] : pkgs) {
            manifest_names.insert(keg_name(name));
        }
    }

    auto linked_dir = dir / ".den" / "linked";
    std::error_code scan_ec;
    if (fs::is_directory(linked_dir, scan_ec)) {
        for (const auto& entry : fs::directory_iterator(linked_dir, scan_ec)) {
            auto pkg_name = entry.path().filename().string();
            if (manifest_names.count(pkg_name)) {
                continue; // still in manifest, nothing to do
            }
            auto stale_version = linked_version(dir, pkg_name);
            if (!stale_version) {
                continue;
            }
            // Find which registered provider owns this package by checking
            // which one produced an on-disk path for the recorded version.
            // In practice this is almost always homebrew; we try all
            // registered providers so the logic is provider-agnostic.
            bool unlinked = false;
            for (auto* prov : registry.all()) {
                if (!prov) {
                    continue;
                }
                auto pkg_path = prov->package_root(cfg, pkg_name, *stale_version);
                if (!fs::exists(pkg_path)) {
                    continue;
                }
                InstalledPackage stale{pkg_name, *stale_version, pkg_path};
                auto exposed = prov->binary_paths(cfg, stale);
                auto subdirs = subdirs_from(exposed);
                unlink_package(pkg_path, dir, pkg_name, subdirs);
                unlinked = true;
                SPDLOG_INFO("unlinked stale package '{}' ({})", pkg_name, *stale_version);
                break;
            }
            if (!unlinked) {
                // Package is no longer on disk; the keg is gone so we can't
                // call unlink_package (it would enumerate the keg's contents
                // to know what to remove).  Instead, scan the env's subdirs
                // for symlinks whose target sits under the Cellar/<pkg_name>/
                // prefix and remove those — they're now-dangling pointers
                // into the deleted keg.
                auto pkg_prefix = (store / pkg_name).string();
                for (const auto& subdir : {"bin", "opt", "lib", "include", "share"}) {
                    auto sub = dir / subdir;
                    std::error_code dir_ec;
                    if (!fs::is_directory(sub, dir_ec)) {
                        continue;
                    }
                    for (const auto& link_entry : fs::directory_iterator(sub, dir_ec)) {
                        std::error_code sym_ec;
                        if (!fs::is_symlink(link_entry, sym_ec)) {
                            continue;
                        }
                        auto target = fs::read_symlink(link_entry, sym_ec);
                        if (sym_ec) {
                            continue;
                        }
                        if (target.string().starts_with(pkg_prefix)) {
                            std::error_code rm_link_ec;
                            fs::remove(link_entry.path(), rm_link_ec);
                            SPDLOG_INFO("removed dangling symlink {} -> {}",
                                        link_entry.path().string(), target.string());
                        }
                    }
                }
                std::error_code rm_ec;
                fs::remove(entry.path(), rm_ec);
                SPDLOG_INFO("cleared stale linked-version record for '{}' (store entry gone)",
                            pkg_name);
            }
        }
    }

    SPDLOG_INFO("materialised env '{}' ({} symlinks)", env_path, total);
    return total;
}

} // namespace

bool root_env_needs_migration(const fs::path& den_home) {
    const auto dir = env_dir(den_home, "/");
    std::error_code ec;
    const auto linked = dir / ".den" / "linked";
    if (fs::is_directory(linked, ec) && !fs::is_empty(linked, ec)) {
        return true;
    }
    for (const auto& sub : root_shim_dirs()) {
        const auto p = dir / sub;
        if (fs::symlink_status(p, ec).type() == fs::file_type::directory && !fs::is_empty(p, ec)) {
            return true;
        }
    }
    return false;
}

RootMigrationReport migrate_root_env_to_prefix(const fs::path& den_home, const fs::path& store,
                                               const PackageIndex* idx) {
    RootMigrationReport report;
    if (!root_env_needs_migration(den_home)) {
        return report;
    }
    report.migrated = true;
    const auto dir = env_dir(den_home, "/");
    const auto ctx = make_prefix_link_context(den_home, store);

    // Package roots the old layout linked: these links are dropped and
    // re-created in the prefix by materialise. Anything else in envs/ROOT
    // (a cask's binary link, say) is moved into the prefix as-is.
    std::vector<fs::path> roots = {ctx.store};
    {
        auto registry = make_default_registry(idx);
        Config cfg;
        cfg.den_home = den_home;
        cfg.store = store;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir / ".den" / "linked", ec)) {
            auto name = e.path().filename().string();
            auto version = linked_version(dir, name);
            if (!version || !is_valid_package_name(name)) {
                continue;
            }
            for (auto* prov : registry.all()) {
                if (prov && prov->provider_name() != "homebrew") {
                    auto root = prov->package_root(cfg, name, *version);
                    if (!root.empty()) {
                        roots.push_back(root.lexically_normal());
                    }
                }
            }
        }
    }
    auto under_root = [&](const fs::path& target) {
        for (const auto& r : roots) {
            auto rel = target.lexically_relative(r);
            if (!rel.empty() && *rel.begin() != "..") {
                return true;
            }
        }
        return false;
    };

    for (const auto& sub : root_shim_dirs()) {
        const auto p = dir / sub;
        std::error_code ec;
        if (fs::symlink_status(p, ec).type() != fs::file_type::directory) {
            continue;
        }
        std::vector<fs::path> links;
        for (auto it = fs::recursive_directory_iterator(p, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_symlink(ec)) {
                links.push_back(it->path());
            }
        }
        for (const auto& link : links) {
            auto target = fs::read_symlink(link, ec);
            if (ec) {
                continue;
            }
            if (target.is_relative()) {
                target = link.parent_path() / target;
            }
            target = target.lexically_normal();
            if (under_root(target)) {
                fs::remove(link, ec);
                report.removed += ec ? 0 : 1;
                continue;
            }
            const auto dest = ctx.prefix / link.lexically_relative(dir);
            std::error_code dec;
            if (fs::symlink_status(dest, dec).type() != fs::file_type::not_found) {
                continue; // leave it; the dir is preserved below
            }
            fs::create_directories(dest.parent_path(), dec);
            fs::create_symlink(target, dest, dec);
            if (!dec) {
                fs::remove(link, dec);
                ++report.moved;
            }
        }
        // Remove directories the migration emptied, deepest first.
        std::vector<fs::path> dirs;
        for (auto it = fs::recursive_directory_iterator(p, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_directory(ec) && !it->is_symlink(ec)) {
                dirs.push_back(it->path());
            }
        }
        std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
            return a.string().size() > b.string().size();
        });
        for (const auto& d : dirs) {
            if (fs::is_empty(d, ec)) {
                fs::remove(d, ec);
            }
        }
        if (fs::is_empty(p, ec)) {
            fs::remove(p, ec);
        } else {
            auto aside = p;
            aside += ".pre-prefix-link";
            for (int n = 1; fs::exists(fs::symlink_status(aside, ec)); ++n) {
                aside = p;
                aside += ".pre-prefix-link." + std::to_string(n);
            }
            fs::rename(p, aside, ec);
            if (!ec) {
                report.preserved.push_back(aside);
            }
        }
    }
    std::error_code ec;
    fs::remove_all(dir / ".den" / "linked", ec);

    try {
        materialise(den_home, store, "/", idx);
    } catch (const UserError& e) {
        report.conflicts = e.what();
    }
    return report;
}

std::string active_env_path(const fs::path& den_home) {
    auto path = den_home / "active_env";
    std::ifstream in(path);
    if (!in) {
        return "/";
    }
    std::string env_path;
    std::getline(in, env_path);
    if (env_path.empty()) {
        return "/";
    }
    return env_path;
}

void set_active_env(const fs::path& den_home, const std::string& env_path) {
    auto path = den_home / "active_env";

    // Atomic write.
    auto tmp_path = path.string() + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out) {
            throw InternalError("failed to write active_env: " + tmp_path);
        }
        out << env_path << '\n';
        out.flush();
        if (!out) {
            throw InternalError("failed to flush active_env: " + tmp_path);
        }
    }

    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        throw InternalError("failed to rename active_env: " + std::string(std::strerror(errno)));
    }

    SPDLOG_INFO("active environment set to '{}'", env_path);
}

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "version_change.h"

#include "../build/linkage_scan.h"
#include "../core/version.h"
#include "../store/pins.h"
#include "../store/store.h"
#include "manifest.h"

#include <spdlog/spdlog.h>

#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace den {

namespace {

/// The state a change is undone to, captured before anything is written.
struct Snapshot {
    fs::path new_keg;                   // where the new version lands in the store
    bool keg_existed = true;            // so a keg we did not pour is never removed
    fs::path opt_link;                  // <prefix>/opt/<name>
    std::optional<fs::path> opt_target; // its link text, if it was a symlink
    bool opt_existed = false;
    bool installed = false;
    /// For a change with no target version yet (`den install`), the versions
    /// already in the store, so the keg the provider resolves to can be
    /// classified as pre-existing or freshly poured after the fact.
    std::set<std::string> versions_before;
};

std::optional<fs::path> read_link(const fs::path& link) {
    std::error_code ec;
    if (!fs::is_symlink(link, ec)) {
        return std::nullopt;
    }
    auto t = fs::read_symlink(link, ec);
    if (ec) {
        return std::nullopt;
    }
    return t;
}

bool resolves_under(const fs::path& link, const fs::path& dir) {
    auto t = read_link(link);
    if (!t) {
        return false;
    }
    fs::path target = t->is_relative() ? link.parent_path() / *t : *t;
    auto rel = target.lexically_normal().lexically_relative(dir.lexically_normal());
    return !rel.empty() && *rel.begin() != "..";
}

/// Point `link` at `target`, swapping atomically so the link never
/// disappears. Logs and returns false on failure.
bool swap_link(const fs::path& link, const fs::path& target) {
    std::error_code ec;
    auto tmp = link;
    tmp += ".den-rollback";
    fs::remove(tmp, ec);
    fs::create_symlink(target, tmp, ec);
    if (!ec) {
        fs::rename(tmp, link, ec);
    }
    if (ec) {
        SPDLOG_WARN("rollback: could not restore {} -> {}: {}", link.string(), target.string(),
                    ec.message());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

/// Every `<prefix>/opt/<name>` symlink's text, by name.
using OptLinks = std::map<std::string, fs::path>;

OptLinks read_opt_links(const fs::path& opt_dir) {
    OptLinks out;
    std::error_code ec;
    for (fs::directory_iterator it(opt_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (auto t = read_link(it->path())) {
            out[it->path().filename().string()] = *t;
        }
    }
    return out;
}

/// Point every opt link the batch moved (it existed before and now names
/// something else) back where it was, unless `keep` says a change that
/// stays applied still needs it. Links the batch created are left: nothing
/// that was installed before the batch loads through them.
void restore_opt_links(const fs::path& opt_dir, const OptLinks& before,
                       const std::function<bool(const std::string&)>& keep) {
    for (const auto& [name, now] : read_opt_links(opt_dir)) {
        auto was = before.find(name);
        if (was == before.end() || was->second == now || keep(name)) {
            continue;
        }
        const auto link = opt_dir / name;
        const auto target = was->second.is_relative() ? opt_dir / was->second : was->second;
        std::error_code ec;
        if (!fs::exists(target, ec)) {
            continue; // the keg it named is gone; leave the link to cleanup
        }
        if (swap_link(link, was->second)) {
            SPDLOG_INFO("rollback: pointed {} back at {}", link.string(), was->second.string());
        }
    }
}

/// Homebrew formula revision bump: the index has the installed upstream
/// version with a higher `_<revision>`.
bool is_revision_bump(const std::string& installed, const Package& pkg) {
    if (pkg.version.empty() || pkg.version == installed || pkg.revision <= 0) {
        return false;
    }
    auto upstream = installed;
    if (auto us = installed.rfind('_');
        us != std::string::npos && us + 1 < installed.size() &&
        installed.find_first_not_of("0123456789", us + 1) == std::string::npos) {
        upstream = installed.substr(0, us);
    }
    return upstream == pkg.upstream_version() && compare_pkg_versions(installed, pkg.version) < 0;
}

/// Append to `changes` every installed package of the active env whose index
/// entry is a revision bump and which depends at runtime on something in the
/// batch (repeated until nothing more joins). `brought_by[i]` lists the batch
/// members a joined change depends on.
void add_revision_bumped_dependents(const Config& config, const PackageIndex& idx,
                                    const std::string& pname, std::vector<VersionChange>& changes,
                                    std::vector<std::vector<size_t>>& brought_by,
                                    const VersionChangeOptions& opts, VersionChangeResult& result) {
    const auto per = resolve_per_provider(config.den_home, active_env_path(config.den_home));
    auto installed = per.find(pname);
    if (installed == per.end()) {
        return;
    }
    const auto pins = read_pins(config.den_home);
    std::map<std::string, size_t> in_batch;
    for (size_t i = 0; i < changes.size(); ++i) {
        in_batch[keg_name(changes[i].name)] = i;
    }
    std::set<std::string> pinned;
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& [name, version] : installed->second) {
            if (in_batch.count(keg_name(name)) || pinned.count(name)) {
                continue;
            }
            const auto* pkg = idx.find(name);
            if (!pkg || !is_revision_bump(version, *pkg)) {
                continue;
            }
            std::vector<size_t> by;
            for (const auto& dep : pkg->dependencies) {
                if (auto b = in_batch.find(keg_name(dep)); b != in_batch.end()) {
                    by.push_back(b->second);
                }
            }
            if (by.empty()) {
                continue;
            }
            if (pins.count(name)) {
                SPDLOG_WARN("{} {} is pinned; not moving it to {} with {}", name, version,
                            pkg->version, changes[by.front()].name);
                pinned.insert(name);
                result.pinned_dependents.push_back({name, version, pkg->version});
                continue;
            }
            if (opts.screen_dependents) {
                VersionChange candidate{name, version, pkg->version};
                auto held = opts.screen_dependents({candidate});
                if (auto h = held.find(name); h != held.end()) {
                    SPDLOG_WARN("{} {} held back ({}); not moving it to {} with {}", name, version,
                                h->second, pkg->version, changes[by.front()].name);
                    pinned.insert(name);
                    result.held_dependents.emplace_back(std::move(candidate), h->second);
                    continue;
                }
            }
            SPDLOG_INFO("{} {} -> {}: revision bump for {}", name, version, pkg->version,
                        changes[by.front()].name);
            in_batch[keg_name(name)] = changes.size();
            changes.push_back({name, version, pkg->version});
            brought_by.resize(changes.size());
            brought_by.back() = std::move(by);
            result.dependents.push_back(changes.back());
            grew = true;
        }
    }
}

/// Keg names of `roots` and everything they depend on at runtime (per the
/// index; just the roots without one).
std::set<std::string> runtime_closure(const PackageIndex* idx,
                                      const std::vector<std::string>& roots) {
    std::set<std::string> seen;
    std::deque<std::string> todo(roots.begin(), roots.end());
    while (!todo.empty()) {
        auto name = std::move(todo.front());
        todo.pop_front();
        if (!seen.insert(keg_name(name)).second || !idx) {
            continue;
        }
        if (const auto* pkg = idx->find(name)) {
            todo.insert(todo.end(), pkg->dependencies.begin(), pkg->dependencies.end());
        }
    }
    return seen;
}

Snapshot take_snapshot(const Config& config, PackageProvider& provider, const VersionChange& c) {
    Snapshot s;
    const auto name = keg_name(c.name);
    if (!c.to.empty()) {
        s.new_keg = provider.package_root(config, name, c.to);
        std::error_code ec;
        s.keg_existed = fs::exists(s.new_keg, ec) || ec;
    } else {
        std::error_code ec;
        const auto dir = provider.package_root(config, name, "0").parent_path();
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            s.versions_before.insert(it->path().filename().string());
        }
    }
    s.opt_link = config.store.parent_path() / "opt" / name;
    std::error_code ec;
    s.opt_existed = fs::symlink_status(s.opt_link, ec).type() != fs::file_type::not_found;
    s.opt_target = read_link(s.opt_link);
    return s;
}

/// Put the store and opt link back the way `s` found them. Only undoes what
/// the attempt itself did: a keg that was already there stays, and the opt
/// link is only touched while it points into the keg being removed.
bool undo_pour(const Config& config, const Snapshot& s) {
    if (s.keg_existed || s.new_keg.empty()) {
        return false;
    }
    std::error_code ec;
    if (!fs::exists(s.new_keg, ec)) {
        return false;
    }
    // A Cellar keg is only den's to remove if den's receipt says it poured it.
    const auto rel =
        s.new_keg.lexically_normal().lexically_relative(config.store.lexically_normal());
    const bool in_store = !rel.empty() && *rel.begin() != "..";
    if (in_store && !keg_owned_by_den(s.new_keg)) {
        SPDLOG_WARN("rollback: leaving {} (not poured by den)", s.new_keg.string());
        return false;
    }

    if (resolves_under(s.opt_link, s.new_keg)) {
        if (s.opt_target) {
            swap_link(s.opt_link, *s.opt_target);
        } else if (!s.opt_existed) {
            fs::remove(s.opt_link, ec);
        }
    }

    fs::remove_all(s.new_keg, ec);
    if (ec) {
        SPDLOG_WARN("rollback: could not remove {}: {}", s.new_keg.string(), ec.message());
        return false;
    }
    // Drop the now-empty <store>/<name> a fresh pour may have created.
    std::error_code dec;
    if (fs::is_empty(s.new_keg.parent_path(), dec) && !dec) {
        fs::remove(s.new_keg.parent_path(), dec);
    }
    SPDLOG_INFO("rollback: removed {}", s.new_keg.string());
    return true;
}

/// Linkage verification: for each change that moves an installed package to
/// a new keg, the libraries kegs active after the batch load through
/// `<prefix>/opt/<name>` that the old keg had and the new one lacks, one line
/// each, by change index. Only kegs whose runtime closure includes a moving
/// package are scanned.
std::map<size_t, std::vector<std::string>>
unverified_linkage(const Config& config, PackageProvider& provider, const PackageIndex& idx,
                   const std::string& pname, const std::vector<VersionChange>& changes,
                   const std::vector<Snapshot>& snaps) {
    std::map<size_t, std::vector<std::string>> out;
    std::map<std::string, size_t> moving;
    for (size_t i = 0; i < changes.size(); ++i) {
        const auto& c = changes[i];
        std::error_code ec;
        if (c.from.empty() || c.to.empty() || c.from == c.to || snaps[i].new_keg.empty() ||
            !fs::is_directory(snaps[i].new_keg, ec)) {
            continue;
        }
        moving[keg_name(c.name)] = i;
    }
    if (moving.empty()) {
        return out;
    }
    std::map<std::string, std::string> after;
    const auto per = resolve_per_provider(config.den_home, active_env_path(config.den_home));
    if (auto it = per.find(pname); it != per.end()) {
        after = it->second;
    }
    for (const auto& c : changes) {
        if (!c.to.empty()) {
            after[c.name] = c.to;
        }
    }
    const auto opt_dir = config.store.parent_path() / "opt";
    for (const auto& [name, version] : after) {
        const auto self = keg_name(name);
        bool relevant = false;
        for (const auto& dep : runtime_closure(&idx, {name})) {
            relevant = relevant || (dep != self && moving.count(dep));
        }
        std::error_code ec;
        const auto keg = provider.package_root(config, self, version);
        if (!relevant || !fs::is_directory(keg, ec)) {
            continue;
        }
        std::set<std::string> seen;
        for (const auto& load : keg_opt_loads(keg, opt_dir)) {
            auto m = moving.find(load.dep);
            if (m == moving.end() || m->first == self) {
                continue;
            }
            const auto& c = changes[m->second];
            const auto old_keg = provider.package_root(config, keg_name(c.name), c.from);
            std::error_code fec;
            if (!fs::exists(old_keg / load.tail, fec) ||
                fs::exists(snaps[m->second].new_keg / load.tail, fec)) {
                continue;
            }
            auto line = name + " " + version + ": " + load.file.lexically_relative(keg).string() +
                        " loads " + load.load;
            if (seen.insert(line).second) {
                SPDLOG_WARN("linkage: {}, which {} {} does not have", line, c.name, c.to);
                out[m->second].push_back(std::move(line));
            }
        }
    }
    return out;
}

} // namespace

VersionChangeResult apply_version_changes(const Config& config, PackageProvider& provider,
                                          const PackageIndex* idx,
                                          std::vector<VersionChange> changes,
                                          const VersionChangeOptions& opts) {
    VersionChangeResult result;
    const auto active = active_env_path(config.den_home);
    const auto pname = std::string(provider.provider_name());

    // Dependents that have to move with the batch join it before anything
    // is written, so the whole set is one transaction.
    std::vector<std::vector<size_t>> brought_by(changes.size());
    const size_t requested = changes.size();
    if (opts.upgrade_dependents && idx) {
        add_revision_bumped_dependents(config, *idx, pname, changes, brought_by, opts, result);
    }

    // A dependent that would move under a running process holds back the
    // whole batch: moving the dependency without it would leave the chain
    // split, so nothing is poured until every dependent is free.
    if (opts.keg_in_use) {
        for (size_t i = requested; i < changes.size(); ++i) {
            const auto& c = changes[i];
            if (!c.from.empty() && opts.keg_in_use(config.store / keg_name(c.name) / c.from)) {
                SPDLOG_INFO("deferring batch: dependent {} {} is in use", c.name, c.from);
                result.deferred_in_use.push_back(c);
            }
        }
        if (!result.deferred_in_use.empty()) {
            return result;
        }
    }

    // Every opt link as the batch found it (installs and materialise both
    // move them forward), so a rollback can put back what it moved.
    const auto opt_dir = config.store.parent_path() / "opt";
    const auto opt_before = read_opt_links(opt_dir);

    std::vector<Snapshot> snaps;
    std::vector<InstallResult> installs(changes.size());
    snaps.reserve(changes.size());
    for (const auto& c : changes) {
        snaps.push_back(take_snapshot(config, provider, c));
    }

    if (opts.install) {
        for (size_t i = 0; i < changes.size(); ++i) {
            try {
                installs[i] = provider.install(config, changes[i].name, changes[i].to);
            } catch (...) {
                // Nothing has reached the manifest yet; undo every pour and
                // every opt link the batch moved.
                restore_opt_links(opt_dir, opt_before, [](const std::string&) { return false; });
                for (size_t j = 0; j <= i; ++j) {
                    undo_pour(config, snaps[j]);
                }
                throw;
            }
            snaps[i].installed = true;
            if (changes[i].to.empty()) {
                changes[i].to = installs[i].resolved_version;
                if (!changes[i].to.empty()) {
                    auto& sn = snaps[i];
                    sn.new_keg =
                        provider.package_root(config, keg_name(changes[i].name), changes[i].to);
                    sn.keg_existed = sn.versions_before.count(changes[i].to) > 0;
                }
            } else if (!installs[i].resolved_version.empty() &&
                       installs[i].resolved_version != changes[i].to) {
                SPDLOG_DEBUG("{}: asked for {}, provider resolved {}", changes[i].name,
                             changes[i].to, installs[i].resolved_version);
            }
        }
    }

    // Linkage verification, before anything switches: a change whose new
    // keg drops a library installed kegs load through its opt link does not
    // reach the manifest, and neither does a dependent it brought in.
    std::map<size_t, std::vector<std::string>> unverified;
    if (opts.verify_linkage && idx) {
        unverified = unverified_linkage(config, provider, *idx, pname, changes, snaps);
    }
    std::map<size_t, std::vector<fs::path>> conflicted;
    std::map<size_t, std::string> undone_with;
    auto failed = [&](size_t i) {
        return conflicted.count(i) || unverified.count(i) || undone_with.count(i);
    };
    // Every dependent that joined the batch only because of a failed change
    // fails with it.
    auto cascade = [&] {
        for (bool grew = true; grew;) {
            grew = false;
            for (size_t i = 0; i < changes.size(); ++i) {
                const auto& c = changes[i];
                if (failed(i) || c.from.empty() || c.from == c.to) {
                    continue;
                }
                for (auto b : brought_by[i]) {
                    if (failed(b)) {
                        undone_with[i] = changes[b].name;
                        grew = true;
                        break;
                    }
                }
            }
        }
    };
    // Opt links the batch moved go back, except where a change that stays
    // applied (or something it depends on) still needs them. A rolled-back
    // package's own link always goes back.
    auto restore_failed_opt_links = [&] {
        std::vector<std::string> kept;
        std::set<std::string> rolled;
        for (size_t i = 0; i < changes.size(); ++i) {
            if (failed(i)) {
                rolled.insert(keg_name(changes[i].name));
            } else {
                kept.push_back(changes[i].name);
            }
        }
        const auto needed = runtime_closure(idx, kept);
        restore_opt_links(opt_dir, opt_before, [&](const std::string& name) {
            return !rolled.count(name) && needed.count(name);
        });
    };
    if (!unverified.empty()) {
        cascade();
        restore_failed_opt_links();
    }

    with_manifest(config.den_home, active, [&](Manifest& m) {
        auto& bucket = m.packages[pname];
        auto& auto_bucket = m.auto_deps[pname];
        for (size_t i = 0; i < changes.size(); ++i) {
            if (failed(i)) {
                continue;
            }
            bucket[changes[i].name] = changes[i].to;
            if (!opts.record_auto_deps) {
                continue;
            }
            for (const auto& dep : installs[i].auto_deps) {
                if (!bucket.count(dep) && idx) {
                    if (const auto* dep_pkg = idx->find(dep)) {
                        bucket[dep] = dep_pkg->version;
                    }
                }
                auto_bucket.insert(dep);
            }
        }
    });

    auto mat = materialise_checked(config.den_home, config.store, active, idx, opts.link_opts);
    result.links = mat.links;

    std::map<std::string, std::vector<fs::path>> by_pkg;
    for (const auto& c : mat.conflicts) {
        by_pkg[c.package].push_back(c.path);
    }

    // A change already in the manifest that cannot be linked is undone, and
    // so is every dependent that joined the batch only because of it.
    for (size_t i = 0; i < changes.size(); ++i) {
        const auto& c = changes[i];
        auto it = by_pkg.find(keg_name(c.name));
        if (it == by_pkg.end() || c.from.empty() || c.from == c.to || failed(i)) {
            continue;
        }
        conflicted[i] = std::move(it->second);
        by_pkg.erase(it);
    }
    cascade();
    std::vector<size_t> undo;
    for (size_t i = 0; i < changes.size(); ++i) {
        auto conf = conflicted.find(i);
        auto with = undone_with.find(i);
        auto unv = unverified.find(i);
        if (conf == conflicted.end() && with == undone_with.end() && unv == unverified.end()) {
            result.applied.push_back(changes[i]);
            continue;
        }
        RolledBackChange rb;
        rb.change = changes[i];
        if (conf != conflicted.end()) {
            rb.conflicts = std::move(conf->second);
        } else if (unv != unverified.end()) {
            rb.broken_loads = std::move(unv->second);
        } else {
            rb.with = with->second;
        }
        result.rolled_back.push_back(std::move(rb));
        undo.push_back(i);
    }
    for (const auto& c : mat.conflicts) {
        if (by_pkg.count(c.package)) {
            result.conflicts.push_back(c);
        }
    }

    if (!undo.empty()) {
        with_manifest(config.den_home, active, [&](Manifest& m) {
            auto& bucket = m.packages[pname];
            for (auto i : undo) {
                bucket[changes[i].name] = changes[i].from;
            }
        });
        // The root env never let go of the old version (materialise checks a
        // swap before unlinking), but descendant envs may have switched;
        // bring them back before the new keg goes.
        auto again =
            materialise_checked(config.den_home, config.store, active, idx, opts.link_opts);
        for (auto& c : again.conflicts) {
            bool dup = false;
            for (const auto& have : result.conflicts) {
                if (have.package == c.package && have.path == c.path) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                result.conflicts.push_back(std::move(c));
            }
        }
        restore_failed_opt_links();
        for (size_t k = 0; k < undo.size(); ++k) {
            if (opts.install) {
                result.rolled_back[k].keg_removed = undo_pour(config, snaps[undo[k]]);
            }
        }
    }
    return result;
}

std::string describe_rollback(const fs::path& prefix, const RolledBackChange& rb,
                              const std::string& verb, size_t max_paths) {
    std::ostringstream out;
    const auto& c = rb.change;
    if (rb.conflicts.empty() && !rb.with.empty()) {
        out << verb << " of " << c.name << " " << c.from << " -> " << c.to
            << " was rolled back with " << rb.with << ": " << c.to
            << " is the revision built for the new " << rb.with << ".\n"
            << c.name << " stays at " << c.from << " (still linked)";
        if (rb.keg_removed) {
            out << "; the " << c.to << " keg den poured was removed";
        }
        out << ".";
        return out.str();
    }
    if (!rb.broken_loads.empty()) {
        out << verb << " of " << c.name << " " << c.from << " -> " << c.to
            << " was rolled back before switching: " << c.name << " " << c.to << " no longer has "
            << rb.broken_loads.size() << " librar" << (rb.broken_loads.size() == 1 ? "y" : "ies")
            << " that installed packages load through "
            << (prefix / "opt" / keg_name(c.name)).string()
            << ", and the index has no rebuilt (revision-bumped) version of them yet:\n";
        size_t shown = 0;
        for (const auto& line : rb.broken_loads) {
            if (shown++ == max_paths) {
                out << "  ... and " << (rb.broken_loads.size() - max_paths) << " more\n";
                break;
            }
            out << "  " << line << "\n";
        }
        out << c.name << " stays at " << c.from << " (still linked)";
        if (rb.keg_removed) {
            out << "; the " << c.to << " keg den poured was removed";
        }
        out << ". Try again once the index has rebuilt them.";
        return out.str();
    }
    out << verb << " of " << c.name << " " << c.from << " -> " << c.to
        << " was rolled back: linking " << c.to << " would clobber " << rb.conflicts.size()
        << " path(s) in " << prefix.string() << " that den does not manage:\n";
    size_t shown = 0;
    for (const auto& p : rb.conflicts) {
        if (shown++ == max_paths) {
            out << "  ... and " << (rb.conflicts.size() - max_paths) << " more\n";
            break;
        }
        std::error_code ec;
        out << "  " << p.string();
        if (fs::is_symlink(p, ec)) {
            out << " -> " << fs::read_symlink(p, ec).string();
        } else if (fs::is_directory(p, ec)) {
            out << " (directory)";
        } else {
            out << " (file)";
        }
        out << "\n";
    }
    out << c.name << " stays at " << c.from << " (still linked)";
    if (rb.keg_removed) {
        out << "; the " << c.to << " keg den poured was removed";
    }
    out << ". Remove those paths (or `den install --overwrite " << c.name
        << "` to replace them), then try again.";
    return out.str();
}

} // namespace den

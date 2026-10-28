// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "version_change.h"

#include "../store/store.h"
#include "manifest.h"

#include <spdlog/spdlog.h>

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
            // Swap back atomically so the link never disappears.
            auto tmp = s.opt_link;
            tmp += ".den-rollback";
            fs::remove(tmp, ec);
            fs::create_symlink(*s.opt_target, tmp, ec);
            if (!ec) {
                fs::rename(tmp, s.opt_link, ec);
            }
            if (ec) {
                SPDLOG_WARN("rollback: could not restore {} -> {}: {}", s.opt_link.string(),
                            s.opt_target->string(), ec.message());
                fs::remove(tmp, ec);
            }
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

} // namespace

VersionChangeResult apply_version_changes(const Config& config, PackageProvider& provider,
                                          const PackageIndex* idx,
                                          std::vector<VersionChange> changes,
                                          const VersionChangeOptions& opts) {
    VersionChangeResult result;
    const auto active = active_env_path(config.den_home);
    const auto pname = std::string(provider.provider_name());

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
                // Nothing has reached the manifest yet; undo every pour.
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

    with_manifest(config.den_home, active, [&](Manifest& m) {
        auto& bucket = m.packages[pname];
        auto& auto_bucket = m.auto_deps[pname];
        for (size_t i = 0; i < changes.size(); ++i) {
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

    std::vector<size_t> undo;
    for (size_t i = 0; i < changes.size(); ++i) {
        const auto& c = changes[i];
        auto it = by_pkg.find(keg_name(c.name));
        if (it == by_pkg.end() || c.from.empty() || c.from == c.to) {
            result.applied.push_back(c);
            continue;
        }
        RolledBackChange rb;
        rb.change = c;
        rb.conflicts = std::move(it->second);
        by_pkg.erase(it);
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

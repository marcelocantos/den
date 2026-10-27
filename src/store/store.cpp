// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "store.h"

#include "../build/relocate.h"
#include "../env/manifest.h"
#include "link.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

namespace den {

std::string keg_name(std::string_view name) {
    if (std::count(name.begin(), name.end(), '/') != 2)
        return std::string(name);
    return std::string(name.substr(name.rfind('/') + 1));
}

fs::path package_path(const fs::path& store, const std::string& name, const std::string& version) {
    return store / name / version;
}

bool is_installed(const fs::path& store, const std::string& name, const std::string& version) {
    auto p = package_path(store, name, version);
    return fs::exists(p) && fs::is_directory(p);
}

std::vector<InstalledPackage> list_installed(const fs::path& store) {
    std::vector<InstalledPackage> result;

    if (!fs::exists(store) || !fs::is_directory(store)) {
        return result;
    }

    std::error_code ec;
    for (const auto& name_entry : fs::directory_iterator(store, ec)) {
        if (!name_entry.is_directory()) {
            continue;
        }
        auto name = name_entry.path().filename().string();
        for (const auto& ver_entry : fs::directory_iterator(name_entry.path(), ec)) {
            if (!ver_entry.is_directory()) {
                continue;
            }
            auto version = ver_entry.path().filename().string();
            result.push_back(InstalledPackage{
                .name = name,
                .version = version,
                .path = ver_entry.path(),
            });
        }
    }

    if (ec) {
        SPDLOG_WARN("error scanning store: {}", ec.message());
    }

    return result;
}

std::optional<InstalledPackage> which_package(const fs::path& store, const fs::path& file) {
    std::error_code ec;

    // Resolve symlinks to get the real path in the store.
    auto real = fs::canonical(file, ec);
    if (ec) {
        return std::nullopt;
    }

    // Check if the real path is under the store directory.
    auto store_canonical = fs::canonical(store, ec);
    if (ec) {
        return std::nullopt;
    }

    auto rel = real.lexically_relative(store_canonical);
    if (rel.empty() || *rel.begin() == "..") {
        return std::nullopt;
    }

    // Store layout: <name>/<version>/...
    // Extract the first two path components.
    auto it = rel.begin();
    if (it == rel.end())
        return std::nullopt;
    std::string name = it->string();
    ++it;
    if (it == rel.end())
        return std::nullopt;
    std::string version = it->string();

    auto pkg_path = store_canonical / name / version;
    if (!fs::is_directory(pkg_path, ec)) {
        return std::nullopt;
    }

    return InstalledPackage{
        .name = name,
        .version = version,
        .path = pkg_path,
    };
}

uint64_t keg_disk_usage(const fs::path& keg) {
    std::error_code ec;
    if (!fs::exists(keg, ec) || !fs::is_directory(keg, ec)) {
        return 0;
    }

    uint64_t total = 0;
    // Do not follow symlinks: count the link entry itself, never its target —
    // a keg may symlink into shared resources we must not double-count.
    for (auto it = fs::recursive_directory_iterator(
             keg, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            SPDLOG_DEBUG("keg_disk_usage: iteration error under {}: {}", keg.string(),
                         ec.message());
            ec.clear();
            continue;
        }
        std::error_code stat_ec;
        if (it->is_symlink(stat_ec) || stat_ec) {
            continue;
        }
        if (it->is_regular_file(stat_ec) && !stat_ec) {
            auto sz = it->file_size(stat_ec);
            if (!stat_ec) {
                total += sz;
            }
        }
    }
    return total;
}

std::vector<KegInfo> inspect_cellar(const fs::path& store, const fs::path& den_home) {
    // Build a map of "name@version" -> sorted set of referencing env paths by
    // resolving every environment manifest.
    std::map<std::string, std::set<std::string>> refs;
    for (const auto& env_path : list_all(den_home)) {
        auto resolved = resolve(den_home, env_path);
        for (const auto& [name, version] : resolved) {
            refs[keg_name(name) + "@" + version].insert(env_path);
        }
    }

    std::vector<KegInfo> result;
    for (const auto& pkg : list_installed(store)) {
        KegInfo info;
        info.name = pkg.name;
        info.version = pkg.version;
        info.path = pkg.path;
        info.disk_bytes = keg_disk_usage(pkg.path);
        auto it = refs.find(pkg.name + "@" + pkg.version);
        if (it != refs.end()) {
            info.env_refs.assign(it->second.begin(), it->second.end());
        }
        result.push_back(std::move(info));
    }

    std::sort(result.begin(), result.end(), [](const KegInfo& a, const KegInfo& b) {
        if (a.name != b.name)
            return a.name < b.name;
        return a.version < b.version;
    });

    return result;
}

void mark_keg_owned(const fs::path& keg, const std::string& name, const std::string& version) {
    std::error_code ec;
    if (!fs::is_directory(keg, ec) || ec) {
        SPDLOG_WARN("not recording keg ownership; directory missing: {}", keg.string());
        return;
    }

    nlohmann::json receipt = {
        {"poured_by", "den"},
        {"name", name},
        {"version", version},
    };

    // Write beside the final name, then rename into place, so a crash cannot
    // leave a half-written receipt that a later cleanup might misread. Rename
    // also replaces a symlink rather than following it.
    const auto dest = keg / kDenReceiptFilename;
    const auto tmp = keg / (std::string(kDenReceiptFilename) + ".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            SPDLOG_WARN("failed to write den receipt in {}", keg.string());
            return;
        }
        out << receipt.dump(2) << '\n';
        out.flush();
        if (!out) {
            SPDLOG_WARN("failed to write den receipt in {}", keg.string());
            fs::remove(tmp, ec);
            return;
        }
    }

    fs::rename(tmp, dest, ec);
    if (ec) {
        SPDLOG_WARN("failed to install den receipt in {}: {}", keg.string(), ec.message());
        fs::remove(tmp, ec);
    }
}

bool keg_owned_by_den(const fs::path& keg) {
    const auto receipt = keg / kDenReceiptFilename;
    std::error_code ec;
    // A symlink could point at a receipt planted outside this keg. Only a
    // regular file sitting in the keg counts.
    if (fs::is_symlink(receipt, ec) || ec) {
        return false;
    }
    if (!fs::is_regular_file(receipt, ec) || ec) {
        return false;
    }

    std::ifstream in(receipt);
    if (!in) {
        return false;
    }

    try {
        const auto j = nlohmann::json::parse(in);
        if (!j.is_object()) {
            return false;
        }
        if (!j.contains("poured_by") || !j["poured_by"].is_string() || j["poured_by"] != "den") {
            return false;
        }
        if (!j.contains("name") || !j["name"].is_string()) {
            return false;
        }
        if (!j.contains("version") || !j["version"].is_string()) {
            return false;
        }
        // The receipt must name this directory. A copy of some other keg's
        // receipt does not authorise deleting this one.
        if (j["name"].get<std::string>() != keg.parent_path().filename().string()) {
            return false;
        }
        if (j["version"].get<std::string>() != keg.filename().string()) {
            return false;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

namespace {

void remove_empty_formula_dir(const fs::path& store, const fs::path& keg) {
    const auto parent = keg.parent_path();
    if (parent.empty() || parent == store) {
        return;
    }
    std::error_code ec;
    if (!fs::is_directory(parent, ec) || ec) {
        return;
    }
    if (!fs::is_empty(parent, ec) || ec) {
        return;
    }
    fs::remove(parent, ec);
}

} // namespace

CleanupReport cleanup_kegs(const fs::path& store, const fs::path& den_home, const fs::path& cache,
                           bool dry_run) {
    // Versions any environment still resolves to. A keg stays while one env
    // references it, even if another env has moved on.
    std::set<std::string> referenced;
    for (const auto& env_path : list_all(den_home)) {
        for (const auto& [name, version] : resolve(den_home, env_path)) {
            referenced.insert(keg_name(name) + "/" + version);
        }
    }

    CleanupReport report;
    for (const auto& pkg : list_installed(store)) {
        if (referenced.count(pkg.name + "/" + pkg.version)) {
            continue;
        }
        // No receipt means den did not install this keg (Homebrew did, or den
        // poured it before receipts existed). Leave it alone.
        if (!keg_owned_by_den(pkg.path)) {
            ++report.kept_unowned;
            continue;
        }
        if (!dry_run) {
            std::error_code ec;
            fs::remove_all(pkg.path, ec);
            if (ec || fs::exists(pkg.path)) {
                SPDLOG_WARN("cleanup: failed to remove {} {} ({})", pkg.name, pkg.version,
                            ec ? ec.message() : "directory still present");
                continue;
            }
            remove_empty_formula_dir(store, pkg.path);
            // Drop <prefix>/opt/<name> if it pointed at the deleted keg, and
            // hand it to the highest keg left (if any) so shebangs through
            // opt/ keep working.
            if (remove_prefix_opt_link_to(store, pkg.name, pkg.path)) {
                auto next = select_dep_keg(store, pkg.name);
                if (!next.empty()) {
                    ensure_prefix_opt_link(store, pkg.name, next);
                }
            }
        }
        report.removed.push_back(pkg);
    }

    std::sort(report.removed.begin(), report.removed.end(),
              [](const InstalledPackage& a, const InstalledPackage& b) {
                  if (a.name != b.name) {
                      return a.name < b.name;
                  }
                  return a.version < b.version;
              });

    const auto cache_dir = cache / "archives";
    if (fs::is_directory(cache_dir)) {
        if (dry_run) {
            report.cache_cleared = true;
        } else {
            std::error_code ec;
            fs::remove_all(cache_dir, ec);
            if (ec) {
                SPDLOG_WARN("cleanup: failed to clear archive cache {}: {}", cache_dir.string(),
                            ec.message());
            } else {
                report.cache_cleared = true;
            }
        }
    }

    return report;
}

OptReleaseAction release_prefix_opt_link(const fs::path& store, const fs::path& den_home,
                                         const std::string& name, std::string* handed_to) {
    if (!is_valid_package_name(name) || name.find('/') != std::string::npos) {
        return OptReleaseAction::NotOwned;
    }
    const auto prefix = store.parent_path();
    if (store.empty() || prefix.empty()) {
        return OptReleaseAction::NotOwned;
    }
    const auto opt_dir = prefix / "opt";
    const auto link = opt_dir / name;

    std::error_code ec;
    if (!fs::is_symlink(link, ec) || ec) {
        // Missing, or a real file or directory: not a link den wrote.
        return OptReleaseAction::NotOwned;
    }
    // Only a link resolving to a keg of this formula, in this store, that den
    // poured is den's to move. Dangling links are left for ensure_prefix_opt_link
    // and cleanup, which already handle them.
    const auto current = fs::canonical(link, ec);
    if (ec) {
        return OptReleaseAction::NotOwned;
    }
    const auto store_canon = fs::weakly_canonical(store, ec);
    if (ec || current.parent_path() != store_canon / name || !keg_owned_by_den(current)) {
        return OptReleaseAction::NotOwned;
    }

    // Versions of this formula any environment still resolves to.
    std::set<std::string> referenced;
    for (const auto& env_path : list_all(den_home)) {
        // Entries may be tap-qualified ("user/repo/name"); they share the keg.
        for (const auto& [entry, version] : resolve(den_home, env_path)) {
            if (keg_name(entry) == name)
                referenced.insert(version);
        }
    }
    if (referenced.count(current.filename().string())) {
        return OptReleaseAction::Kept;
    }

    // Highest other version that den owns and an environment still uses.
    fs::path next;
    std::string next_ver;
    for (const auto& entry : fs::directory_iterator(store / name, ec)) {
        std::error_code dec;
        if (!entry.is_directory(dec) || dec) {
            continue;
        }
        const auto ver = entry.path().filename().string();
        if (ver == current.filename().string() || !referenced.count(ver) ||
            !keg_owned_by_den(entry.path())) {
            continue;
        }
        if (next.empty() || compare_keg_versions(ver, next_ver) > 0) {
            next = entry.path();
            next_ver = ver;
        }
    }

    if (next.empty()) {
        fs::remove(link, ec);
        if (ec) {
            SPDLOG_WARN("failed to remove prefix opt symlink {}: {}", link.string(), ec.message());
            return OptReleaseAction::Failed;
        }
        return OptReleaseAction::Removed;
    }

    // Relative, as Homebrew writes it; swapped atomically so the link never
    // disappears under a running process.
    auto target = next.lexically_normal().lexically_relative(opt_dir.lexically_normal());
    if (target.empty()) {
        target = next;
    }
    const auto tmp = opt_dir / ("." + name + ".den-tmp");
    fs::remove(tmp, ec);
    std::error_code cec;
    fs::create_symlink(target, tmp, cec);
    if (!cec) {
        fs::rename(tmp, link, cec);
    }
    if (cec) {
        fs::remove(tmp, ec);
        SPDLOG_WARN("failed to repoint prefix opt symlink {}: {}", link.string(), cec.message());
        return OptReleaseAction::Failed;
    }
    if (handed_to) {
        *handed_to = next_ver;
    }
    return OptReleaseAction::HandedOff;
}

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "prefix_link.h"

#include "../core/error.h"
#include "link.h"
#include "store.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace den {

namespace {

constexpr const char* kLedgerDir = "prefix-links";
constexpr const char* kVersionDir = "prefix-linked";

fs::path ledger_path(const PrefixLinkContext& ctx, const std::string& name) {
    return ctx.state_dir / kLedgerDir / name;
}

fs::path version_path(const PrefixLinkContext& ctx, const std::string& name) {
    return ctx.state_dir / kVersionDir / name;
}

/// A ledger line is "<link>\t<target>", both absolute and lexically normal.
using Ledger = std::vector<std::pair<fs::path, fs::path>>;

Ledger read_ledger(const fs::path& file) {
    Ledger out;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        out.emplace_back(fs::path(line.substr(0, tab)), fs::path(line.substr(tab + 1)));
    }
    return out;
}

void write_ledger(const fs::path& file, const Ledger& ledger) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    auto tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            throw InternalError("failed to write prefix link ledger: " + tmp);
        }
        for (const auto& [link, target] : ledger) {
            out << link.string() << '\t' << target.string() << '\n';
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        throw InternalError("failed to write prefix link ledger: " + file.string() + ": " +
                            ec.message());
    }
}

/// Every link path den's ledgers record, across all ROOT packages.
std::set<fs::path> all_ledger_links(const PrefixLinkContext& ctx) {
    std::set<fs::path> out;
    std::error_code ec;
    auto dir = ctx.state_dir / kLedgerDir;
    if (!fs::is_directory(dir, ec)) {
        return out;
    }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".tmp") {
            continue;
        }
        for (const auto& [link, _] : read_ledger(entry.path())) {
            out.insert(link);
        }
    }
    return out;
}

fs::path lexical_target(const fs::path& link) {
    std::error_code ec;
    auto target = fs::read_symlink(link, ec);
    if (ec) {
        return {};
    }
    if (target.is_relative()) {
        target = link.parent_path() / target;
    }
    return target.lexically_normal();
}

bool is_under(const fs::path& p, const fs::path& dir) {
    if (dir.empty()) {
        return false;
    }
    auto rel = p.lexically_normal().lexically_relative(dir.lexically_normal());
    return !rel.empty() && *rel.begin() != "..";
}

/// Does `target` sit inside something den manages: a keg den poured in the
/// Cellar, or a non-Homebrew provider's package under den_home?
bool den_managed_target(const PrefixLinkContext& ctx, const fs::path& target) {
    if (is_under(target, ctx.den_home)) {
        return true;
    }
    if (!is_under(target, ctx.store)) {
        return false;
    }
    auto rel = target.lexically_normal().lexically_relative(ctx.store.lexically_normal());
    auto it = rel.begin();
    if (it == rel.end()) {
        return false;
    }
    auto name = *it++;
    if (it == rel.end()) {
        return false;
    }
    return keg_owned_by_den(ctx.store / name / *it);
}

/// The link text to write: relative for a keg in the prefix's Cellar (as brew
/// writes it), absolute otherwise.
fs::path link_text(const fs::path& prefix, const fs::path& src, const fs::path& dest) {
    if (is_under(src, prefix / "Cellar")) {
        return src.lexically_normal().lexically_relative(dest.parent_path().lexically_normal());
    }
    return src.lexically_normal();
}

enum class Op { MkDir, Link, Replace, Explode };

struct Step {
    Op op;
    fs::path dest;
    fs::path src;
};

struct Planner {
    const PrefixLinkContext& ctx;
    const std::set<fs::path>& ledger_links;
    bool overwrite;
    std::vector<Step> steps;
    std::vector<fs::path> conflicts;

    void plan(const fs::path& src_dir, const fs::path& dst_dir, bool config_dir) {
        std::error_code ec;
        std::vector<fs::directory_entry> entries;
        for (const auto& e : fs::directory_iterator(src_dir, ec)) {
            entries.push_back(e);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.path() < b.path(); });

        for (const auto& entry : entries) {
            const auto src = entry.path().lexically_normal();
            const auto dest = (dst_dir / entry.path().filename()).lexically_normal();
            std::error_code sec;
            // A symlink inside the keg (Frameworks' Versions/Current, a
            // lib alias) is linked as-is, like brew; only real directories
            // are descended into.
            const bool src_is_dir = entry.is_directory(sec) && !entry.is_symlink(sec);
            const auto dstat = fs::symlink_status(dest, sec);

            if (dstat.type() == fs::file_type::not_found) {
                if (src_is_dir) {
                    steps.push_back({Op::MkDir, dest, src});
                    plan(src, dest, config_dir);
                } else {
                    steps.push_back({Op::Link, dest, src});
                }
                continue;
            }

            if (dstat.type() == fs::file_type::symlink) {
                const auto target = lexical_target(dest);
                if (target == src) {
                    continue; // already linked to this exact file
                }
                std::error_code xec;
                const bool dangling = !fs::exists(dest, xec);
                const bool ours = ledger_links.count(dest) || den_managed_target(ctx, target);
                if (src_is_dir) {
                    // brew links whole directories when a single keg owns
                    // them; another keg adding to it explodes the link into a
                    // real directory of per-entry links (keeping the old
                    // keg's files visible).
                    if (dangling) {
                        steps.push_back({Op::Replace, dest, {}});
                        steps.push_back({Op::MkDir, dest, src});
                        plan(src, dest, config_dir);
                    } else if (fs::is_directory(dest, xec) &&
                               (ours || is_under(target, ctx.prefix / "Cellar") || overwrite)) {
                        steps.push_back({Op::Explode, dest, target});
                        plan(src, dest, config_dir);
                    } else {
                        conflicts.push_back(dest);
                    }
                    continue;
                }
                if (dangling || ours || overwrite) {
                    steps.push_back({Op::Replace, dest, src});
                } else {
                    conflicts.push_back(dest);
                }
                continue;
            }

            if (dstat.type() == fs::file_type::directory) {
                if (src_is_dir) {
                    plan(src, dest, config_dir);
                } else {
                    // Never replace a real directory with a file link, even
                    // with --overwrite: it may hold anything.
                    conflicts.push_back(dest);
                }
                continue;
            }

            // A real file (or other non-link) is in the way.
            if (config_dir) {
                SPDLOG_INFO("keeping existing {} (config file)", dest.string());
                continue;
            }
            if (overwrite && !src_is_dir) {
                steps.push_back({Op::Replace, dest, src});
            } else {
                conflicts.push_back(dest);
            }
        }
    }
};

} // namespace

const std::vector<std::string>& prefix_link_dirs() {
    static const std::vector<std::string> dirs = {"bin",   "sbin", "lib", "include",
                                                  "share", "etc",  "var", "Frameworks"};
    return dirs;
}

PrefixLinkContext make_prefix_link_context(const fs::path& den_home, const fs::path& store) {
    PrefixLinkContext ctx;
    ctx.store = store.lexically_normal();
    ctx.prefix = ctx.store.parent_path();
    ctx.den_home = den_home.lexically_normal();
    ctx.state_dir = ctx.den_home / "envs" / "ROOT" / ".den";
    return ctx;
}

PrefixLinkResult link_into_prefix(const PrefixLinkContext& ctx, const fs::path& pkg_path,
                                  const std::string& name, const std::vector<std::string>& subdirs,
                                  bool overwrite) {
    if (!is_valid_package_name(name)) {
        throw UserError("invalid package name: " + name);
    }
    PrefixLinkResult result;
    const auto ledger_links = all_ledger_links(ctx);
    Planner planner{ctx, ledger_links, overwrite, {}, {}};

    for (const auto& dir : subdirs) {
        auto src = (pkg_path / dir).lexically_normal();
        std::error_code ec;
        if (!fs::is_directory(src, ec)) {
            continue;
        }
        auto dst = ctx.prefix / dir;
        const auto dstat = fs::symlink_status(dst, ec);
        if (dstat.type() == fs::file_type::not_found) {
            planner.steps.push_back({Op::MkDir, dst, src});
        } else if (dstat.type() != fs::file_type::directory) {
            // <prefix>/bin itself being a file or a link is not something to
            // paper over.
            planner.conflicts.push_back(dst);
            continue;
        }
        planner.plan(src, dst, dir == "etc" || dir == "var");
    }

    if (!planner.conflicts.empty()) {
        result.conflicts = std::move(planner.conflicts);
        SPDLOG_WARN("not linking {}: {} conflicting path(s) in {}", name, result.conflicts.size(),
                    ctx.prefix.string());
        return result;
    }

    Ledger ledger = read_ledger(ledger_path(ctx, name));
    for (const auto& step : planner.steps) {
        std::error_code ec;
        switch (step.op) {
        case Op::MkDir:
            fs::create_directories(step.dest, ec);
            break;
        case Op::Explode: {
            fs::remove(step.dest, ec);
            fs::create_directory(step.dest, ec);
            for (const auto& child : fs::directory_iterator(step.src, ec)) {
                auto d = step.dest / child.path().filename();
                std::error_code lec;
                fs::create_symlink(link_text(ctx.prefix, child.path(), d), d, lec);
            }
            break;
        }
        case Op::Replace:
            fs::remove(step.dest, ec);
            if (step.src.empty()) {
                break;
            }
            [[fallthrough]];
        case Op::Link:
            fs::create_symlink(link_text(ctx.prefix, step.src, step.dest), step.dest, ec);
            if (ec) {
                SPDLOG_WARN("failed to link {} -> {}: {}", step.dest.string(), step.src.string(),
                            ec.message());
            } else {
                ledger.emplace_back(step.dest, step.src);
                ++result.created;
            }
            break;
        }
    }
    write_ledger(ledger_path(ctx, name), ledger);
    SPDLOG_INFO("linked {} into {} ({} symlinks)", name, ctx.prefix.string(), result.created);
    return result;
}

bool link_opt_into_prefix(const PrefixLinkContext& ctx, const fs::path& pkg_path,
                          const std::string& name) {
    if (!is_valid_package_name(name) || name.find('/') != std::string::npos) {
        return false;
    }
    const auto link = (ctx.prefix / "opt" / name).lexically_normal();
    const auto src = pkg_path.lexically_normal();
    std::error_code ec;
    const auto st = fs::symlink_status(link, ec);
    if (st.type() == fs::file_type::symlink) {
        if (lexical_target(link) == src) {
            return false;
        }
        const bool dangling = !fs::exists(link, ec);
        if (!dangling && !all_ledger_links(ctx).count(link)) {
            return false; // somebody else's opt link
        }
        fs::remove(link, ec);
    } else if (st.type() != fs::file_type::not_found) {
        return false;
    }
    fs::create_directories(link.parent_path(), ec);
    fs::create_symlink(link_text(ctx.prefix, src, link), link, ec);
    if (ec) {
        SPDLOG_WARN("failed to create {}: {}", link.string(), ec.message());
        return false;
    }
    auto ledger = read_ledger(ledger_path(ctx, name));
    ledger.emplace_back(link, src);
    write_ledger(ledger_path(ctx, name), ledger);
    return true;
}

uint32_t unlink_from_prefix(const PrefixLinkContext& ctx, const std::string& name) {
    if (!is_valid_package_name(name)) {
        throw UserError("invalid package name: " + name);
    }
    uint32_t removed = 0;
    std::set<fs::path> parents;
    for (const auto& [link, target] : read_ledger(ledger_path(ctx, name))) {
        std::error_code ec;
        if (!fs::is_symlink(link, ec) || lexical_target(link) != target) {
            continue; // replaced since (by brew, the user, or --overwrite)
        }
        fs::remove(link, ec);
        if (!ec) {
            ++removed;
            parents.insert(link.parent_path());
        }
    }
    // Prune directories den's links left empty, deepest first, but never the
    // top-level prefix dirs (bin, lib, ...) themselves.
    std::vector<fs::path> dirs(parents.begin(), parents.end());
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
        return a.string().size() > b.string().size();
    });
    for (auto dir : dirs) {
        while (is_under(dir, ctx.prefix) && dir.parent_path() != ctx.prefix) {
            std::error_code ec;
            if (fs::is_symlink(dir, ec) || !fs::is_directory(dir, ec) || !fs::is_empty(dir, ec)) {
                break;
            }
            fs::remove(dir, ec);
            dir = dir.parent_path();
        }
    }
    std::error_code ec;
    fs::remove(ledger_path(ctx, name), ec);
    fs::remove(version_path(ctx, name), ec);
    SPDLOG_INFO("unlinked {} from {} ({} symlinks removed)", name, ctx.prefix.string(), removed);
    return removed;
}

std::optional<std::string> prefix_linked_version(const PrefixLinkContext& ctx,
                                                 const std::string& name) {
    std::ifstream in(version_path(ctx, name));
    std::string v;
    if (!in || !std::getline(in, v) || v.empty()) {
        return std::nullopt;
    }
    return v;
}

void record_prefix_linked_version(const PrefixLinkContext& ctx, const std::string& name,
                                  const std::string& version) {
    auto path = version_path(ctx, name);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    out << version << '\n';
}

std::vector<std::string> prefix_linked_names(const PrefixLinkContext& ctx) {
    std::set<std::string> names;
    for (const char* sub : {kLedgerDir, kVersionDir}) {
        std::error_code ec;
        auto dir = ctx.state_dir / sub;
        if (!fs::is_directory(dir, ec)) {
            continue;
        }
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() != ".tmp") {
                names.insert(e.path().filename().string());
            }
        }
    }
    return {names.begin(), names.end()};
}

bool prefix_ledger_has_tree_links(const PrefixLinkContext& ctx, const std::string& name) {
    for (const auto& [link, _] : read_ledger(ledger_path(ctx, name))) {
        if (!is_under(link, ctx.prefix / "opt")) {
            return true;
        }
    }
    return false;
}

} // namespace den

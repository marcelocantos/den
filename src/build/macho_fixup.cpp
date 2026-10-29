// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "macho_fixup.h"

#include "opt_prefix_build.h"
#include "relocate.h"

#include "core/error.h"
#include "provider/exec.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>
#include <system_error>

namespace den {

namespace {

/// `p` relative to `root` when `p` is `root` or below it.
std::optional<fs::path> rel_within(const fs::path& p, const fs::path& root) {
    if (root.empty()) {
        return std::nullopt;
    }
    const auto rel = p.lexically_normal().lexically_relative(root.lexically_normal());
    if (rel.empty() || rel.native().rfind("..", 0) == 0) {
        return std::nullopt;
    }
    return rel;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

/// The default prefix of another Homebrew install that `path` names, if any.
/// A keg den builds into its own prefix must not load from or search one:
/// a build that found Homebrew there (git's Makefile assumes /opt/homebrew
/// on arm64) installs a keg that breaks when that Homebrew changes or goes.
/// A root that is, contains or lies inside den's prefix is den's own.
std::optional<std::string> foreign_homebrew_root(const std::string& path, const fs::path& store) {
    const auto own = store.parent_path().lexically_normal().string() + "/";
    for (const std::string root :
         {"/opt/homebrew", "/usr/local/Cellar", "/usr/local/opt", "/home/linuxbrew/.linuxbrew"}) {
        const auto r = root + "/";
        if (starts_with(own, r) || starts_with(r, own)) {
            continue;
        }
        if (path == root || starts_with(path, r)) {
            return root;
        }
    }
    return std::nullopt;
}

/// `target` (a path inside the keg) as seen from `file`'s directory.
std::string loader_relative(const fs::path& target, const fs::path& file) {
    auto rel = target.lexically_normal().lexically_relative(file.parent_path().lexically_normal());
    if (rel.empty() || rel == ".") {
        return "@loader_path";
    }
    return "@loader_path/" + rel.generic_string();
}

/// Expand an rpath the way dyld would for `file`; empty when unknown.
std::optional<fs::path> expand_rpath(const std::string& rpath, const fs::path& file,
                                     uint32_t filetype) {
    for (const std::string tag : {"@loader_path", "@executable_path"}) {
        if (rpath == tag || starts_with(rpath, tag + "/")) {
            if (tag == "@executable_path" && filetype != kMachoExecute) {
                return std::nullopt; // depends on the host executable
            }
            const auto rest = rpath.substr(tag.size());
            return (file.parent_path() / fs::path(rest.empty() ? "." : rest.substr(1)))
                .lexically_normal();
        }
    }
    if (starts_with(rpath, "/")) {
        return fs::path(rpath);
    }
    return std::nullopt;
}

bool path_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

bool resolves_via(const std::string& leaf, const std::vector<std::string>& rpaths,
                  const fs::path& file, uint32_t filetype) {
    return std::any_of(rpaths.begin(), rpaths.end(), [&](const std::string& rp) {
        const auto dir = expand_rpath(rp, file, filetype);
        return dir && path_exists(*dir / leaf);
    });
}

/// Does dyld find `load` for `file`?
bool load_resolves(const std::string& load, const fs::path& file, const MachoInfo& info,
                   const std::vector<std::string>& rpaths) {
    if (starts_with(load, "/usr/lib/") || starts_with(load, "/System/")) {
        return true; // dyld shared cache: not necessarily on disk
    }
    if (starts_with(load, "@rpath/")) {
        return resolves_via(load.substr(7), rpaths, file, info.filetype);
    }
    if (starts_with(load, "@executable_path/") && info.filetype != kMachoExecute) {
        return true; // resolved against whichever executable loads it
    }
    const auto expanded = expand_rpath(load, file, info.filetype);
    return expanded && path_exists(*expanded);
}

void push_unique(std::vector<std::string>& v, const std::string& s) {
    if (std::find(v.begin(), v.end(), s) == v.end()) {
        v.push_back(s);
    }
}

#ifdef __APPLE__
bool codesign_ok(const fs::path& file) {
    const auto r = run_tool({"codesign", "-v", file.string()});
    return r.spawned && r.exit_code == 0;
}

/// Run `argv` against `file` with owner-write temporarily granted (kegs can
/// install read-only binaries).
ToolResult run_writable(const std::vector<std::string>& argv, const fs::path& file) {
    std::error_code ec;
    const auto perms = fs::status(file, ec).permissions();
    const bool add_write = !ec && (perms & fs::perms::owner_write) == fs::perms::none;
    if (add_write) {
        fs::permissions(file, fs::perms::owner_write, fs::perm_options::add, ec);
    }
    auto r = run_tool(argv);
    if (add_write) {
        fs::permissions(file, perms, fs::perm_options::replace, ec);
    }
    return r;
}

void sign(const fs::path& file) {
    const auto r = run_writable({"codesign", "--force", "--sign", "-", file.string()}, file);
    if (!r.spawned || r.exit_code != 0) {
        throw UserError("codesign -f -s - failed for " + file.string() + ": " + r.output);
    }
}
#endif

} // namespace

MachoFixContext macho_fix_context(const fs::path& store, const std::string& name,
                                  const std::string& version) {
    return {store / name / version, opt_prefix_for(store, name), store, name};
}

std::string fixup_load_path(const std::string& path, const fs::path& file,
                            const MachoFixContext& ctx) {
    if (!starts_with(path, "/")) {
        return path;
    }
    const fs::path p(path);
    if (auto rel = rel_within(p, ctx.keg)) {
        return loader_relative(ctx.keg / *rel, file);
    }
    if (auto rel = rel_within(p, ctx.opt)) {
        return loader_relative(ctx.keg / *rel, file);
    }
    if (auto rel = rel_within(p, ctx.store)) {
        auto it = rel->begin();
        const auto dep = it->string();
        if (++it == rel->end()) {
            return path; // the store's <dep> directory itself
        }
        fs::path rest; // below <store>/<dep>/<version>
        for (++it; it != rel->end(); ++it) {
            rest /= *it;
        }
        if (dep == ctx.name) {
            return loader_relative(ctx.keg / rest, file); // another version of this keg
        }
        const auto opt = opt_prefix_for(ctx.store, dep);
        return (rest.empty() ? opt : opt / rest).string();
    }
    return path;
}

MachoFixPlan plan_macho_fixup(const fs::path& file, const MachoInfo& info,
                              const MachoFixContext& ctx) {
    MachoFixPlan plan;
    plan.file = file;
    const auto in_keg = rel_within(file, ctx.keg).value_or(file.filename());

    std::vector<std::string> rpaths; // after the plan is applied
    for (const auto& lc : info.commands) {
        if (lc.kind == MachoLoadKind::Id) {
            if (info.filetype != kMachoDylib || !starts_with(lc.path, "/")) {
                continue; // keep @rpath/... ids as built
            }
            const fs::path id(lc.path);
            fs::path rel = in_keg;
            if (auto r = rel_within(id, ctx.keg)) {
                rel = *r;
            } else if (auto r2 = rel_within(id, ctx.opt)) {
                rel = *r2;
            }
            const auto wanted = (ctx.opt / rel).lexically_normal().string();
            if (wanted != lc.path) {
                plan.new_id = wanted;
            }
        } else if (lc.kind == MachoLoadKind::Rpath) {
            const auto fixed = fixup_load_path(lc.path, file, ctx);
            if (std::find(rpaths.begin(), rpaths.end(), fixed) != rpaths.end()) {
                plan.delete_rpaths.push_back(lc.path); // would duplicate
            } else {
                if (fixed != lc.path) {
                    plan.rpath_changes.emplace_back(lc.path, fixed);
                }
                rpaths.push_back(fixed);
            }
        } else {
            const auto fixed = fixup_load_path(lc.path, file, ctx);
            const std::pair<std::string, std::string> change{lc.path, fixed};
            if (fixed != lc.path &&
                std::find(plan.changes.begin(), plan.changes.end(), change) == plan.changes.end()) {
                plan.changes.push_back(change);
            }
        }
    }

    // An @rpath load that only resolved through a now-rewritten (or never
    // present) rpath gets an @loader_path rpath to the keg's own copy.
    for (const auto& lc : info.commands) {
        if (!is_dylib_load(lc.kind) || !starts_with(lc.path, "@rpath/")) {
            continue;
        }
        const auto leaf = lc.path.substr(7);
        std::vector<std::string> all = rpaths;
        all.insert(all.end(), plan.add_rpaths.begin(), plan.add_rpaths.end());
        if (resolves_via(leaf, all, file, info.filetype)) {
            continue;
        }
        for (const auto& dir : {ctx.keg / "lib", file.parent_path()}) {
            if (path_exists(dir / leaf)) {
                push_unique(plan.add_rpaths, loader_relative(dir, file));
                break;
            }
        }
    }
    return plan;
}

std::vector<fs::path> keg_macho_files(const fs::path& keg) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator
             it(keg, fs::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        std::error_code sec;
        const auto st = it->symlink_status(sec);
        if (sec || !fs::is_regular_file(st)) {
            continue;
        }
        const auto info = macho_info(it->path());
        if (info.is_macho && (info.filetype == kMachoExecute || info.filetype == kMachoDylib ||
                              info.filetype == kMachoBundle)) {
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<MachoProblem> verify_keg_macho(const MachoFixContext& ctx, bool check_signatures) {
    std::vector<MachoProblem> problems;
    for (const auto& file : keg_macho_files(ctx.keg)) {
        const auto info = macho_info(file);
        std::vector<std::string> rpaths;
        for (const auto& lc : info.commands) {
            if (lc.kind == MachoLoadKind::Rpath) {
                rpaths.push_back(lc.path);
            }
        }
        for (const auto& lc : info.commands) {
            if (starts_with(lc.path, "/") && rel_within(fs::path(lc.path), ctx.store)) {
                problems.push_back({file, "names a versioned keg path: " + lc.path});
            }
            if (const auto root = foreign_homebrew_root(lc.path, ctx.store)) {
                problems.push_back({file, "names another Homebrew prefix (" + *root +
                                              "), outside den's: " + lc.path});
            }
            if (is_dylib_load(lc.kind) && lc.kind != MachoLoadKind::WeakLoad &&
                !load_resolves(lc.path, file, info, rpaths)) {
                problems.push_back({file, "cannot resolve dylib load: " + lc.path});
            }
        }
#ifdef __APPLE__
        if (check_signatures && !codesign_ok(file)) {
            problems.push_back({file, "codesign -v failed"});
        }
#else
        (void)check_signatures;
#endif
    }
    return problems;
}

std::size_t fixup_keg_macho(const MachoFixContext& ctx) {
#ifdef __APPLE__
    std::size_t modified = 0;
    const auto files = keg_macho_files(ctx.keg);
    for (const auto& file : files) {
        const auto plan = plan_macho_fixup(file, macho_info(file), ctx);
        if (plan.empty()) {
            if (!codesign_ok(file)) {
                sign(file); // e.g. an unsigned x86_64 build product
                ++modified;
            }
            continue;
        }
        std::vector<std::string> argv{macho_tool("install_name_tool")};
        if (!plan.new_id.empty()) {
            argv.insert(argv.end(), {"-id", plan.new_id});
        }
        for (const auto& [from, to] : plan.changes) {
            argv.insert(argv.end(), {"-change", from, to});
        }
        for (const auto& [from, to] : plan.rpath_changes) {
            argv.insert(argv.end(), {"-rpath", from, to});
        }
        for (const auto& r : plan.delete_rpaths) {
            argv.insert(argv.end(), {"-delete_rpath", r});
        }
        for (const auto& r : plan.add_rpaths) {
            argv.insert(argv.end(), {"-add_rpath", r});
        }
        argv.push_back(file.string());
        SPDLOG_DEBUG("macho fixup: {} ({} change(s))", file.string(), argv.size() / 3);
        const auto r = run_writable(argv, file);
        if (!r.spawned || r.exit_code != 0) {
            if (install_name_tool_out_of_header_space(r.output)) {
                throw UserError("cannot fix Mach-O linkage of " + file.string() +
                                ": the rewritten load commands do not fit in its header "
                                "(link with -Wl,-headerpad_max_install_names). The keg was not "
                                "installed.\ninstall_name_tool: " +
                                r.output);
            }
            throw UserError("install_name_tool failed for " + file.string() + ": " + r.output);
        }
        sign(file);
        ++modified;
    }

    const auto problems = verify_keg_macho(ctx, true);
    if (!problems.empty()) {
        std::string msg = "Mach-O linkage check failed for " + ctx.keg.string() + ":";
        std::size_t shown = 0;
        for (const auto& p : problems) {
            if (++shown > 20) {
                msg += "\n  ... and " + std::to_string(problems.size() - 20) + " more";
                break;
            }
            const auto rel = rel_within(p.file, ctx.keg).value_or(p.file);
            msg += "\n  " + rel.string() + ": " + p.detail;
        }
        msg += "\nThe keg was not installed.";
        throw UserError(msg);
    }
    SPDLOG_INFO("macho fixup: {} of {} Mach-O file(s) in {} modified", modified, files.size(),
                ctx.keg.string());
    return modified;
#else
    (void)ctx;
    return 0;
#endif
}

} // namespace den

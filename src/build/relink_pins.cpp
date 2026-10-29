// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "relink_pins.h"

#include "linkage_scan.h"
#include "macho_fixup.h"
#include "macho_scan.h"
#include "relocate.h"

#include "provider/exec.h"
#include "store/store.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <map>
#include <set>
#include <system_error>
#include <unistd.h>

namespace den {

namespace {

/// `p` below `root` as "<dep>/<version>/<tail...>", or empty.
fs::path below(const std::string& p, const fs::path& root) {
    const auto rel = fs::path(p).lexically_normal().lexically_relative(root.lexically_normal());
    if (rel.empty() || rel.native().rfind("..", 0) == 0) {
        return {};
    }
    return rel;
}

#ifdef __APPLE__
bool codesign_ok(const fs::path& file) {
    const auto r = run_tool({"codesign", "-v", file.string()});
    return r.spawned && r.exit_code == 0;
}

/// Rewrite `file`'s pinned loads and rpaths per `opt_for` ("<store>/<dep>/<version>"
/// prefix -> "<opt>/<dep>"). Returns an error, or empty after `file` was
/// replaced with the rewritten copy.
std::string relink_file(const fs::path& file, const fs::path& store,
                        const std::map<std::string, fs::path>& opt_for, const std::string& tool) {
    const auto info = macho_info(file);
    std::vector<std::string> rpaths;
    for (const auto& lc : info.commands) {
        if (lc.kind == MachoLoadKind::Rpath) {
            rpaths.push_back(lc.path);
        }
    }
    std::vector<std::string> argv{tool};
    std::vector<std::string> rewritten;
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto& lc : info.commands) {
        if (lc.kind == MachoLoadKind::Id) {
            continue;
        }
        const auto rel = below(lc.path, store);
        auto it = rel.begin();
        if (rel.empty() || std::distance(rel.begin(), rel.end()) < 2) {
            continue;
        }
        const auto dep = (it++)->string();
        const auto version = (it++)->string();
        auto opt = opt_for.find(dep + "/" + version);
        if (opt == opt_for.end()) {
            continue;
        }
        fs::path to = opt->second;
        for (; it != rel.end(); ++it) {
            to /= *it;
        }
        const auto target = to.string();
        if (!seen.insert({lc.path, target}).second) {
            continue;
        }
        if (lc.kind == MachoLoadKind::Rpath) {
            if (std::find(rpaths.begin(), rpaths.end(), target) != rpaths.end()) {
                argv.insert(argv.end(), {"-delete_rpath", lc.path});
            } else {
                argv.insert(argv.end(), {"-rpath", lc.path, target});
                rpaths.push_back(target);
            }
        } else {
            argv.insert(argv.end(), {"-change", lc.path, target});
            rewritten.push_back(target);
        }
    }
    if (argv.size() == 1) {
        return {};
    }

    // Rewrite a copy with the same base name (ad-hoc signing names it by
    // file name) in a scratch dir beside the keg, then rename it over.
    std::error_code ec;
    const auto scratch = file.parent_path() / (".den-relink-" + std::to_string(::getpid()));
    fs::create_directories(scratch, ec);
    if (ec) {
        return "cannot create " + scratch.string() + ": " + ec.message();
    }
    struct Cleanup {
        fs::path dir;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(dir, e);
        }
    } cleanup{scratch};
    const auto copy = scratch / file.filename();
    fs::copy_file(file, copy, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return "cannot copy: " + ec.message();
    }
    const auto perms = fs::status(file, ec).permissions();
    fs::permissions(copy, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::add,
                    ec);
    argv.push_back(copy.string());
    auto r = run_tool(argv);
    if (!r.spawned || r.exit_code != 0) {
        if (install_name_tool_out_of_header_space(r.output)) {
            return "the opt paths do not fit in its Mach-O header";
        }
        return "install_name_tool failed: " + r.output;
    }
    r = run_tool({"codesign", "--force", "--sign", "-", copy.string()});
    if (!r.spawned || r.exit_code != 0) {
        return "codesign -f -s - failed: " + r.output;
    }
    if (!codesign_ok(copy)) {
        return "codesign -v failed after the rewrite";
    }
    for (const auto& load : rewritten) {
        if (!fs::exists(load, ec)) {
            return "rewritten load " + load + " does not resolve";
        }
    }
    fs::permissions(copy, perms, fs::perm_options::replace, ec);
    fs::rename(copy, file, ec);
    if (ec) {
        return "cannot replace the file: " + ec.message();
    }
    return {};
}
#endif

} // namespace

std::string relink_blocker(const fs::path& store, const std::string& dep,
                           const std::string& version) {
    const auto link = store.parent_path() / "opt" / dep;
    const auto keg = store / dep / version;
    std::error_code ec;
    if (fs::symlink_status(link, ec).type() != fs::file_type::symlink) {
        return link.string() +
               (fs::exists(fs::symlink_status(link, ec)) ? " is not a symlink" : " is missing");
    }
    const auto real = fs::weakly_canonical(link, ec);
    if (ec || !fs::exists(real, ec)) {
        return link.string() + " is dangling";
    }
    if (real != fs::weakly_canonical(keg, ec)) {
        return link.string() + " names " + real.filename().string() + ", not " + version;
    }
    if (!keg_owned_by_den(keg)) {
        return keg.string() + " was not poured by den";
    }
    return {};
}

RelinkReport relink_pinned_kegs(const Config& config, const std::string& install_name_tool) {
    RelinkReport report;
#ifdef __APPLE__
    std::error_code ec;
    if (config.shares_homebrew_cellar() || !fs::is_directory(config.store, ec)) {
        return report;
    }
    const auto opt_dir = config.store.parent_path() / "opt";
    const auto tool =
        install_name_tool.empty() ? macho_tool("install_name_tool") : install_name_tool;
    for (const auto& keg : list_installed(config.store)) {
        if (!keg_owned_by_den(keg.path)) {
            continue;
        }
        const auto pins = keg_pinned_loads(keg.path, config.store, keg.name);
        if (pins.empty()) {
            continue;
        }
        const auto label = keg.name + " " + keg.version;
        std::map<std::string, fs::path> opt_for; // "<dep>/<version>" -> opt/<dep>
        std::set<std::string> blocked;
        std::map<fs::path, std::set<std::string>> files; // file -> "dep version"
        for (const auto& p : pins) {
            const auto key = p.dep + "/" + p.version;
            if (!opt_for.count(key) && !blocked.count(key)) {
                if (auto why = relink_blocker(config.store, p.dep, p.version); why.empty()) {
                    opt_for[key] = opt_dir / p.dep;
                } else {
                    blocked.insert(key);
                    report.left_pinned.push_back(label + " -> " + p.dep + " " + p.version + ": " +
                                                 why);
                }
            }
            if (opt_for.count(key)) {
                files[p.file].insert(p.dep + " " + p.version);
            }
        }
        std::vector<std::string> done;
        for (const auto& [file, deps] : files) {
            const auto rel = file.lexically_relative(keg.path).string();
            if (auto err = relink_file(file, config.store, opt_for, tool); !err.empty()) {
                SPDLOG_WARN("relink: {} {}: {}", label, rel, err);
                report.failed.push_back(label + ": " + rel + ": " + err);
                continue;
            }
            ++report.files;
            std::string d;
            for (const auto& x : deps) {
                d += (d.empty() ? "" : ", ") + x;
            }
            done.push_back(rel + ": " + d);
        }
        if (!done.empty()) {
            std::string line = label + " (";
            for (size_t i = 0; i < done.size(); ++i) {
                line += (i ? "; " : "") + done[i];
            }
            report.relinked.push_back(line + ")");
            SPDLOG_INFO("relink: {} now loads its dependencies through {}", label,
                        opt_dir.string());
        }
    }
#else
    (void)config;
    (void)install_name_tool;
#endif
    return report;
}

} // namespace den

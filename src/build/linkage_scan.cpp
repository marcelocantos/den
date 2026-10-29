// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "linkage_scan.h"

#include "elf_scan.h"
#include "macho_scan.h"

#include <iterator>
#include <set>
#include <utility>

namespace den {

namespace {

/// `p` relative to `dir` when `p` lies strictly below it, else empty.
fs::path below(const fs::path& p, const fs::path& dir) {
    if (dir.empty() || !p.is_absolute()) {
        return {};
    }
    auto rel = p.lexically_normal().lexically_relative(dir.lexically_normal());
    if (rel.empty() || *rel.begin() == ".." || rel == ".") {
        return {};
    }
    return rel;
}

/// Split `rel` into its first component and the rest.
std::pair<std::string, fs::path> head_rest(const fs::path& rel) {
    auto it = rel.begin();
    std::string head = it->string();
    fs::path rest;
    for (++it; it != rel.end(); ++it) {
        rest /= *it;
    }
    return {head, rest};
}

template <typename Fn> void for_each_binary(const fs::path& keg, Fn&& fn) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(keg, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code sec;
        const auto st = it->symlink_status(sec);
        if (sec || !fs::is_regular_file(st)) {
            continue;
        }
        fn(it->path());
    }
}

} // namespace

std::vector<OptLoad> keg_opt_loads(const fs::path& keg, const fs::path& opt_dir) {
    std::vector<OptLoad> out;
    std::set<std::pair<fs::path, std::string>> seen;
    auto add = [&](const fs::path& file, const fs::path& rel, const std::string& load, bool exact) {
        auto [dep, tail] = head_rest(rel);
        if (tail.empty() || !seen.insert({file, load}).second) {
            return;
        }
        out.push_back({file, dep, load, tail, exact});
    };
    for_each_binary(keg, [&](const fs::path& file) {
        if (auto m = macho_info(file); m.is_macho) {
            std::vector<fs::path> rpaths;
            for (const auto& c : m.commands) {
                if (c.kind == MachoLoadKind::Rpath) {
                    if (auto rel = below(c.path, opt_dir); !rel.empty()) {
                        rpaths.push_back(rel);
                    }
                }
            }
            for (const auto& c : m.commands) {
                if (!is_dylib_load(c.kind)) {
                    continue;
                }
                if (auto rel = below(c.path, opt_dir); !rel.empty()) {
                    add(file, rel, c.path, true);
                } else if (c.path.starts_with("@rpath/")) {
                    const auto name = c.path.substr(7);
                    for (const auto& r : rpaths) {
                        add(file, r / name, (opt_dir / r).string() + "/" + name, false);
                    }
                }
            }
            return;
        }
        auto e = elf_dynamic(file);
        if (!e.is_elf) {
            return;
        }
        for (const auto& sp : e.search_paths) {
            const auto rel = below(sp, opt_dir);
            if (rel.empty()) {
                continue;
            }
            for (const auto& n : e.needed) {
                if (n.find('/') == std::string::npos) {
                    add(file, rel / n, (opt_dir / rel).string() + "/" + n, false);
                }
            }
        }
    });
    return out;
}

std::vector<PinnedLoad> keg_pinned_loads(const fs::path& keg, const fs::path& store,
                                         const std::string& self_name) {
    std::vector<PinnedLoad> out;
    auto add = [&](const fs::path& file, const std::string& load) {
        const auto rel = below(load, store);
        auto it = rel.begin();
        if (rel.empty() || std::distance(rel.begin(), rel.end()) < 2) {
            return;
        }
        const auto dep = (it++)->string();
        const auto version = it->string();
        if (dep == self_name) {
            return;
        }
        out.push_back({file, dep, version, load});
    };
    for_each_binary(keg, [&](const fs::path& file) {
        if (auto m = macho_info(file); m.is_macho) {
            for (const auto& c : m.commands) {
                if (c.kind != MachoLoadKind::Id) {
                    add(file, c.path);
                }
            }
            return;
        }
        auto e = elf_dynamic(file);
        for (const auto& sp : e.search_paths) {
            add(file, sp);
        }
    });
    return out;
}

} // namespace den

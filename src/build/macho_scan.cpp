// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "macho_scan.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <optional>
#include <set>
#include <tuple>

namespace den {

namespace {

constexpr uint32_t kMhMagic = 0xfeedface;
constexpr uint32_t kMhMagic64 = 0xfeedfacf;
constexpr uint32_t kMhCigam = 0xcefaedfe;
constexpr uint32_t kMhCigam64 = 0xcffaedfe;
constexpr uint32_t kFatMagic = 0xcafebabe;
constexpr uint32_t kFatMagic64 = 0xcafebabf;
constexpr uint32_t kLcReqDyld = 0x80000000;

// Load commands whose payload starts with an lc_str offset to a path.
std::optional<MachoLoadKind> path_kind(uint32_t cmd) {
    switch (cmd) {
    case 0xc: // LC_LOAD_DYLIB
        return MachoLoadKind::Load;
    case 0xd: // LC_ID_DYLIB
        return MachoLoadKind::Id;
    case 0x18 | kLcReqDyld: // LC_LOAD_WEAK_DYLIB
        return MachoLoadKind::WeakLoad;
    case 0x1c | kLcReqDyld: // LC_RPATH
        return MachoLoadKind::Rpath;
    case 0x1f | kLcReqDyld: // LC_REEXPORT_DYLIB
        return MachoLoadKind::Reexport;
    case 0x20: // LC_LAZY_LOAD_DYLIB
        return MachoLoadKind::LazyLoad;
    case 0x23 | kLcReqDyld: // LC_LOAD_UPWARD_DYLIB
        return MachoLoadKind::UpwardLoad;
    default:
        return std::nullopt;
    }
}

uint32_t swap32(uint32_t v) {
    return ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
}

uint32_t le32(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint32_t be32(const unsigned char* p) {
    return uint32_t(p[3]) | (uint32_t(p[2]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[0]) << 24);
}
uint64_t be64(const unsigned char* p) {
    return (uint64_t(be32(p)) << 32) | be32(p + 4);
}

bool read_at(std::ifstream& f, uint64_t off, unsigned char* buf, std::size_t n) {
    f.clear();
    f.seekg(static_cast<std::streamoff>(off));
    if (!f) {
        return false;
    }
    f.read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(n));
    return static_cast<std::size_t>(f.gcount()) == n;
}

// Load commands of the thin image at `off`; false when it isn't one.
bool scan_thin(std::ifstream& f, uint64_t off, MachoInfo& out) {
    std::array<unsigned char, 32> h{};
    if (!read_at(f, off, h.data(), 28)) {
        return false;
    }
    const uint32_t magic = le32(h.data());
    bool swap = false;
    std::size_t header = 0;
    if (magic == kMhMagic) {
        header = 28;
    } else if (magic == kMhMagic64) {
        header = 32;
    } else if (magic == kMhCigam) {
        header = 28;
        swap = true;
    } else if (magic == kMhCigam64) {
        header = 32;
        swap = true;
    } else {
        return false;
    }
    auto u32 = [swap](const unsigned char* p) { return swap ? swap32(le32(p)) : le32(p); };
    const uint32_t filetype = u32(h.data() + 12);
    const uint32_t ncmds = u32(h.data() + 16);
    const uint32_t sizeofcmds = u32(h.data() + 20);
    if (sizeofcmds > (64u << 20) || ncmds > sizeofcmds / 8) {
        return false; // implausible: not a real header
    }
    std::vector<unsigned char> cmds(sizeofcmds);
    if (sizeofcmds && !read_at(f, off + header, cmds.data(), sizeofcmds)) {
        return false;
    }
    if (!out.is_macho) {
        out.is_macho = true;
        out.filetype = filetype;
    }
    std::size_t pos = 0;
    for (uint32_t i = 0; i < ncmds && pos + 8 <= cmds.size(); ++i) {
        const uint32_t cmd = u32(cmds.data() + pos);
        const uint32_t size = u32(cmds.data() + pos + 4);
        if (size < 8 || pos + size > cmds.size()) {
            break;
        }
        const auto kind = path_kind(cmd);
        if (kind && size >= 12) {
            const uint32_t str = u32(cmds.data() + pos + 8);
            if (str >= 12 && str < size) {
                const auto* begin = reinterpret_cast<const char*>(cmds.data() + pos + str);
                const auto* end = reinterpret_cast<const char*>(cmds.data() + pos + size);
                const auto* nul = std::find(begin, end, '\0');
                MachoLoadCommand lc{*kind, std::string(begin, nul)};
                if (!lc.path.empty() &&
                    std::find(out.commands.begin(), out.commands.end(), lc) == out.commands.end()) {
                    out.commands.push_back(std::move(lc));
                }
            }
        }
        pos += size;
    }
    return true;
}

bool within(const fs::path& p, const fs::path& root) {
    const auto rel = p.lexically_normal().lexically_relative(root.lexically_normal());
    return !rel.empty() && rel.native().rfind("..", 0) != 0;
}

std::string formula_of(const fs::path& brew_path, const fs::path& brew_prefix) {
    const auto rel =
        brew_path.lexically_normal().lexically_relative(brew_prefix.lexically_normal());
    auto it = rel.begin();
    if (it == rel.end()) {
        return {};
    }
    const auto top = it->string();
    if ((top == "opt" || top == "Cellar") && ++it != rel.end()) {
        return it->string();
    }
    return {};
}

} // namespace

bool is_dylib_load(MachoLoadKind kind) {
    return kind != MachoLoadKind::Id && kind != MachoLoadKind::Rpath;
}

std::vector<std::string> macho_load_paths(const fs::path& file) {
    std::vector<std::string> out;
    for (auto& lc : macho_info(file).commands) {
        if (std::find(out.begin(), out.end(), lc.path) == out.end()) {
            out.push_back(std::move(lc.path));
        }
    }
    return out;
}

MachoInfo macho_info(const fs::path& file) {
    MachoInfo out;
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        return out;
    }
    std::array<unsigned char, 8> h{};
    if (!read_at(f, 0, h.data(), 8)) {
        return out;
    }
    const uint32_t magic = be32(h.data());
    if (magic == kFatMagic || magic == kFatMagic64) {
        const uint32_t n = be32(h.data() + 4);
        if (n == 0 || n >= 30) {
            return out; // Java class files share 0xCAFEBABE; their "count" is a version
        }
        const std::size_t entry = magic == kFatMagic64 ? 32 : 20;
        std::vector<unsigned char> arch(entry * n);
        if (!read_at(f, 8, arch.data(), arch.size())) {
            return out;
        }
        for (uint32_t i = 0; i < n; ++i) {
            const auto* a = arch.data() + i * entry;
            const uint64_t off = magic == kFatMagic64 ? be64(a + 8) : be32(a + 8);
            scan_thin(f, off, out);
        }
        return out;
    }
    scan_thin(f, 0, out);
    return out;
}

std::vector<BrewLinkRef> keg_brew_links(const fs::path& keg, const fs::path& brew_prefix) {
    std::vector<BrewLinkRef> refs;
    if (brew_prefix.empty()) {
        return refs;
    }
    std::error_code ec;
    const auto brew_canon = fs::weakly_canonical(brew_prefix, ec);
    for (auto it = fs::recursive_directory_iterator(keg, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code sec;
        const auto st = it->symlink_status(sec);
        if (sec || !fs::is_regular_file(st)) {
            continue;
        }
        for (const auto& load : macho_load_paths(it->path())) {
            if (load.empty() || load.front() != '/') {
                continue; // @rpath / @loader_path / @executable_path
            }
            const fs::path lp(load);
            fs::path landed;
            if (within(lp, brew_prefix)) {
                landed = lp;
            } else {
                std::error_code cec;
                const auto canon = fs::weakly_canonical(lp, cec);
                if (!cec && !brew_canon.empty() && within(canon, brew_canon)) {
                    landed = brew_prefix / canon.lexically_relative(brew_canon);
                } else {
                    // Resolve just the <x>/opt/<f> link itself: dyld follows
                    // it even when the file below is missing.
                    for (auto p = lp.parent_path(); !p.empty() && p != p.root_path();
                         p = p.parent_path()) {
                        std::error_code lec;
                        if (fs::is_symlink(fs::symlink_status(p, lec))) {
                            auto target = fs::read_symlink(p, lec);
                            if (!lec) {
                                if (target.is_relative()) {
                                    target = p.parent_path() / target;
                                }
                                const auto full = target / lp.lexically_relative(p);
                                if (within(full, brew_prefix)) {
                                    landed = full.lexically_normal();
                                } else if (!brew_canon.empty() && within(full, brew_canon)) {
                                    landed = brew_prefix /
                                             full.lexically_normal().lexically_relative(brew_canon);
                                }
                            }
                            break;
                        }
                    }
                }
            }
            if (landed.empty()) {
                continue;
            }
            refs.push_back({it->path(), load, landed.lexically_normal().string(),
                            formula_of(landed, brew_prefix)});
        }
    }
    std::sort(refs.begin(), refs.end(), [](const BrewLinkRef& a, const BrewLinkRef& b) {
        return std::tie(a.file, a.load) < std::tie(b.file, b.load);
    });
    return refs;
}

std::vector<std::string> brew_link_formulae(const std::vector<BrewLinkRef>& refs) {
    std::set<std::string> s;
    for (const auto& r : refs) {
        s.insert(r.formula.empty() ? r.brew_path : r.formula);
    }
    return {s.begin(), s.end()};
}

} // namespace den

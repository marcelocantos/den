// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "relocate.h"

#include "provider/exec.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

namespace den {

namespace {

/// Read a file into a string.
std::string read_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Write a string to a file, preserving permissions.
void write_file(const fs::path& path, const std::string& content) {
    auto perms = fs::status(path).permissions();
    auto tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    fs::rename(tmp, path);
    fs::permissions(path, perms);
}

/// Check if a file is likely a text file (no null bytes in first 8KB).
bool is_text_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    char buf[8192];
    f.read(buf, sizeof(buf));
    auto n = f.gcount();
    for (std::streamsize i = 0; i < n; ++i) {
        if (buf[i] == '\0')
            return false;
    }
    return true;
}

/// Replace all occurrences of `from` with `to` in a string.
/// Returns true if any replacements were made.
bool replace_all(std::string& s, const std::string& from, const std::string& to) {
    bool changed = false;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
        changed = true;
    }
    return changed;
}

/// Replace a string in binary data, null-padding if the replacement
/// is shorter. The replacement must be <= the original length.
bool replace_binary(std::string& data, const std::string& from, const std::string& to) {
    if (to.size() > from.size()) {
        SPDLOG_WARN("binary replacement '{}' -> '{}' would grow — skipping", from, to);
        return false;
    }

    bool changed = false;
    size_t pos = 0;
    while ((pos = data.find(from, pos)) != std::string::npos) {
        // Replace the old string with the new one, null-pad the rest.
        data.replace(pos, from.size(), to);
        // Null-pad the remaining bytes.
        for (size_t i = to.size(); i < from.size(); ++i) {
            data[pos + i] = '\0';
        }
        pos += from.size(); // Skip past the full original length.
        changed = true;
    }
    return changed;
}

bool has_homebrew_placeholder(std::string_view s) {
    return s.find("@@HOMEBREW_") != std::string_view::npos;
}

#ifdef __APPLE__
/// True if the file looks like a Mach-O binary (thin or fat).
bool is_macho_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    unsigned char magic[4] = {};
    f.read(reinterpret_cast<char*>(magic), 4);
    if (f.gcount() < 4)
        return false;
    // MH_MAGIC / MH_CIGAM / MH_MAGIC_64 / MH_CIGAM_64 / FAT_MAGIC / FAT_CIGAM
    // (and 64-bit fat variants).
    const uint32_t m = (uint32_t(magic[0]) << 24) | (uint32_t(magic[1]) << 16) |
                       (uint32_t(magic[2]) << 8) | uint32_t(magic[3]);
    switch (m) {
    case 0xFEEDFACE: // MH_MAGIC
    case 0xCEFAEDFE: // MH_CIGAM
    case 0xFEEDFACF: // MH_MAGIC_64
    case 0xCFFAEDFE: // MH_CIGAM_64
    case 0xCAFEBABE: // FAT_MAGIC
    case 0xBEBAFECA: // FAT_CIGAM
    case 0xCAFEBABF: // FAT_MAGIC_64
    case 0xBFBAFECA: // FAT_CIGAM_64
        return true;
    default:
        return false;
    }
}

/// Strip the " (compatibility version …)" suffix from an otool -L line.
std::string strip_otool_compat(std::string line) {
    auto paren = line.find(" (compatibility version");
    if (paren != std::string::npos) {
        line.resize(paren);
    }
    // Trim leading whitespace/tabs.
    size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
        ++start;
    }
    return line.substr(start);
}

/// Collect install names from `otool -L` (includes the dylib id as the first
/// dependency line for dylibs; for executables every line is a dependency).
std::vector<std::string> otool_load_names(const fs::path& file) {
    auto result = run_tool({"otool", "-L", file.string()});
    if (!result.spawned || result.exit_code != 0) {
        return {};
    }
    std::vector<std::string> names;
    std::istringstream ss(result.output);
    std::string line;
    bool first = true;
    while (std::getline(ss, line)) {
        if (first) {
            // Header line: "<path>:"
            first = false;
            continue;
        }
        auto name = strip_otool_compat(line);
        if (!name.empty()) {
            names.push_back(std::move(name));
        }
    }
    return names;
}

/// Dylib identity from `otool -D` (empty for executables).
std::string otool_dylib_id(const fs::path& file) {
    auto result = run_tool({"otool", "-D", file.string()});
    if (!result.spawned || result.exit_code != 0) {
        return {};
    }
    std::istringstream ss(result.output);
    std::string line;
    bool first = true;
    while (std::getline(ss, line)) {
        if (first) {
            first = false;
            continue;
        }
        auto name = strip_otool_compat(line);
        if (!name.empty()) {
            return name;
        }
    }
    return {};
}

/// LC_RPATH values from `otool -l`.
std::vector<std::string> otool_rpaths(const fs::path& file) {
    auto result = run_tool({"otool", "-l", file.string()});
    if (!result.spawned || result.exit_code != 0) {
        return {};
    }
    std::vector<std::string> rpaths;
    std::istringstream ss(result.output);
    std::string line;
    bool in_rpath = false;
    while (std::getline(ss, line)) {
        if (line.find("LC_RPATH") != std::string::npos) {
            in_rpath = true;
            continue;
        }
        if (in_rpath) {
            auto pos = line.find("path ");
            if (pos != std::string::npos) {
                auto path = line.substr(pos + 5);
                // "path /foo/bar (offset N)"
                auto paren = path.find(" (offset");
                if (paren != std::string::npos) {
                    path.resize(paren);
                }
                // trim
                while (!path.empty() && (path.back() == ' ' || path.back() == '\r')) {
                    path.pop_back();
                }
                if (!path.empty()) {
                    rpaths.push_back(path);
                }
                in_rpath = false;
            }
        }
    }
    return rpaths;
}

void codesign_adhoc(const fs::path& file) {
    // Arm64 macOS kills modified Mach-O binaries with an invalid signature
    // (SIGKILL, no output). Re-sign ad-hoc after install_name_tool, matching
    // Homebrew's post-relocate step.
    auto result = run_tool({"codesign", "--force", "--sign", "-", file.string()});
    if (!result.spawned || result.exit_code != 0) {
        SPDLOG_WARN("codesign ad-hoc failed for {}: {}", file.string(), result.output);
    }
}

/// Expand @@HOMEBREW_*@@ placeholders in Mach-O load commands, dylib ids, and
/// rpaths via install_name_tool. Length may grow (CELLAR placeholder is 19
/// bytes; /opt/homebrew/Cellar is 20), so in-place null-pad patching is not
/// sufficient for load commands.
uint32_t fix_macho_placeholders(const fs::path& dir, const std::string& prefix,
                                const std::string& cellar) {
    uint32_t count = 0;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        // Skip obvious non-binaries quickly; still verify Mach-O magic.
        auto ext = entry.path().extension().string();
        if (ext == ".h" || ext == ".pc" || ext == ".cmake" || ext == ".rb" || ext == ".txt" ||
            ext == ".md" || ext == ".json" || ext == ".1" || ext == ".3") {
            continue;
        }
        if (!is_macho_file(entry.path())) {
            continue;
        }

        const auto file = entry.path();
        bool changed = false;

        // Snapshot names before any mutation. otool -L lists the dylib id as
        // the first entry for shared libraries; that is LC_ID_DYLIB and must
        // be rewritten with -id, not -change.
        auto id = otool_dylib_id(file);
        auto load_names = otool_load_names(file);

        if (!id.empty() && has_homebrew_placeholder(id)) {
            auto new_id = expand_homebrew_placeholders(id, prefix, cellar);
            if (new_id != id) {
                auto r = run_tool({"install_name_tool", "-id", new_id, file.string()});
                if (r.spawned && r.exit_code == 0) {
                    changed = true;
                    SPDLOG_DEBUG("relocated dylib id: {} -> {}", id, new_id);
                } else {
                    SPDLOG_WARN("install_name_tool -id failed for {}: {}", file.string(), r.output);
                }
            }
        }

        // Dependent install names (LC_LOAD_DYLIB), skipping the id entry.
        for (const auto& name : load_names) {
            if (!id.empty() && name == id) {
                continue;
            }
            if (!has_homebrew_placeholder(name)) {
                continue;
            }
            auto new_name = expand_homebrew_placeholders(name, prefix, cellar);
            if (new_name == name) {
                continue;
            }
            auto r = run_tool({"install_name_tool", "-change", name, new_name, file.string()});
            if (r.spawned && r.exit_code == 0) {
                changed = true;
                SPDLOG_DEBUG("relocated install name: {} -> {}", name, new_name);
            } else {
                SPDLOG_WARN("install_name_tool -change failed for {} ({} -> {}): {}", file.string(),
                            name, new_name, r.output);
            }
        }

        // Rpaths.
        for (const auto& rpath : otool_rpaths(file)) {
            if (!has_homebrew_placeholder(rpath)) {
                continue;
            }
            auto new_rpath = expand_homebrew_placeholders(rpath, prefix, cellar);
            if (new_rpath == rpath) {
                continue;
            }
            auto r = run_tool({"install_name_tool", "-rpath", rpath, new_rpath, file.string()});
            if (r.spawned && r.exit_code == 0) {
                changed = true;
                SPDLOG_DEBUG("relocated rpath: {} -> {}", rpath, new_rpath);
            } else {
                SPDLOG_WARN("install_name_tool -rpath failed for {}: {}", file.string(), r.output);
            }
        }

        if (changed) {
            codesign_adhoc(file);
            ++count;
        }
    }
    return count;
}
#endif // __APPLE__

} // namespace

std::string expand_homebrew_placeholders(std::string path, const std::string& prefix,
                                         const std::string& cellar) {
    replace_all(path, "@@HOMEBREW_PREFIX@@", prefix);
    replace_all(path, "@@HOMEBREW_CELLAR@@", cellar);
    replace_all(path, "@@HOMEBREW_REPOSITORY@@", prefix);
    replace_all(path, "@@HOMEBREW_LIBRARY@@", prefix + "/Library");
    return path;
}

uint32_t relocate_text_placeholders(const fs::path& dir, const std::string& prefix,
                                    const std::string& cellar) {
    uint32_t count = 0;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (!entry.is_regular_file())
            continue;

        // Skip binary files.
        auto ext = entry.path().extension().string();
        if (ext == ".dylib" || ext == ".so" || ext == ".a" || ext == ".o" || ext == ".bundle")
            continue;

        if (!is_text_file(entry.path()))
            continue;

        auto content = read_file(entry.path());
        auto expanded = expand_homebrew_placeholders(content, prefix, cellar);
        if (expanded != content) {
            write_file(entry.path(), expanded);
            ++count;
            SPDLOG_DEBUG("relocated text: {}", entry.path().string());
        }
    }
    return count;
}

uint32_t relocate_binary_paths(const fs::path& dir, const std::string& old_path,
                               const std::string& new_path) {
    uint32_t count = 0;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (!entry.is_regular_file())
            continue;

        auto ext = entry.path().extension().string();
        if (ext != ".dylib" && ext != ".so" && ext != ".bundle" && ext != "")
            continue;

        // Only process binary files.
        if (is_text_file(entry.path()))
            continue;

        auto data = read_file(entry.path());
        if (replace_binary(data, old_path, new_path)) {
            write_file(entry.path(), data);
            ++count;
            SPDLOG_DEBUG("relocated binary: {}", entry.path().string());
        }
    }
    return count;
}

uint32_t fix_dylib_paths(const fs::path& dir, const fs::path& prefix) {
    // Legacy helper: rewrite install names to @rpath. Not used by
    // relocate_bottle anymore — shared-Cellar bottles need placeholder
    // expansion to absolute /opt/homebrew paths, not @rpath rewriting (which
    // also invalidates code signatures without fixing @@HOMEBREW_*@@ loads).
    (void)dir;
    (void)prefix;
    return 0;
}

void relocate_bottle(const fs::path& package_dir, const std::string& name,
                     const std::string& version, const fs::path& store) {
    (void)version;
    // @@HOMEBREW_CELLAR@@ in bottles is followed by /<name>/<version>,
    // so we replace it with the store path (not including name/version).
    // With shared Cellar (/opt/homebrew/Cellar), placeholders expand to the
    // pour location. Text files use ordinary string replace; Mach-O load
    // commands use install_name_tool because CELLAR can grow (19 → 20 bytes
    // for /opt/homebrew/Cellar) and mid-path null-padding would truncate.
    auto cellar_path = store.string();
    auto prefix_path = store.parent_path().string(); // /opt/homebrew

    auto text_count = relocate_text_placeholders(package_dir, prefix_path, cellar_path);
#ifdef __APPLE__
    auto macho_count = fix_macho_placeholders(package_dir, prefix_path, cellar_path);
#else
    uint32_t macho_count = 0;
#endif

    if (text_count > 0 || macho_count > 0) {
        SPDLOG_INFO("relocated {}: {} text files, {} mach-o files", name, text_count, macho_count);
    }
}

void relocate_ruby(const fs::path& ruby_dir, const std::string& original_prefix) {
    auto new_prefix = ruby_dir.string();

    if (new_prefix.size() > original_prefix.size()) {
        SPDLOG_ERROR("new Ruby prefix ({}) is longer than original ({}) — cannot relocate binary",
                     new_prefix, original_prefix);
        SPDLOG_ERROR("try installing den to a shorter path");
        return;
    }

    // Relocate the Ruby binary itself.
    auto ruby_bin = ruby_dir / "ruby" / "bin" / "ruby";
    if (fs::exists(ruby_bin)) {
        auto data = read_file(ruby_bin);
        if (replace_binary(data, original_prefix, new_prefix)) {
            write_file(ruby_bin, data);
            SPDLOG_INFO("relocated Ruby binary: {} -> {}", original_prefix, new_prefix);
        }
    }

    // Also relocate any .rb files that reference the original prefix.
    relocate_text_placeholders(ruby_dir, new_prefix, new_prefix);

    // Relocate rbconfig.rb if it exists in the stdlib.
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(ruby_dir, ec)) {
        if (entry.path().filename() == "rbconfig.rb") {
            auto content = read_file(entry.path());
            if (replace_all(content, original_prefix, new_prefix)) {
                write_file(entry.path(), content);
                SPDLOG_INFO("relocated rbconfig.rb");
            }
        }
    }
}

} // namespace den

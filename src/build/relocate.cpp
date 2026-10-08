// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "relocate.h"

#include "core/error.h"
#include "provider/exec.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

namespace den {

// Defined below; ELF relocation in the anonymous namespace calls it.
std::string expand_homebrew_placeholders(std::string path, const std::string& prefix,
                                         const std::string& cellar);

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

/// True if the file starts with the ELF magic number.
bool is_elf_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    unsigned char magic[4] = {};
    f.read(reinterpret_cast<char*>(magic), 4);
    return f.gcount() == 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' &&
           magic[3] == 'F';
}

/// Trim trailing whitespace/newlines from a tool's single-line output.
std::string trim_line(std::string s) {
    while (!s.empty() &&
           (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) {
        ++start;
    }
    return s.substr(start);
}

/// Best-effort path to the host dynamic linker (used when the bottle's
/// expanded @@HOMEBREW_PREFIX@@/lib/ld.so is absent — e.g. harness containers
/// that pour into the Linuxbrew prefix without a full Homebrew glibc).
std::string system_elf_interpreter() {
    const char* candidates[] = {
        "/lib64/ld-linux-x86-64.so.2",  "/lib/ld-linux-x86-64.so.2", "/lib/ld-linux-aarch64.so.1",
        "/lib64/ld-linux-aarch64.so.1", "/lib/ld-linux.so.2",        "/lib/ld64.so.1",
    };
    for (const char* c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec) && !ec) {
            return c;
        }
    }
    return {};
}

/// Expand @@HOMEBREW_*@@ placeholders in ELF PT_INTERP and DT_RPATH/RUNPATH
/// via patchelf. Paths grow on Linux (@@HOMEBREW_PREFIX@@ is 19 bytes;
/// /home/linuxbrew/.linuxbrew is 26), so in-place null-pad patching cannot
/// work — matching Homebrew's use of patchelf for keg relocation.
uint32_t fix_elf_placeholders(const fs::path& dir, const std::string& prefix,
                              const std::string& cellar) {
    uint32_t count = 0;
    bool patchelf_checked = false;
    bool patchelf_ok = false;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        auto ext = entry.path().extension().string();
        if (ext == ".h" || ext == ".pc" || ext == ".cmake" || ext == ".rb" || ext == ".txt" ||
            ext == ".md" || ext == ".json" || ext == ".1" || ext == ".3" || ext == ".a") {
            continue;
        }
        if (!is_elf_file(entry.path())) {
            continue;
        }

        // Defer the patchelf presence check until we know this ELF actually
        // carries @@HOMEBREW_*@@ placeholders — most trees have no such files
        // on macOS, and we must not warn/fail there. Once a placeholder is
        // found, missing or failing patchelf is a hard pour error (otherwise
        // the install "succeeds" with binaries that exit 127 at runtime).
        const auto file = entry.path();

        // Probe placeholders via strings in the file when patchelf is not yet
        // confirmed, so we can fail before attempting rewrites. Prefer
        // patchelf --print-* once available (handles compressed sections etc.).
        auto needs_reloc = [&](const std::string& interp, const std::string& rpath) {
            return (!interp.empty() && has_homebrew_placeholder(interp)) ||
                   (!rpath.empty() && has_homebrew_placeholder(rpath));
        };

        std::string interp;
        std::string rpath;
        if (patchelf_ok) {
            auto interp_r = run_tool({"patchelf", "--print-interpreter", file.string()});
            if (interp_r.spawned && interp_r.exit_code == 0) {
                interp = trim_line(interp_r.output);
            }
            auto rpath_r = run_tool({"patchelf", "--print-rpath", file.string()});
            if (rpath_r.spawned && rpath_r.exit_code == 0) {
                rpath = trim_line(rpath_r.output);
            }
        } else {
            // Cheap scan: placeholders live as plain C strings in the dynamic
            // section. Avoids requiring patchelf just to detect the need.
            auto data = read_file(file);
            if (has_homebrew_placeholder(data)) {
                // Synthesize non-empty markers so needs_reloc is true; the real
                // values are read after patchelf is confirmed.
                interp = "@@HOMEBREW_PREFIX@@/lib/ld.so";
                rpath = "@@HOMEBREW_PREFIX@@/lib";
            }
        }

        if (!needs_reloc(interp, rpath)) {
            continue;
        }

        if (!patchelf_checked) {
            patchelf_checked = true;
            auto ver = run_tool({"patchelf", "--version"});
            patchelf_ok = ver.spawned && ver.exit_code == 0;
        }
        if (!patchelf_ok) {
            throw UserError(
                "patchelf is required to relocate Linux bottles (apt install patchelf); "
                "found unresolved @@HOMEBREW_*@@ placeholders in " +
                file.string());
        }

        // Re-read with patchelf now that we know it works (first file may have
        // used the string-scan path above).
        {
            auto interp_r = run_tool({"patchelf", "--print-interpreter", file.string()});
            if (interp_r.spawned && interp_r.exit_code == 0) {
                interp = trim_line(interp_r.output);
            } else {
                interp.clear();
            }
            auto rpath_r = run_tool({"patchelf", "--print-rpath", file.string()});
            if (rpath_r.spawned && rpath_r.exit_code == 0) {
                rpath = trim_line(rpath_r.output);
            } else {
                rpath.clear();
            }
        }
        if (!needs_reloc(interp, rpath)) {
            // String scan found @@HOMEBREW_*@@ but patchelf reports none —
            // still a hard error: the binary needs relocation and we cannot
            // do it (corrupt ELF, stub tool, etc.).
            throw UserError("patchelf could not read @@HOMEBREW_*@@ interpreter/RPATH from '" +
                            file.string() +
                            "' (placeholders are present in the file; install a working patchelf)");
        }

        bool changed = false;

        // Bottles ship with mode 555/444 binaries; patchelf must open O_RDWR.
        {
            std::error_code perm_ec;
            fs::permissions(file,
                            fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec |
                                fs::perms::group_read | fs::perms::group_exec |
                                fs::perms::others_read | fs::perms::others_exec,
                            fs::perm_options::add, perm_ec);
            if (perm_ec) {
                throw UserError("cannot make '" + file.string() +
                                "' writable for patchelf: " + perm_ec.message());
            }
        }

        // PT_INTERP — bottles ship with @@HOMEBREW_PREFIX@@/lib/ld.so.
        if (!interp.empty() && has_homebrew_placeholder(interp)) {
            auto new_interp = expand_homebrew_placeholders(interp, prefix, cellar);
            std::error_code exists_ec;
            if (!fs::exists(new_interp, exists_ec)) {
                auto sys = system_elf_interpreter();
                if (!sys.empty()) {
                    SPDLOG_DEBUG("ELF interpreter {} missing; falling back to {}", new_interp, sys);
                    new_interp = sys;
                }
            }
            if (new_interp != interp) {
                auto r = run_tool({"patchelf", "--set-interpreter", new_interp, file.string()});
                if (!(r.spawned && r.exit_code == 0)) {
                    throw UserError("patchelf --set-interpreter failed for '" + file.string() +
                                    "' (" + interp + " -> " + new_interp + "): " +
                                    (r.output.empty() ? "unknown error" : trim_line(r.output)));
                }
                changed = true;
                SPDLOG_DEBUG("relocated ELF interpreter: {} -> {}", interp, new_interp);
            }
        }

        // DT_RPATH / DT_RUNPATH.
        if (!rpath.empty() && has_homebrew_placeholder(rpath)) {
            auto new_rpath = expand_homebrew_placeholders(rpath, prefix, cellar);
            if (new_rpath != rpath) {
                auto r = run_tool({"patchelf", "--set-rpath", new_rpath, file.string()});
                if (!(r.spawned && r.exit_code == 0)) {
                    throw UserError("patchelf --set-rpath failed for '" + file.string() + "': " +
                                    (r.output.empty() ? "unknown error" : trim_line(r.output)));
                }
                changed = true;
                SPDLOG_DEBUG("relocated ELF rpath: {} -> {}", rpath, new_rpath);
            }
        }

        if (changed) {
            ++count;
        }
    }
    return count;
}

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

        // Never rewrite ELF as text — bottles embed @@HOMEBREW_*@@ in dynamic
        // sections that must go through patchelf (and a naive string replace
        // would also corrupt the binary if lengths differ).
        {
            std::ifstream ef(entry.path(), std::ios::binary);
            unsigned char m[4] = {};
            ef.read(reinterpret_cast<char*>(m), 4);
            if (ef.gcount() == 4 && m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F') {
                continue;
            }
        }

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
    // Linux ELF bottles embed @@HOMEBREW_PREFIX@@ in PT_INTERP and RPATH;
    // those strings grow (/home/linuxbrew/.linuxbrew is longer than the
    // placeholder), so we rewrite them with patchelf — without this, the
    // kernel reports "required file not found" for the literal placeholder
    // interpreter path (harness Linux jq failure).
    auto cellar_path = store.string();
    auto prefix_path = store.parent_path().string(); // /opt/homebrew or linuxbrew prefix

    auto text_count = relocate_text_placeholders(package_dir, prefix_path, cellar_path);
#ifdef __APPLE__
    auto macho_count = fix_macho_placeholders(package_dir, prefix_path, cellar_path);
#else
    uint32_t macho_count = 0;
#endif
    auto elf_count = fix_elf_placeholders(package_dir, prefix_path, cellar_path);

    if (text_count > 0 || macho_count > 0 || elf_count > 0) {
        SPDLOG_INFO("relocated {}: {} text files, {} mach-o files, {} elf files", name, text_count,
                    macho_count, elf_count);
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

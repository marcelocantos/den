// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "relocate.h"

#include "core/error.h"
#include "core/self_exe.h"
#include "provider/exec.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
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
    unsigned char header[8] = {};
    f.read(reinterpret_cast<char*>(header), sizeof header);
    return looks_like_macho_header(header, static_cast<std::size_t>(f.gcount()));
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
    auto result = run_tool({macho_tool("otool"), "-L", file.string()});
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
    auto result = run_tool({macho_tool("otool"), "-D", file.string()});
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
    auto result = run_tool({macho_tool("otool"), "-l", file.string()});
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
    // Never re-sign den's own running binary: relocating a keg must not
    // change den's code identity (launchd records it for dev.den.daemon, and
    // a changed signature makes the next spawn fail with
    // OS_REASON_CODESIGNING).
    if (is_den_own_binary(file, current_executable_path())) {
        SPDLOG_WARN("not re-signing den's own binary {}", file.string());
        return;
    }
    // Arm64 macOS kills modified Mach-O binaries with an invalid signature
    // (SIGKILL, no output). Re-sign ad-hoc after install_name_tool, matching
    // Homebrew's post-relocate step.
    auto result = run_tool({"codesign", "--force", "--sign", "-", file.string()});
    if (!result.spawned || result.exit_code != 0) {
        SPDLOG_WARN("codesign ad-hoc failed for {}: {}", file.string(), result.output);
    }
}

[[noreturn]] void throw_header_space(const fs::path& file, const std::string& from,
                                     const std::string& to, const std::string& output) {
    throw UserError("cannot relocate " + file.string() + ": the rewritten load command\n  " + from +
                    "\n  -> " + to +
                    "\ndoes not fit in the binary's Mach-O header space (den's prefix is longer "
                    "than the one the bottle was built for). The keg was not installed.\n"
                    "install_name_tool: " +
                    output);
}

/// Expand @@HOMEBREW_*@@ placeholders in Mach-O load commands, dylib ids, and
/// rpaths via install_name_tool. Length may grow (CELLAR placeholder is 19
/// bytes; /opt/homebrew/Cellar is 20), so in-place null-pad patching is not
/// sufficient for load commands.
uint32_t fix_macho_placeholders(const fs::path& dir, const std::string& prefix,
                                const std::string& cellar, const std::string& self_name) {
    auto rewrite = [&](const std::string& n) {
        return expand_homebrew_placeholders(pin_opt_reference(n, cellar, self_name, dir), prefix,
                                            cellar);
    };
    uint32_t count = 0;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (entry.is_symlink() || !entry.is_regular_file()) {
            continue;
        }
        // Decide by magic only. An extension skip-list misfires on versioned
        // executables: gawk ships bin/gawk-5.4.1 (extension ".1"), which was
        // skipped and left with @@HOMEBREW_PREFIX@@ load commands.
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
            auto new_id = rewrite(id);
            if (new_id != id) {
                auto r = run_tool({macho_tool("install_name_tool"), "-id", new_id, file.string()});
                if (r.spawned && r.exit_code == 0) {
                    changed = true;
                    SPDLOG_DEBUG("relocated dylib id: {} -> {}", id, new_id);
                } else if (install_name_tool_out_of_header_space(r.output)) {
                    throw_header_space(file, id, new_id, r.output);
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
            auto new_name = rewrite(name);
            if (new_name == name) {
                continue;
            }
            auto r = run_tool(
                {macho_tool("install_name_tool"), "-change", name, new_name, file.string()});
            if (r.spawned && r.exit_code == 0) {
                changed = true;
                SPDLOG_DEBUG("relocated install name: {} -> {}", name, new_name);
            } else if (install_name_tool_out_of_header_space(r.output)) {
                throw_header_space(file, name, new_name, r.output);
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
            auto new_rpath = rewrite(rpath);
            if (new_rpath == rpath) {
                continue;
            }
            auto r = run_tool(
                {macho_tool("install_name_tool"), "-rpath", rpath, new_rpath, file.string()});
            if (r.spawned && r.exit_code == 0) {
                changed = true;
                SPDLOG_DEBUG("relocated rpath: {} -> {}", rpath, new_rpath);
            } else if (install_name_tool_out_of_header_space(r.output)) {
                throw_header_space(file, rpath, new_rpath, r.output);
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

/// After relocation, every Mach-O load name, id and rpath must be free of
/// @@HOMEBREW_*@@ placeholders; otherwise dyld fails at runtime with an
/// opaque "Symbol not found … Expected in: unknown". Returns offending
/// "<file>: <name>" entries (or a tool-failure note).
std::vector<std::string> unresolved_macho_placeholders(const fs::path& dir) {
    std::vector<std::string> bad;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (entry.is_symlink() || !entry.is_regular_file() || !is_macho_file(entry.path())) {
            continue;
        }
        auto r = run_tool({macho_tool("otool"), "-L", entry.path().string()});
        if (!r.spawned || r.exit_code != 0) {
            bad.push_back(entry.path().string() + ": otool failed (" + r.output.substr(0, 120) +
                          ")");
            continue;
        }
        std::vector<std::string> names = otool_load_names(entry.path());
        for (const auto& rp : otool_rpaths(entry.path())) {
            names.push_back(rp);
        }
        for (const auto& n : names) {
            if (has_homebrew_placeholder(n)) {
                bad.push_back(entry.path().string() + ": " + n);
            }
        }
    }
    return bad;
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
        if (entry.is_symlink() || !entry.is_regular_file()) {
            continue;
        }
        // Magic check only (see fix_macho_placeholders): versioned names such
        // as bin/gawk-5.4.1 or libfoo.so.1 must not be skipped by extension.
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

/// install_name_tool's report that a rewritten load command no longer fits
/// in the binary's header padding ("larger updated load commands do not
/// fit"). Brew bottles are built for their standard prefix, so a longer den
/// prefix can exhaust the padding.
bool install_name_tool_out_of_header_space(const std::string& output) {
    return output.find("load commands do not fit") != std::string::npos ||
           output.find("headerpad") != std::string::npos;
}

std::string expand_homebrew_placeholders(std::string path, const std::string& prefix,
                                         const std::string& cellar) {
    replace_all(path, "@@HOMEBREW_PREFIX@@", prefix);
    replace_all(path, "@@HOMEBREW_CELLAR@@", cellar);
    replace_all(path, "@@HOMEBREW_REPOSITORY@@", prefix);
    replace_all(path, "@@HOMEBREW_LIBRARY@@", prefix + "/Library");
    return path;
}

std::string homebrew_perl_path(const std::string& prefix, const std::string& name,
                               const std::vector<std::string>& deps) {
    const std::string brewed = prefix + "/opt/perl/bin/perl";
#ifdef __APPLE__
    // extend/os/mac/keg_relocate.rb: brewed perl only when declared directly.
    if (name == "perl" || std::find(deps.begin(), deps.end(), "perl") != deps.end()) {
        return brewed;
    }
    // Otherwise MacOS.preferred_perl_version (5.34 on Sonoma+, 5.30 before).
    // Den has no bottle tab (built_on.preferred_perl), so probe what exists.
    for (const char* candidate : {"/usr/bin/perl5.34", "/usr/bin/perl5.30"}) {
        std::error_code ec;
        if (fs::exists(candidate, ec)) {
            return candidate;
        }
    }
    return "/usr/bin/perl";
#else
    (void)name;
    (void)deps;
    return brewed;
#endif
}

std::string homebrew_java_path(const std::string& prefix, const std::vector<std::string>& deps) {
    // Keg#openjdk_dep_name_if_applicable: first dep matching ^openjdk(@\d[\d.]*)?$.
    static const std::regex openjdk_re(R"(^openjdk(@\d[\d.]*)?$)");
    std::string jdk = "openjdk";
    for (const auto& d : deps) {
        if (std::regex_match(d, openjdk_re)) {
            jdk = d;
            break;
        }
    }
#ifdef __APPLE__
    return prefix + "/opt/" + jdk + "/libexec/openjdk.jdk/Contents/Home";
#else
    return prefix + "/opt/" + jdk + "/libexec";
#endif
}

uint32_t relocate_text_placeholders(const fs::path& dir, const std::string& prefix,
                                    const std::string& cellar, const std::string& java,
                                    const std::string& perl) {
    uint32_t count = 0;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (entry.is_symlink() || !entry.is_regular_file())
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
        if (!java.empty()) {
            replace_all(expanded, "@@HOMEBREW_JAVA@@", java);
        }
        if (!perl.empty()) {
            replace_all(expanded, "@@HOMEBREW_PERL@@", perl);
        }
        if (expanded != content) {
            write_file(entry.path(), expanded);
            ++count;
            SPDLOG_DEBUG("relocated text: {}", entry.path().string());
        }
    }
    return count;
}

namespace {
/// True for extensions like ".1" or ".3" that are really version suffixes
/// (tclsh9.1, emacs-31.1, gforth-0.7.3).
bool is_version_suffix(const std::string& ext) {
    return ext.size() > 1 && ext.find_first_not_of("0123456789", 1) == std::string::npos;
}
} // namespace

uint32_t relocate_binary_paths(const fs::path& dir, const std::string& old_path,
                               const std::string& new_path) {
    uint32_t count = 0;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (entry.is_symlink() || !entry.is_regular_file())
            continue;

        auto ext = entry.path().extension().string();
        if (ext != ".dylib" && ext != ".so" && ext != ".bundle" && ext != "" &&
            !is_version_suffix(ext))
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

int compare_keg_versions(const std::string& a, const std::string& b) {
    auto split = [](const std::string& v) {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : v) {
            if (c == '.' || c == '_' || c == '-') {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        parts.push_back(cur);
        return parts;
    };
    auto pa = split(a), pb = split(b);
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        const std::string x = i < pa.size() ? pa[i] : "";
        const std::string y = i < pb.size() ? pb[i] : "";
        const bool xn = !x.empty() && std::all_of(x.begin(), x.end(), ::isdigit);
        const bool yn = !y.empty() && std::all_of(y.begin(), y.end(), ::isdigit);
        if (xn && yn) {
            const auto xv = std::stoull(x), yv = std::stoull(y);
            if (xv != yv) {
                return xv < yv ? -1 : 1;
            }
        } else if (x != y) {
            if (x.empty()) {
                return -1;
            }
            if (y.empty()) {
                return 1;
            }
            return x < y ? -1 : 1;
        }
    }
    return 0;
}

fs::path select_dep_keg(const fs::path& cellar, const std::string& dep) {
    std::error_code ec;
    const fs::path base = cellar / dep;
    if (!fs::is_directory(base, ec)) {
        return {};
    }
    fs::path best;
    std::string best_ver;
    for (const auto& e : fs::directory_iterator(base, ec)) {
        if (!e.is_directory()) {
            continue;
        }
        // Skip malformed kegs whose only content is one nested version
        // directory (e.g. 1.9.7/1.9.7_2/…, from the old revisioned-pour bug):
        // binding to them breaks dyld lookups.
        const auto kver = e.path().filename().string();
        size_t n = 0;
        bool nested_version = false;
        for (const auto& c : fs::directory_iterator(e.path(), ec)) {
            ++n;
            nested_version = c.is_directory() && c.path().filename().string().starts_with(kver);
        }
        if (n == 0 || (n == 1 && nested_version)) {
            continue;
        }
        const auto ver = e.path().filename().string();
        if (best.empty() || compare_keg_versions(ver, best_ver) > 0) {
            best = e.path();
            best_ver = ver;
        }
    }
    return best;
}

std::string pin_opt_reference(const std::string& name, const fs::path& cellar,
                              const std::string& self_name, const fs::path& self_dir) {
    static constexpr std::string_view kOpt = "@@HOMEBREW_PREFIX@@/opt/";
    if (!name.starts_with(kOpt)) {
        return name;
    }
    const auto rest = name.substr(kOpt.size());
    const auto slash = rest.find('/');
    const std::string dep = rest.substr(0, slash);
    const std::string tail = slash == std::string::npos ? "" : rest.substr(slash);
    fs::path keg = dep == self_name ? self_dir : select_dep_keg(cellar, dep);
    if (keg.empty()) {
        SPDLOG_WARN("relocate: no keg for dependency '{}' in {}; leaving opt reference", dep,
                    cellar.string());
        return name;
    }
    return keg.string() + tail;
}

std::string macho_tool(const std::string& tool) {
    static std::map<std::string, std::string> cache;
    if (auto it = cache.find(tool); it != cache.end()) {
        return it->second;
    }
    std::string chosen = tool;
#ifdef __APPLE__
    // Probe the PATH shim against a binary that is always present.
    const std::vector<std::string> probe =
        tool == "otool" ? std::vector<std::string>{tool, "-L", "/usr/lib/libSystem.B.dylib"}
                        : std::vector<std::string>{tool};
    auto r = run_tool(probe);
    // install_name_tool with no args prints usage and exits non-zero, so
    // treat "spawned and no xcode-select/loader error" as working.
    const bool shim_broken =
        !r.spawned || r.output.find("xcode-select") != std::string::npos ||
        r.output.find("Error loading required libraries") != std::string::npos ||
        (tool == "otool" && r.exit_code != 0);
    const std::string clt = "/Library/Developer/CommandLineTools/usr/bin/" + tool;
    std::error_code ec;
    if (shim_broken && fs::exists(clt, ec)) {
        SPDLOG_WARN("{} from PATH is unusable (stale Xcode selection?); using {}", tool, clt);
        chosen = clt;
    }
#endif
    cache[tool] = chosen;
    return chosen;
}

void relocate_bottle(const fs::path& package_dir, const std::string& name,
                     const std::string& version, const fs::path& store,
                     const RelocationOptions& options) {
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

    auto text_count =
        relocate_text_placeholders(package_dir, prefix_path, cellar_path,
                                   homebrew_java_path(prefix_path, options.dependencies),
                                   homebrew_perl_path(prefix_path, name, options.dependencies));
    if (options.skip_linkage) {
        // :any_skip_relocation — brew replaces text placeholders (scripts,
        // .pm/.pc files, launchers) but never touches dynamic linkage.
        if (text_count > 0) {
            SPDLOG_INFO("relocated {}: {} text files (linkage skipped)", name, text_count);
        }
        return;
    }
#ifdef __APPLE__
    auto macho_count = fix_macho_placeholders(package_dir, prefix_path, cellar_path, name);
    if (auto bad = unresolved_macho_placeholders(package_dir); !bad.empty()) {
        std::string msg = "relocation of '" + name +
                          "' left unresolved Homebrew placeholders "
                          "(the keg would fail to load):";
        for (size_t i = 0; i < bad.size() && i < 5; ++i) {
            msg += "\n  " + bad[i];
        }
        msg += "\nCheck that otool/install_name_tool work (xcode-select -p; "
               "xcode-select --install).";
        throw Error(msg);
    }
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

bool looks_like_macho_header(const unsigned char* header, std::size_t size) {
    if (size < 4)
        return false;
    auto be32 = [&](std::size_t off) {
        return (uint32_t(header[off]) << 24) | (uint32_t(header[off + 1]) << 16) |
               (uint32_t(header[off + 2]) << 8) | uint32_t(header[off + 3]);
    };
    switch (be32(0)) {
    case 0xFEEDFACE: // MH_MAGIC
    case 0xCEFAEDFE: // MH_CIGAM
    case 0xFEEDFACF: // MH_MAGIC_64
    case 0xCFFAEDFE: // MH_CIGAM_64
        return true;
    case 0xCAFEBABE: // FAT_MAGIC (big-endian on disk)
    case 0xCAFEBABF: // FAT_MAGIC_64
    {
        // Java .class files share 0xCAFEBABE. In a fat header the next word
        // is nfat_arch (small); in a class file it is minor<<16|major with
        // major >= 45. Same heuristic as file(1).
        if (size < 8)
            return false;
        const uint32_t nfat = be32(4);
        return nfat > 0 && nfat < 20;
    }
    default:
        return false;
    }
}

bool is_den_own_binary(const fs::path& file, const std::optional<fs::path>& self_exe) {
    if (!self_exe)
        return false;
    std::error_code ec;
    if (fs::equivalent(file, *self_exe, ec) && !ec)
        return true;
    // Compare resolved paths too, in case one of them no longer exists.
    auto a = fs::weakly_canonical(file, ec);
    if (ec)
        return false;
    auto b = fs::weakly_canonical(*self_exe, ec);
    return !ec && a == b;
}

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "relocate.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
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

} // namespace

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
        bool changed = false;
        changed |= replace_all(content, "@@HOMEBREW_PREFIX@@", prefix);
        changed |= replace_all(content, "@@HOMEBREW_CELLAR@@", cellar);
        changed |= replace_all(content, "@@HOMEBREW_REPOSITORY@@", prefix);
        changed |= replace_all(content, "@@HOMEBREW_LIBRARY@@", prefix + "/Library");

        if (changed) {
            write_file(entry.path(), content);
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
    uint32_t count = 0;
    std::error_code ec;
    auto lib_dir = prefix / "lib";

    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (!entry.is_regular_file() && !entry.is_symlink())
            continue;

        auto ext = entry.path().extension().string();
        bool is_dylib = (ext == ".dylib");
        bool is_executable = false;

        if (!is_dylib) {
            // Check if it's an executable in bin/ or sbin/.
            auto rel = fs::relative(entry.path(), dir);
            auto first_component = rel.begin()->string();
            if ((first_component == "bin" || first_component == "sbin") &&
                fs::is_regular_file(entry.path()) &&
                (fs::status(entry.path()).permissions() & fs::perms::owner_exec) !=
                    fs::perms::none) {
                is_executable = true;
            }
        }

        if (!is_dylib && !is_executable)
            continue;

#ifdef __APPLE__
        if (is_dylib) {
            auto basename = entry.path().filename().string();
            std::system(
                ("install_name_tool -id @rpath/" + basename + " " + entry.path().string() +
                 " 2>/dev/null")
                    .c_str());
        }

        // Add rpath for this package's lib.
        std::system(("install_name_tool -add_rpath " + lib_dir.string() + " " +
                     entry.path().string() + " 2>/dev/null")
                        .c_str());
        ++count;
#endif
    }
    return count;
}

void relocate_bottle(const fs::path& package_dir, const std::string& name,
                     const std::string& version, const fs::path& store) {
    // @@HOMEBREW_CELLAR@@ in bottles is followed by /<name>/<version>,
    // so we replace it with the store path (not including name/version).
    // Example: @@HOMEBREW_CELLAR@@/readline/8.3.3 → ~/.den/store/readline/8.3.3
    auto cellar_path = store.string();
    auto prefix_path = store.parent_path().string(); // ~/.den

    auto text_count = relocate_text_placeholders(package_dir, prefix_path, cellar_path);
    auto dylib_count = fix_dylib_paths(package_dir, package_dir);

    if (text_count > 0 || dylib_count > 0) {
        SPDLOG_INFO("relocated {}: {} text files, {} dylibs", name, text_count, dylib_count);
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

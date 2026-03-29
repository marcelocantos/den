// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "formula_parser.h"

#include <spdlog/spdlog.h>

#include <regex>
#include <sstream>

namespace den {

namespace {

/// Extract the install method body from formula source.
std::string extract_install_body(const std::string& source) {
    // Find "def install" and extract until the matching "end".
    auto pos = source.find("def install");
    if (pos == std::string::npos)
        return "";

    pos = source.find('\n', pos);
    if (pos == std::string::npos)
        return "";
    ++pos;

    // Track nesting depth to find the matching end.
    int depth = 1;
    auto start = pos;
    while (pos < source.size() && depth > 0) {
        auto line_end = source.find('\n', pos);
        if (line_end == std::string::npos)
            line_end = source.size();

        auto line = source.substr(pos, line_end - pos);

        // Trim leading whitespace.
        auto trimmed = line;
        auto first = trimmed.find_first_not_of(" \t");
        if (first != std::string::npos)
            trimmed = trimmed.substr(first);

        // Count nesting (simplified — works for Homebrew formulas).
        if (trimmed.starts_with("def ") || trimmed.starts_with("do") ||
            trimmed.starts_with("if ") || trimmed.starts_with("unless ") ||
            trimmed.starts_with("case ") || trimmed.starts_with("begin")) {
            if (!trimmed.contains(" end") && !trimmed.ends_with(" end"))
                ++depth;
        }
        if (trimmed == "end" || trimmed.starts_with("end ") || trimmed.starts_with("end#")) {
            --depth;
        }

        pos = line_end + 1;
    }

    return source.substr(start, pos - start - 1);
}

/// Expand Homebrew's std_configure_args for a given prefix.
std::string std_configure_args(const std::string& prefix, const std::string& lib) {
    return "--disable-debug --disable-dependency-tracking --prefix=" + prefix + " --libdir=" + lib;
}

/// Expand Homebrew's std_cmake_args for a given prefix.
std::string std_cmake_args(const std::string& prefix) {
    return "-DCMAKE_INSTALL_PREFIX=" + prefix +
           " -DCMAKE_BUILD_TYPE=Release"
           " -DCMAKE_FIND_FRAMEWORK=LAST"
           " -DCMAKE_VERBOSE_MAKEFILE=ON"
           " -Wno-dev"
           " -DBUILD_TESTING=OFF";
}

/// Expand std_meson_args.
std::string std_meson_args(const std::string& prefix, const std::string& lib) {
    return "--prefix=" + prefix + " --libdir=" + lib +
           " --buildtype=release --wrap-mode=nofallback";
}

/// Parse a Ruby system() call into a shell command string.
/// Handles: system "cmd", "arg1", "arg2", *std_configure_args
std::string parse_system_call(const std::string& line, const std::string& prefix,
                              const std::string& lib, const std::string& name) {
    // Extract everything after "system "
    auto sys_pos = line.find("system ");
    if (sys_pos == std::string::npos)
        return "";

    auto args_str = line.substr(sys_pos + 7);

    // Comments are already stripped during line joining.
    // Split by comma, handling quoted strings.
    std::vector<std::string> parts;
    std::string current;
    bool in_quote = false;
    for (size_t i = 0; i < args_str.size(); ++i) {
        char c = args_str[i];
        if (c == '"' && (i == 0 || args_str[i - 1] != '\\')) {
            in_quote = !in_quote;
        } else if (c == ',' && !in_quote) {
            parts.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty())
        parts.push_back(current);

    // Process each part.
    std::string cmd;
    for (auto& part : parts) {
        // Trim whitespace.
        auto start = part.find_first_not_of(" \t\n");
        if (start == std::string::npos)
            continue;
        part = part.substr(start);
        auto end = part.find_last_not_of(" \t\n");
        if (end != std::string::npos)
            part = part.substr(0, end + 1);

        // Handle special expansions.
        if (part == "*std_configure_args") {
            cmd += " " + std_configure_args(prefix, lib);
        } else if (part == "*std_cmake_args") {
            cmd += " " + std_cmake_args(prefix);
        } else if (part == "*std_meson_args") {
            cmd += " " + std_meson_args(prefix, lib);
        } else {
            // Substitute #{prefix}, #{lib}, #{bin}, etc.
            std::string val = part;
            auto replace_all = [](std::string& s, const std::string& from,
                                  const std::string& to) {
                size_t pos = 0;
                while ((pos = s.find(from, pos)) != std::string::npos) {
                    s.replace(pos, from.size(), to);
                    pos += to.size();
                }
            };

            replace_all(val, "#{prefix}", prefix);
            replace_all(val, "#{lib}", lib + "/lib");
            replace_all(val, "#{bin}", prefix + "/bin");
            replace_all(val, "#{include}", prefix + "/include");
            replace_all(val, "#{share}", prefix + "/share");
            replace_all(val, "#{sbin}", prefix + "/sbin");
            replace_all(val, "#{libexec}", prefix + "/libexec");
            replace_all(val, "#{man}", prefix + "/share/man");
            replace_all(val, "#{pkgshare}", prefix + "/share/" + name);
            replace_all(val, "#{etc}", prefix + "/etc");
            replace_all(val, "#{var}", prefix + "/var");
            replace_all(val, "#{rpath}", "@loader_path/../lib");

            if (!cmd.empty())
                cmd += " ";
            cmd += val;
        }
    }

    return cmd;
}

} // namespace

ParsedFormula parse_formula(const std::string& brew_cat_output, const std::string& prefix,
                            const std::string& name) {
    ParsedFormula result;
    std::string lib = prefix + "/lib";

    // Extract source URL and SHA256.
    std::smatch match;
    std::regex url_re(R"RE(url\s+"([^"]+)")RE");
    if (std::regex_search(brew_cat_output, match, url_re))
        result.source_url = match[1].str();

    std::regex sha_re(R"RE(sha256\s+"([0-9a-f]{64})")RE");
    if (std::regex_search(brew_cat_output, match, sha_re))
        result.source_sha256 = match[1].str();

    // Parse the install method.
    auto body = extract_install_body(brew_cat_output);
    if (body.empty()) {
        SPDLOG_WARN("no install method found in formula for {}", name);
        return result;
    }

    // Join continuation lines (lines ending with comma).
    std::vector<std::string> lines;
    {
        std::istringstream raw(body);
        std::string raw_line;
        std::string accum;
        while (std::getline(raw, raw_line)) {
            auto first = raw_line.find_first_not_of(" \t");
            if (first == std::string::npos)
                continue;
            auto trimmed = raw_line.substr(first);
            // Remove inline comments (but not #{} Ruby interpolation).
            // A comment starts with " #" where # is NOT followed by {.
            for (size_t i = 1; i < trimmed.size(); ++i) {
                if (trimmed[i] == '#' && trimmed[i - 1] == ' ') {
                    // Skip #{} interpolation.
                    if (i + 1 < trimmed.size() && trimmed[i + 1] == '{')
                        continue;
                    // Check we're not inside a quoted string.
                    int quotes = 0;
                    for (size_t j = 0; j < i; ++j)
                        if (trimmed[j] == '"')
                            ++quotes;
                    if (quotes % 2 == 0) {
                        trimmed = trimmed.substr(0, i);
                        break;
                    }
                }
            }

            // Trim trailing whitespace after comment removal.
            while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
                trimmed.pop_back();

            if (accum.empty()) {
                accum = trimmed;
            } else {
                accum += " " + trimmed;
            }

            // If accumulated line ends with comma, it continues on the next line.
            auto accum_trimmed = accum;
            while (!accum_trimmed.empty() &&
                   (accum_trimmed.back() == ' ' || accum_trimmed.back() == '\t'))
                accum_trimmed.pop_back();
            if (!accum_trimmed.empty() && accum_trimmed.back() == ',') {
                accum = accum_trimmed;
                continue;
            }

            lines.push_back(accum_trimmed);
            accum.clear();
        }
        if (!accum.empty())
            lines.push_back(accum);
    }

    // Process joined lines.
    for (const auto& trimmed : lines) {

        // Skip comments, conditionals, blocks we can't handle.
        if (trimmed.starts_with("#"))
            continue;
        if (trimmed.starts_with("if ") || trimmed.starts_with("unless ") ||
            trimmed.starts_with("else") || trimmed.starts_with("end") ||
            trimmed.starts_with("do") || trimmed.starts_with("resource") ||
            trimmed.starts_with("(") || trimmed.starts_with("}") ||
            trimmed.starts_with("{") || trimmed.starts_with("|"))
            continue;

        // ENV modifications.
        if (trimmed.starts_with("ENV.append") || trimmed.starts_with("ENV.prepend") ||
            trimmed.starts_with("ENV[")) {
            // Extract key=value from ENV.append "KEY", "value"
            std::regex env_re(R"RE(ENV\.\w+\s+"(\w+)",\s+"([^"]+)")RE");
            if (std::regex_search(trimmed, match, env_re)) {
                result.env_settings.push_back(match[1].str() + "+=" + match[2].str());
            }
            continue;
        }

        // system calls — the main build commands.
        if (trimmed.starts_with("system ")) {
            auto cmd = parse_system_call(trimmed, prefix, lib, name);
            if (!cmd.empty()) {
                result.build_commands.push_back(cmd);
            }
            continue;
        }

        // inreplace — text substitution in files (skip for now).
        if (trimmed.starts_with("inreplace "))
            continue;

        // mkdir_p, cd, etc. — directory manipulation.
        if (trimmed.starts_with("mkdir")) {
            // mkdir_p "path" -> mkdir -p path
            std::regex mkdir_re(R"RE(mkdir_p\s+"([^"]+)")RE");
            if (std::regex_search(trimmed, match, mkdir_re)) {
                auto dir = match[1].str();
                // Substitute prefix vars.
                auto replace_prefix = [&](std::string& s) {
                    size_t p = 0;
                    while ((p = s.find("#{prefix}", p)) != std::string::npos)
                        s.replace(p, 9, prefix);
                };
                replace_prefix(dir);
                result.build_commands.push_back("mkdir -p " + dir);
            }
            continue;
        }
    }

    return result;
}

} // namespace den

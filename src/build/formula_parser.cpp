// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "formula_parser.h"

#include <spdlog/spdlog.h>

#include <regex>
#include <sstream>

namespace den {

namespace {

/// Extract the install method body from formula source.
/// True if `line` (already trimmed) opens a block whose matching `end`
/// lives on a later line. Ruby has two syntactic forms for this:
///
///   1. A leading keyword:  `def`, `if`, `unless`, `case`, `while`,
///      `until`, `for`, `begin`, `module`, `class`, or bare `do`.
///   2. A method call that yields a block, signalled by a trailing
///      ` do` or ` do |args|`  (e.g. `resource "x" do`,
///      `Dir.chdir("x") do`, `array.each do |i|`).
///
/// Self-closing single-line forms like `if x then y end` or
/// `foo { |x| x + 1 }` don't open a block and are filtered out by the
/// caller.
bool opens_block(const std::string& line) {
    auto is_word_boundary = [&](size_t after_kw) {
        if (after_kw >= line.size())
            return true;
        char c = line[after_kw];
        return c == ' ' || c == '\t' || c == '(' || c == ':' || c == '\0';
    };
    auto starts_with_keyword = [&](const char* kw) {
        auto n = std::string_view(kw).size();
        return line.size() >= n && line.compare(0, n, kw) == 0 && is_word_boundary(n);
    };

    // Leading keywords that open a block.
    if (starts_with_keyword("def") || starts_with_keyword("if") || starts_with_keyword("unless") ||
        starts_with_keyword("case") || starts_with_keyword("while") ||
        starts_with_keyword("until") || starts_with_keyword("for") ||
        starts_with_keyword("begin") || starts_with_keyword("module") ||
        starts_with_keyword("class"))
        return true;
    // Bare `do` at the start of a line (rare but legal).
    if (line == "do" || starts_with_keyword("do"))
        return true;

    // Trailing-do form: any line ending in ` do`, `do`, ` do |args|`
    // opens a block. We look for the last ` do` occurrence and check
    // whether what follows is either empty, just a `|...|` block-args
    // list, or a comment.
    auto find_trailing_do = [&]() -> bool {
        // Case A: line literally ends in ` do`.
        if (line.size() >= 3 && line.ends_with(" do"))
            return true;
        // Case B: line ends in ` do |...|`.
        auto bar = line.rfind('|');
        if (bar == std::string::npos || bar == 0)
            return false;
        // Walk back from `bar` to find the matching opening `|`.
        auto open_bar = line.rfind('|', bar - 1);
        if (open_bar == std::string::npos)
            return false;
        // What's before the opening `|` must end in ` do`.
        // Allow optional whitespace.
        auto before = open_bar;
        while (before > 0 && (line[before - 1] == ' ' || line[before - 1] == '\t'))
            --before;
        return before >= 3 && line.compare(before - 3, 3, " do") == 0;
    };
    if (find_trailing_do())
        return true;

    return false;
}

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

        // A line that opens a block increments the depth, unless it
        // also closes the same block on the same line (`if x then y
        // end`, `foo do bar end`).
        if (opens_block(trimmed)) {
            bool self_closes =
                trimmed.contains(" end") || trimmed.ends_with(" end") || trimmed.ends_with(";end");
            if (!self_closes)
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
            auto replace_all = [](std::string& s, const std::string& from, const std::string& to) {
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

/// Strip Ruby interpolation so a leading-keyword test (e.g. "if") doesn't
/// misfire on a quoted string that happens to contain those characters.
bool line_starts_with_keyword(const std::string& line, const std::string& keyword) {
    if (!line.starts_with(keyword))
        return false;
    if (line.size() == keyword.size())
        return true;
    char next = line[keyword.size()];
    // The keyword must be a whole word, not a prefix of something else
    // (e.g. "iffy", "doctor", "case_insensitive"). Allow space, tab,
    // EOL, or an opening paren / brace.
    return next == ' ' || next == '\t' || next == '(' || next == '{' || next == ':';
}

/// Does the logical line end in a Ruby trailing conditional modifier
/// (`if`, `unless`, `while`, `until`)? These attach a predicate to an
/// arbitrary statement (`system "cmd" if build.head?`) and are a
/// silent-drop hazard: without this check the `system` branch would
/// happily parse the left-hand side as a plain invocation and fold the
/// `if build.head?` tail into the command's argument list.
///
/// We approximate by finding the rightmost match for ` if `, ` unless `,
/// ` while `, ` until ` that sits outside a double-quoted string, then
/// checking that what follows is a non-empty expression. String
/// boundaries are tracked by counting unescaped quotes; this is good
/// enough for Homebrew install bodies, which don't nest heredocs or
/// single-quoted strings containing double quotes.
bool has_trailing_conditional(const std::string& line) {
    static constexpr std::string_view kws[] = {" if ", " unless ", " while ", " until "};
    for (auto kw : kws) {
        auto pos = line.rfind(kw);
        if (pos == std::string::npos)
            continue;
        // Count unescaped double-quotes before `pos`. If odd, `pos` is
        // inside a string literal and doesn't open a modifier.
        int quotes = 0;
        for (size_t i = 0; i < pos; ++i) {
            if (line[i] == '"' && (i == 0 || line[i - 1] != '\\'))
                ++quotes;
        }
        if (quotes % 2 != 0)
            continue;
        // The condition expression must be non-empty.
        auto cond_start = pos + kw.size();
        while (cond_start < line.size() && (line[cond_start] == ' ' || line[cond_start] == '\t'))
            ++cond_start;
        if (cond_start < line.size())
            return true;
    }
    return false;
}

/// Does a logical line contain an unhandled DSL construct? Returns the
/// construct name to record, or empty if the line is plain. The returned
/// name identifies *why* the formula is complex, for explainability.
std::string detect_complexity_construct(const std::string& line) {
    // Trailing conditional modifiers come first: they can attach to any
    // statement (including `system`), and every subsequent branch needs
    // to treat them as "unhandled" rather than silently absorbing them.
    if (has_trailing_conditional(line))
        return "trailing-conditional";
    // Conditional / control flow.
    if (line_starts_with_keyword(line, "if"))
        return "if";
    if (line_starts_with_keyword(line, "unless"))
        return "unless";
    if (line_starts_with_keyword(line, "case"))
        return "case";
    if (line_starts_with_keyword(line, "begin"))
        return "begin";
    // Platform / arch gates — these are part of the install body in modern
    // formulae and must never be silently skipped.
    if (line_starts_with_keyword(line, "on_macos"))
        return "on_macos";
    if (line_starts_with_keyword(line, "on_linux"))
        return "on_linux";
    if (line_starts_with_keyword(line, "on_arm"))
        return "on_arm";
    if (line_starts_with_keyword(line, "on_intel"))
        return "on_intel";
    if (line_starts_with_keyword(line, "on_big_sur") ||
        line_starts_with_keyword(line, "on_monterey") ||
        line_starts_with_keyword(line, "on_ventura") ||
        line_starts_with_keyword(line, "on_sonoma") ||
        line_starts_with_keyword(line, "on_sequoia") || line_starts_with_keyword(line, "on_tahoe"))
        return "on_macos_version";
    // Top-level DSL blocks that can appear in install.
    if (line_starts_with_keyword(line, "resource"))
        return "resource";
    if (line_starts_with_keyword(line, "patch"))
        return "patch";
    if (line_starts_with_keyword(line, "inreplace"))
        return "inreplace";
    if (line_starts_with_keyword(line, "bottle"))
        return "bottle";
    // Generic do-block (e.g. `Dir.chdir("foo") do`). We cannot reason
    // about what the block does without a real evaluator.
    if (line.ends_with(" do") || line.ends_with("|") || line.starts_with("do "))
        return "do";
    return "";
}

} // namespace

const char* to_string(FormulaComplexity c) {
    switch (c) {
    case FormulaComplexity::Simple:
        return "simple";
    case FormulaComplexity::Complex:
        return "complex";
    case FormulaComplexity::Unsupported:
        return "unsupported";
    }
    return "?";
}

ParsedFormula parse_formula(const std::string& brew_cat_output, const std::string& prefix,
                            const std::string& name) {
    ParsedFormula result;
    // Optimistic default: downgraded to Complex on any unhandled construct,
    // or left as Unsupported if the formula has no install body at all.
    result.complexity = FormulaComplexity::Simple;
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
        result.complexity = FormulaComplexity::Unsupported;
        result.complexity_markers.push_back({"missing-install", 0, "no `def install` block found"});
        return result;
    }

    // Join continuation lines (lines ending with comma). Each entry is
    // paired with the 1-based line number it started on, so complexity
    // markers can point at the offending construct.
    struct LogicalLine {
        std::string text;
        int start_line;
    };
    std::vector<LogicalLine> lines;
    {
        std::istringstream raw(body);
        std::string raw_line;
        std::string accum;
        int accum_start = 0;
        int current_line = 0;
        while (std::getline(raw, raw_line)) {
            ++current_line;
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
                accum_start = current_line;
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

            lines.push_back({accum_trimmed, accum_start});
            accum.clear();
            accum_start = 0;
        }
        if (!accum.empty())
            lines.push_back({accum, accum_start});
    }

    // Process joined lines.
    for (const auto& ll : lines) {
        const auto& trimmed = ll.text;

        // Comments are not load-bearing.
        if (trimmed.starts_with("#"))
            continue;

        // Scope/structure tokens from previously-opened blocks. `else`/`end`/
        // closing braces on their own are not unhandled constructs — they're
        // the tail of a construct the parser already flagged when it saw the
        // opening keyword.
        if (trimmed == "else" || trimmed.starts_with("else ") || trimmed.starts_with("elsif ") ||
            trimmed == "end" || trimmed.starts_with("end ") || trimmed.starts_with("end#") ||
            trimmed == "}" || trimmed == ")" || trimmed.starts_with("| ") || trimmed == "|")
            continue;

        // Known DSL constructs the parser does not handle: emit a marker
        // and downgrade the verdict. Keep reading so every offender is
        // collected — callers get a full picture of *why* a formula is
        // complex, not just the first trigger.
        if (auto construct = detect_complexity_construct(trimmed); !construct.empty()) {
            result.complexity_markers.push_back({construct, ll.start_line, trimmed});
            result.complexity = FormulaComplexity::Complex;
            continue;
        }

        // ENV modifications.
        if (trimmed.starts_with("ENV.append") || trimmed.starts_with("ENV.prepend") ||
            trimmed.starts_with("ENV[")) {
            // Extract key=value from ENV.append "KEY", "value"
            std::regex env_re(R"RE(ENV\.\w+\s+"(\w+)",\s+"([^"]+)")RE");
            if (std::regex_search(trimmed, match, env_re)) {
                result.env_settings.push_back(match[1].str() + "+=" + match[2].str());
                continue;
            }
            // An ENV mutation we can't decode is an unhandled construct.
            result.complexity_markers.push_back({"ENV", ll.start_line, trimmed});
            result.complexity = FormulaComplexity::Complex;
            continue;
        }

        // system calls — the main build commands.
        if (trimmed.starts_with("system ")) {
            auto cmd = parse_system_call(trimmed, prefix, lib, name);
            if (!cmd.empty()) {
                result.build_commands.push_back(cmd);
                continue;
            }
            // A `system` line the parser couldn't decode is still an
            // unhandled construct — don't drop it on the floor.
            result.complexity_markers.push_back({"system", ll.start_line, trimmed});
            result.complexity = FormulaComplexity::Complex;
            continue;
        }

        // mkdir_p — directory manipulation.
        if (trimmed.starts_with("mkdir_p ")) {
            std::regex mkdir_re(R"RE(mkdir_p\s+"([^"]+)")RE");
            if (std::regex_search(trimmed, match, mkdir_re)) {
                auto dir = match[1].str();
                auto replace_prefix = [&](std::string& s) {
                    size_t p = 0;
                    while ((p = s.find("#{prefix}", p)) != std::string::npos)
                        s.replace(p, 9, prefix);
                };
                replace_prefix(dir);
                result.build_commands.push_back("mkdir -p " + dir);
                continue;
            }
            result.complexity_markers.push_back({"mkdir_p", ll.start_line, trimmed});
            result.complexity = FormulaComplexity::Complex;
            continue;
        }

        // Anything else is an unhandled statement. Record it so we never
        // silently drop install-method logic the parser does not understand.
        result.complexity_markers.push_back({"unknown-statement", ll.start_line, trimmed});
        result.complexity = FormulaComplexity::Complex;
    }

    return result;
}

} // namespace den

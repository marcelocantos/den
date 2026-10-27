// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "url_formula.h"

#include "../core/error.h"
#include "../download/archive.h"
#include "../download/fetch.h"
#include "../store/link.h"
#include "../store/store.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fnmatch.h>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace den {

TapPlatform host_tap_platform(const Config& config) {
    TapPlatform p;
#ifdef __APPLE__
    p.mac = true;
#else
    p.mac = false;
#endif
    p.arm = config.arch == Arch::Arm64;
    return p;
}

namespace {

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return std::string(s.substr(b, e - b));
}

bool starts_with_word(const std::string& s, std::string_view w) {
    if (!s.starts_with(w))
        return false;
    if (s.size() == w.size())
        return true;
    char c = s[w.size()];
    return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '?' || c == '!');
}

// Strip a trailing `# comment` that lies outside string literals.
std::string strip_comment(const std::string& line) {
    char quote = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (quote) {
            if (c == '\\') {
                ++i;
            } else if (c == quote) {
                quote = 0;
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '#') {
            return line.substr(0, i);
        }
    }
    return line;
}

// The heredoc terminator a line opens (`<<~SH`, `<<-EOS`, `<<EOS`), if any.
std::optional<std::pair<std::string, bool>> heredoc_tag(const std::string& line) {
    auto pos = line.find("<<");
    while (pos != std::string::npos) {
        size_t i = pos + 2;
        bool squiggly = false;
        if (i < line.size() && (line[i] == '~' || line[i] == '-')) {
            squiggly = line[i] == '~';
            ++i;
        }
        size_t start = i;
        while (i < line.size() &&
               (std::isupper(static_cast<unsigned char>(line[i])) || line[i] == '_' ||
                std::isdigit(static_cast<unsigned char>(line[i]))))
            ++i;
        if (i > start && std::isupper(static_cast<unsigned char>(line[start])))
            return std::make_pair(line.substr(start, i - start), squiggly);
        pos = line.find("<<", pos + 2);
    }
    return std::nullopt;
}

// Ruby's <<~ removes the smallest common leading indentation.
std::string dedent(const std::vector<std::string>& body) {
    size_t common = std::string::npos;
    for (const auto& l : body) {
        if (trim(l).empty())
            continue;
        size_t n = 0;
        while (n < l.size() && (l[n] == ' ' || l[n] == '\t'))
            ++n;
        common = std::min(common, n);
    }
    if (common == std::string::npos)
        common = 0;
    std::string out;
    for (const auto& l : body) {
        out += l.size() >= common ? l.substr(common) : trim(l);
        out += '\n';
    }
    return out;
}

// A logical statement: continuation lines joined with a space, plus the body
// of any heredoc it opens.
struct Statement {
    std::string text;
    std::optional<std::string> heredoc;
};

bool continues(const std::string& t) {
    if (t.empty())
        return false;
    static const char* tails[] = {",", "\\", "+", "||", "&&", "(", "[", "=>", " if", " unless"};
    for (const char* tail : tails) {
        if (t.ends_with(tail))
            return true;
    }
    return false;
}

std::vector<Statement> statements(const std::string& src) {
    std::vector<std::string> lines;
    {
        std::istringstream in(src);
        std::string l;
        while (std::getline(in, l))
            lines.push_back(l);
    }
    std::vector<Statement> out;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string t = trim(strip_comment(lines[i]));
        if (t.empty())
            continue;
        while (continues(t) && i + 1 < lines.size()) {
            if (t.ends_with("\\"))
                t.pop_back();
            t = trim(t) + " " + trim(strip_comment(lines[++i]));
            t = trim(t);
        }
        Statement st;
        if (auto tag = heredoc_tag(t)) {
            std::vector<std::string> body;
            while (i + 1 < lines.size() && trim(lines[i + 1]) != tag->first)
                body.push_back(lines[++i]);
            ++i; // terminator
            st.heredoc = tag->second ? dedent(body) : [&] {
                std::string s;
                for (auto& b : body)
                    s += b + "\n";
                return s;
            }();
        }
        st.text = std::move(t);
        out.push_back(std::move(st));
    }
    return out;
}

// First double-quoted literal in s, unescaped.
std::optional<std::string> first_string(const std::string& s, size_t from = 0) {
    auto q = s.find('"', from);
    if (q == std::string::npos)
        return std::nullopt;
    std::string v;
    for (size_t i = q + 1; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            v += s[++i];
        } else if (s[i] == '"') {
            return v;
        } else {
            v += s[i];
        }
    }
    return std::nullopt;
}

enum class FrameKind { Class, Platform, Install, Other };

struct Frame {
    FrameKind kind;
    bool active = true;       // Platform: the condition holds here
    std::optional<bool> cond; // Platform `if`: the condition, for `else`
};

// Value of a platform condition, or nullopt when it is not one den knows.
std::optional<bool> platform_cond(std::string c, const TapPlatform& p) {
    c = trim(c);
    if (c.ends_with(" then"))
        c = trim(c.substr(0, c.size() - 5));
    bool neg = false;
    if (c.starts_with("!")) {
        neg = true;
        c = trim(c.substr(1));
    }
    std::optional<bool> v;
    if (c == "OS.mac?")
        v = p.mac;
    else if (c == "OS.linux?")
        v = !p.mac;
    else if (c == "Hardware::CPU.arm?")
        v = p.arm;
    else if (c == "Hardware::CPU.intel?")
        v = !p.arm;
    else if (c == "Hardware::CPU.is_64_bit?")
        v = true;
    else if (c == "Hardware::CPU.is_32_bit?")
        v = false;
    if (v && neg)
        *v = !*v;
    return v;
}

bool opens_block(const std::string& t) {
    if (t.ends_with(" do") || t == "do")
        return true;
    // `foo do |x|`
    auto d = t.rfind(" do |");
    if (d != std::string::npos && t.ends_with("|"))
        return true;
    for (auto w : {"if", "unless", "case", "while", "until", "begin", "def", "class", "module"}) {
        if (starts_with_word(t, w)) {
            // `def x = y` (endless) and one-line `... end` do not open.
            if (std::string_view(w) == "def" && t.find(" = ") != std::string::npos)
                return false;
            if (t.ends_with(" end"))
                return false;
            return true;
        }
    }
    return false;
}

} // namespace

ParsedTapFormula parse_tap_formula(const std::string& src, const TapPlatform& platform) {
    ParsedTapFormula out;
    std::vector<Frame> stack;
    std::string url, sha;
    std::vector<std::string> install;
    std::vector<std::string> current_install;
    bool have_install = false;
    bool capturing = false;
    size_t install_depth = 0;

    auto eligible = [&] {
        if (stack.empty() || stack.front().kind != FrameKind::Class)
            return false;
        for (size_t i = 1; i < stack.size(); ++i) {
            if (stack[i].kind != FrameKind::Platform || !stack[i].active)
                return false;
        }
        return true;
    };

    for (const auto& st : statements(src)) {
        const std::string& t = st.text;

        if (capturing) {
            if (t == "end" && stack.size() == install_depth) {
                stack.pop_back();
                capturing = false;
                install = std::move(current_install);
                current_install.clear();
                have_install = true;
                continue;
            }
            if (t == "end") {
                stack.pop_back();
            } else if (opens_block(t)) {
                stack.push_back({FrameKind::Other});
            }
            current_install.push_back(st.heredoc ? t + "\n" + *st.heredoc : t);
            continue;
        }

        if (t == "end") {
            if (!stack.empty())
                stack.pop_back();
            continue;
        }
        if ((t == "else" || starts_with_word(t, "elsif")) && !stack.empty()) {
            auto& f = stack.back();
            if (f.kind == FrameKind::Platform && f.cond) {
                if (t == "else") {
                    f.active = !*f.cond;
                } else if (auto c = platform_cond(t.substr(6), platform); c && !*f.cond) {
                    f.cond = *c;
                    f.active = *c;
                } else {
                    f.active = false;
                }
            }
            continue;
        }

        if (starts_with_word(t, "class")) {
            stack.push_back({stack.empty() ? FrameKind::Class : FrameKind::Other});
            continue;
        }
        if (t == "def install") {
            if (eligible()) {
                stack.push_back({FrameKind::Install});
                capturing = true;
                install_depth = stack.size();
            } else {
                stack.push_back({FrameKind::Other});
            }
            continue;
        }
        static const std::map<std::string, int> on_blocks = {
            {"on_macos do", 0}, {"on_linux do", 1}, {"on_arm do", 2}, {"on_intel do", 3}};
        if (auto it = on_blocks.find(t); it != on_blocks.end()) {
            bool v = it->second == 0   ? platform.mac
                     : it->second == 1 ? !platform.mac
                     : it->second == 2 ? platform.arm
                                       : !platform.arm;
            stack.push_back({FrameKind::Platform, v});
            continue;
        }
        if (starts_with_word(t, "if") || starts_with_word(t, "unless")) {
            bool is_unless = starts_with_word(t, "unless");
            if (t.ends_with(" end")) {
                continue; // one-liner
            }
            auto c = platform_cond(t.substr(is_unless ? 7 : 3), platform);
            if (c) {
                bool v = is_unless ? !*c : *c;
                stack.push_back({FrameKind::Platform, v, v});
            } else {
                stack.push_back({FrameKind::Other});
            }
            continue;
        }
        if (opens_block(t)) {
            stack.push_back({FrameKind::Other});
            continue;
        }

        if (!eligible())
            continue;
        if (starts_with_word(t, "url")) {
            if (auto v = first_string(t)) {
                url = *v;
                sha.clear();
            }
        } else if (starts_with_word(t, "sha256")) {
            if (auto v = first_string(t))
                sha = *v;
        } else if (starts_with_word(t, "version")) {
            if (auto v = first_string(t))
                out.version = *v;
        } else if (starts_with_word(t, "depends_on")) {
            auto rest = t.substr(10);
            if (rest.find(":build") != std::string::npos || rest.find(":test") != std::string::npos)
                continue;
            if (auto v = first_string(rest); v && trim(rest).starts_with("\""))
                out.dependencies.push_back(*v);
        }
    }

    if (!url.empty()) {
        Package::UrlInstall ui;
        ui.url = url;
        ui.sha256 = sha;
        ui.install_lines = install;
        if (sha.empty()) {
            ui.refusal = "no sha256 for " + url + " on this platform";
        } else if (!have_install) {
            ui.refusal = "no `def install` block for this platform";
        }
        out.url_install = std::move(ui);
    }
    return out;
}

// ─── install steps ────────────────────────────────────────────────────────

namespace {

// Keg-relative directory for a Formula path helper, or nullopt.
std::optional<std::string> base_dir(const std::string& b, const std::string& name) {
    static const std::map<std::string, std::string> dirs = {
        {"bin", "bin"},
        {"sbin", "sbin"},
        {"lib", "lib"},
        {"libexec", "libexec"},
        {"include", "include"},
        {"share", "share"},
        {"etc", "etc"},
        {"man", "share/man"},
        {"doc", "share/doc"},
        {"info", "share/info"},
        {"bash_completion", "etc/bash_completion.d"},
        {"zsh_completion", "share/zsh/site-functions"},
        {"fish_completion", "share/fish/vendor_completions.d"},
        {"prefix", ""},
    };
    if (auto it = dirs.find(b); it != dirs.end())
        return it->second;
    if (b == "pkgshare")
        return "share/" + name;
    if (b.size() == 4 && b.starts_with("man") && b[3] >= '1' && b[3] <= '8')
        return "share/man/" + b;
    return std::nullopt;
}

bool safe_relative(const std::string& p) {
    if (p.empty() || p.front() == '/')
        return false;
    for (const auto& part : fs::path(p)) {
        if (part == "..")
            return false;
    }
    return true;
}

std::string join_rel(const std::string& a, const std::string& b) {
    if (a.empty())
        return b;
    if (b.empty())
        return a;
    return a + "/" + b;
}

// Cursor-based parser over one statement.
struct Cursor {
    const std::string& s;
    size_t i = 0;

    void ws() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
    }
    bool eat(std::string_view tok) {
        ws();
        if (s.compare(i, tok.size(), tok) == 0) {
            i += tok.size();
            return true;
        }
        return false;
    }
    bool done() {
        ws();
        return i >= s.size();
    }
    std::optional<std::string> ident() {
        ws();
        size_t b = i;
        while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_'))
            ++i;
        if (i == b)
            return std::nullopt;
        return s.substr(b, i - b);
    }
    std::optional<std::string> str() {
        ws();
        if (i >= s.size() || s[i] != '"')
            return std::nullopt;
        std::string v;
        for (++i; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                v += s[++i];
            } else if (s[i] == '"') {
                ++i;
                if (v.find("#{") != std::string::npos)
                    return std::nullopt; // no interpolation in paths
                return v;
            } else {
                v += s[i];
            }
        }
        return std::nullopt;
    }
    // `base` or `base/"sub"` (optionally parenthesised) -> keg-relative dir.
    std::optional<std::string> dir_expr(const std::string& name) {
        bool paren = eat("(");
        auto b = ident();
        if (!b)
            return std::nullopt;
        auto d = base_dir(*b, name);
        if (!d)
            return std::nullopt;
        std::string dir = *d;
        while (eat("/")) {
            auto sub = str();
            if (!sub || !safe_relative(*sub))
                return std::nullopt;
            dir = join_rel(dir, *sub);
        }
        if (paren && !eat(")"))
            return std::nullopt;
        return dir;
    }
};

// Interpolations allowed in `system` arguments, expanded when the step runs.
bool system_var(std::string_view v) {
    return v == "version" || v == "name";
}

// One `system` argument: "…" (with #{version}/#{name}), '…', or a %w[…]
// word list (optionally splatted). Appends to `out`; false on anything else.
bool system_arg(Cursor& c, std::vector<std::string>& out) {
    c.ws();
    const auto& s = c.s;
    if (c.i >= s.size())
        return false;
    if (s[c.i] == '"') {
        std::string v;
        for (++c.i; c.i < s.size(); ++c.i) {
            char ch = s[c.i];
            if (ch == '\\' && c.i + 1 < s.size()) {
                v += s[++c.i];
            } else if (ch == '"') {
                ++c.i;
                for (size_t b = v.find("#{"); b != std::string::npos; b = v.find("#{", b + 2)) {
                    auto e = v.find('}', b);
                    if (e == std::string::npos ||
                        !system_var(std::string_view(v).substr(b + 2, e - b - 2)))
                        return false;
                }
                out.push_back(std::move(v));
                return true;
            } else {
                v += ch;
            }
        }
        return false;
    }
    if (s[c.i] == '\'') {
        std::string v;
        for (++c.i; c.i < s.size(); ++c.i) {
            char ch = s[c.i];
            if (ch == '\\' && c.i + 1 < s.size() && (s[c.i + 1] == '\'' || s[c.i + 1] == '\\')) {
                v += s[++c.i];
            } else if (ch == '\'') {
                ++c.i;
                out.push_back(std::move(v));
                return true;
            } else {
                v += ch;
            }
        }
        return false;
    }
    c.eat("*");
    if (c.eat("%w[") || c.eat("%w(")) {
        const char close = s[c.i - 1] == '[' ? ']' : ')';
        auto e = s.find(close, c.i);
        if (e == std::string::npos)
            return false;
        std::istringstream words(s.substr(c.i, e - c.i));
        for (std::string w; words >> w;)
            out.push_back(w);
        c.i = e + 1;
        return true;
    }
    return false;
}

// make's -C/--directory must name a directory inside the stage.
bool make_dirs_safe(const std::vector<std::string>& argv) {
    auto base = fs::path(argv.front()).filename().string();
    if (base != "make" && base != "gmake")
        return true;
    for (size_t i = 1; i < argv.size(); ++i) {
        const auto& a = argv[i];
        std::string dir;
        if (a == "-C" || a == "--directory") {
            if (i + 1 >= argv.size())
                return false;
            dir = argv[++i];
        } else if (a.starts_with("--directory=")) {
            dir = a.substr(12);
        } else if (a.starts_with("-C")) {
            dir = a.substr(2);
        } else {
            continue;
        }
        if (!safe_relative(dir))
            return false;
    }
    return true;
}

// `system "cmd", "arg", …` or `system("cmd", …)`.
std::optional<InstallStep> parse_system(const std::string& text) {
    Cursor c{text};
    if (!c.eat("system"))
        return std::nullopt;
    if (c.i < text.size() && text[c.i] != '(' &&
        !std::isspace(static_cast<unsigned char>(text[c.i])))
        return std::nullopt;
    bool paren = c.eat("(");
    InstallStep step;
    step.kind = InstallStep::Kind::System;
    do {
        if (!system_arg(c, step.argv))
            return std::nullopt;
    } while (c.eat(","));
    if (paren && !c.eat(")"))
        return std::nullopt;
    if (!c.done() || step.argv.empty())
        return std::nullopt;
    // A lone command string goes through /bin/sh in Ruby only when it holds
    // shell metacharacters; den runs no shell, so it accepts the plain form
    // and splits it on whitespace the way Ruby does.
    if (step.argv.size() == 1 && step.argv[0].find_first_of(" \t") != std::string::npos) {
        if (step.argv[0].find_first_of("*?{}[]<>()~&|\\$;'`\"\n#=%") != std::string::npos)
            return std::nullopt;
        std::istringstream words(step.argv[0]);
        step.argv.clear();
        for (std::string w; words >> w;)
            step.argv.push_back(w);
    }
    if (step.argv.empty() || step.argv.front().empty() || !make_dirs_safe(step.argv))
        return std::nullopt;
    return step;
}

std::optional<InstallStep> parse_step(const std::string& stmt, const std::string& name) {
    std::string text = stmt;
    std::string heredoc;
    if (auto nl = stmt.find('\n'); nl != std::string::npos) {
        text = stmt.substr(0, nl);
        heredoc = stmt.substr(nl + 1);
    }
    if (starts_with_word(text, "system") || text.starts_with("system("))
        return heredoc.empty() ? parse_system(text) : std::nullopt;
    Cursor c{text};
    auto dir = c.dir_expr(name);
    if (!dir || !c.eat("."))
        return std::nullopt;
    auto method = c.ident();
    if (!method)
        return std::nullopt;

    InstallStep step;
    if (*method == "install") {
        step.kind = InstallStep::Kind::Install;
        step.dir = *dir;
        bool paren = c.eat("(");
        do {
            if (c.eat("Dir[")) {
                auto g = c.str();
                if (!g || !c.eat("]") || !step.items.empty())
                    return std::nullopt;
                step.glob = true;
                step.items.emplace_back(*g, "");
                continue;
            }
            auto src = c.str();
            if (!src || !safe_relative(*src) || step.glob)
                return std::nullopt;
            std::string dst;
            if (c.eat("=>")) {
                auto d = c.str();
                if (!d || !safe_relative(*d))
                    return std::nullopt;
                dst = *d;
            }
            step.items.emplace_back(*src, dst);
        } while (c.eat(","));
        if (paren && !c.eat(")"))
            return std::nullopt;
        if (c.eat("if")) {
            if (!c.eat("File.exist?(") && !c.eat("File.exist? ("))
                return std::nullopt;
            auto p = c.str();
            if (!p || !safe_relative(*p) || !c.eat(")"))
                return std::nullopt;
            step.if_exists = *p;
        }
        return c.done() && !step.items.empty() ? std::optional(step) : std::nullopt;
    }
    if (*method == "install_symlink") {
        step.kind = InstallStep::Kind::Symlink;
        step.dir = *dir;
        bool paren = c.eat("(");
        auto target = c.dir_expr(name);
        if (!target || *target == step.dir)
            return std::nullopt;
        step.target = *target;
        step.name = fs::path(*target).filename().string();
        if (c.eat("=>")) {
            auto n = c.str();
            if (!n || !safe_relative(*n))
                return std::nullopt;
            step.name = *n;
        }
        if (paren && !c.eat(")"))
            return std::nullopt;
        return c.done() ? std::optional(step) : std::nullopt;
    }
    // (dir/"file").write <<~TAG / .chmod 0755: the file is the last segment.
    if (*method == "write" || *method == "chmod") {
        auto slash = dir->rfind('/');
        auto file = slash == std::string::npos ? *dir : dir->substr(slash + 1);
        auto parent = slash == std::string::npos ? std::string{} : dir->substr(0, slash);
        // A bare base helper ("bin".write) is a directory, not a file.
        if (!base_dir(*dir, name) || slash != std::string::npos) {
            step.dir = parent;
            step.name = file;
        } else {
            return std::nullopt;
        }
        if (*method == "write") {
            step.kind = InstallStep::Kind::Write;
            if (!c.eat("<<~") && !c.eat("<<-") && !c.eat("<<"))
                return std::nullopt;
            c.ident();
            if (!c.done())
                return std::nullopt;
            step.content = heredoc;
            return step;
        }
        step.kind = InstallStep::Kind::Chmod;
        c.ws();
        size_t b = c.i;
        while (c.i < text.size() && std::isdigit(static_cast<unsigned char>(text[c.i])))
            ++c.i;
        auto digits = text.substr(b, c.i - b);
        if (digits.size() < 3 || digits.front() != '0' || !c.done())
            return std::nullopt;
        step.mode = static_cast<unsigned>(std::stoul(digits, nullptr, 8));
        return step;
    }
    return std::nullopt;
}

// `mkdir "d" do` / `cd "d" do` (optionally `mkdir("d") do`): the block's
// directory, and whether it is created (mkdir) or must already exist (cd).
std::optional<std::pair<std::string, bool>> parse_dir_block(const std::string& text) {
    if (text.find('\n') != std::string::npos)
        return std::nullopt;
    bool create;
    if (starts_with_word(text, "mkdir") || text.starts_with("mkdir("))
        create = true;
    else if (starts_with_word(text, "cd") || text.starts_with("cd("))
        create = false;
    else
        return std::nullopt;
    Cursor c{text};
    c.eat(create ? "mkdir" : "cd");
    bool paren = c.eat("(");
    auto d = c.str();
    if (!d || !safe_relative(*d))
        return std::nullopt;
    if (paren && !c.eat(")"))
        return std::nullopt;
    if (!c.eat("do") || !c.done())
        return std::nullopt;
    return std::pair{*d, create};
}

} // namespace

std::optional<std::vector<InstallStep>> parse_install_steps(const std::vector<std::string>& lines,
                                                            const std::string& formula_name,
                                                            std::string& refusal) {
    std::vector<InstallStep> steps;
    std::vector<std::string> cwds; // open mkdir/cd blocks, innermost last
    auto unsupported = [&](const std::string& l) {
        refusal = "unsupported install step: `" + l.substr(0, l.find('\n')) + "`";
        return std::nullopt;
    };
    for (const auto& l : lines) {
        const std::string cwd = cwds.empty() ? std::string{} : cwds.back();
        if (l == "end") {
            if (cwds.empty())
                return unsupported(l);
            cwds.pop_back();
            continue;
        }
        if (auto blk = parse_dir_block(l)) {
            InstallStep step;
            step.kind = InstallStep::Kind::Mkdir;
            step.cwd = cwd;
            step.name = blk->first;
            step.create = blk->second;
            steps.push_back(std::move(step));
            cwds.push_back(join_rel(cwd, blk->first));
            continue;
        }
        auto step = parse_step(l, formula_name);
        if (!step)
            return unsupported(l);
        step->cwd = cwd;
        steps.push_back(std::move(*step));
    }
    if (!cwds.empty()) {
        refusal = "unterminated `mkdir`/`cd` block in `def install`";
        return std::nullopt;
    }
    if (steps.empty()) {
        refusal = "empty `def install` block";
        return std::nullopt;
    }
    return steps;
}

namespace {

std::string expand(const std::string& text, const InstallPaths& p) {
    const std::string pre = p.homebrew_prefix.string();
    const std::string opt = pre + "/opt/" + p.name;
    const std::string keg = p.keg.string();
    const std::map<std::string, std::string> vars = {
        {"HOMEBREW_PREFIX", pre},
        {"prefix", keg},
        {"bin", keg + "/bin"},
        {"libexec", keg + "/libexec"},
        {"share", keg + "/share"},
        {"pkgshare", keg + "/share/" + p.name},
        {"opt_prefix", opt},
        {"opt_bin", opt + "/bin"},
        {"opt_libexec", opt + "/libexec"},
        {"opt_share", opt + "/share"},
        {"opt_pkgshare", opt + "/share/" + p.name},
        {"version", p.version},
        {"name", p.name},
    };
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        auto b = text.find("#{", i);
        if (b == std::string::npos) {
            out += text.substr(i);
            break;
        }
        out += text.substr(i, b - i);
        auto e = text.find('}', b);
        if (e == std::string::npos)
            throw UserError("cannot install " + p.name + ": unterminated #{ in write heredoc");
        auto var = text.substr(b + 2, e - b - 2);
        auto it = vars.find(var);
        if (it == vars.end())
            throw UserError("cannot install " + p.name +
                            ": unsupported interpolation in write heredoc: #{" + var + "}");
        out += it->second;
        i = e + 1;
    }
    return out;
}

// True when `p` (resolved through symlinks) lies inside `root`.
bool inside(const fs::path& p, const fs::path& root) {
    std::error_code ec;
    auto rp = fs::weakly_canonical(p, ec);
    if (ec)
        return false;
    auto rr = fs::weakly_canonical(root, ec);
    if (ec)
        return false;
    auto [ri, pi] = std::mismatch(rr.begin(), rr.end(), rp.begin(), rp.end());
    return ri == rr.end();
}

void move_into(const fs::path& src, const fs::path& dst) {
    fs::create_directories(dst.parent_path());
    if (fs::exists(fs::symlink_status(dst)))
        fs::remove_all(dst);
    std::error_code ec;
    fs::rename(src, dst, ec);
    if (ec) {
        fs::copy(src, dst,
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks |
                     fs::copy_options::overwrite_existing);
        fs::remove_all(src);
    }
}

// Expand #{version}/#{name} in a `system` argument.
std::string expand_system_arg(std::string a, const InstallPaths& p) {
    for (auto [var, val] : {std::pair<std::string, std::string>{"#{version}", p.version},
                            std::pair<std::string, std::string>{"#{name}", p.name}}) {
        for (size_t at = a.find(var); at != std::string::npos; at = a.find(var, at + val.size()))
            a.replace(at, var.size(), val);
    }
    return a;
}

bool within(const fs::path& p, const fs::path& root) {
    auto rel = p.lexically_normal().lexically_relative(root.lexically_normal());
    return !rel.empty() && *rel.begin() != "..";
}

// Run argv in `cwd` with inherited stdout/stderr and stdin from /dev/null.
// Returns the exit status, 128+signal, or -1 when the command did not start.
int run_step_command(const std::vector<std::string>& argv, const fs::path& cwd, int& spawn_errno) {
    std::vector<char*> ptrs;
    for (const auto& a : argv)
        ptrs.push_back(const_cast<char*>(a.c_str()));
    ptrs.push_back(nullptr);
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        spawn_errno = errno;
        return -1;
    }
    ::fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    std::cout.flush();
    std::cerr.flush();
    pid_t pid = ::fork();
    if (pid < 0) {
        spawn_errno = errno;
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        ::close(pipefd[0]);
        int err = 0;
        if (::chdir(cwd.c_str()) != 0) {
            err = errno;
        } else {
            int null = ::open("/dev/null", O_RDONLY);
            if (null >= 0) {
                ::dup2(null, STDIN_FILENO);
                ::close(null);
            }
            ::execvp(ptrs[0], ptrs.data());
            err = errno;
        }
        (void)!::write(pipefd[1], &err, sizeof err);
        ::_exit(127);
    }
    ::close(pipefd[1]);
    int err = 0;
    ssize_t n;
    while ((n = ::read(pipefd[0], &err, sizeof err)) < 0 && errno == EINTR) {
    }
    ::close(pipefd[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (n == static_cast<ssize_t>(sizeof err)) {
        spawn_errno = err;
        return -1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}

std::string quote_argv(const std::vector<std::string>& argv) {
    std::string out;
    for (const auto& a : argv) {
        if (!out.empty())
            out += ' ';
        bool plain = !a.empty() && a.find_first_of(" \t\"'\\$`") == std::string::npos;
        out += plain ? a : "\"" + a + "\"";
    }
    return out;
}

} // namespace

void apply_install_steps(const std::vector<InstallStep>& steps, const fs::path& stage,
                         const InstallPaths& paths) {
    const auto& keg = paths.staging_keg.empty() ? paths.keg : paths.staging_keg;
    auto fail = [&](const std::string& why) {
        throw UserError("cannot install " + paths.name + ": " + why);
    };
    for (const auto& s : steps) {
        const fs::path dir = s.dir.empty() ? keg : keg / s.dir;
        const fs::path cwd = s.cwd.empty() ? stage : stage / s.cwd;
        switch (s.kind) {
        case InstallStep::Kind::Mkdir: {
            auto d = cwd / s.name;
            const auto shown = join_rel(s.cwd, s.name);
            if (!inside(d, stage))
                fail("`" + std::string(s.create ? "mkdir" : "cd") + " \"" + s.name +
                     "\"` resolves outside the download");
            if (s.create) {
                std::error_code ec;
                fs::create_directories(d, ec);
                if (ec)
                    fail("cannot create directory '" + shown + "': " + ec.message());
            } else if (!fs::is_directory(d)) {
                fail("`cd \"" + s.name + "\"` names '" + shown +
                     "', which is not a directory in the download");
            }
            break;
        }
        case InstallStep::Kind::Install: {
            if (!s.if_exists.empty() && !fs::exists(fs::symlink_status(cwd / s.if_exists)))
                break;
            if (s.glob) {
                const auto& pat = s.items.front().first;
                std::vector<fs::path> matches;
                for (const auto& e : fs::directory_iterator(cwd)) {
                    auto fname = e.path().filename().string();
                    if (fname.starts_with(".") && !pat.starts_with("."))
                        continue;
                    if (fnmatch(pat.c_str(), fname.c_str(), 0) == 0)
                        matches.push_back(e.path());
                }
                std::sort(matches.begin(), matches.end());
                for (const auto& m : matches)
                    move_into(m, dir / m.filename());
                break;
            }
            for (const auto& [src, dst] : s.items) {
                auto from = cwd / src;
                if (!fs::exists(fs::symlink_status(from)))
                    fail("install step names '" + join_rel(s.cwd, src) +
                         "', which is not in the download");
                move_into(from, dir / (dst.empty() ? fs::path(src).filename() : fs::path(dst)));
            }
            break;
        }
        case InstallStep::Kind::Symlink: {
            auto target = keg / s.target;
            if (!fs::exists(fs::symlink_status(target)))
                fail("install_symlink target '" + s.target + "' was not installed");
            fs::create_directories(dir);
            auto link = dir / s.name;
            if (fs::exists(fs::symlink_status(link)))
                fs::remove(link);
            fs::create_symlink(fs::relative(target, dir), link);
            break;
        }
        case InstallStep::Kind::Write: {
            fs::create_directories(dir);
            std::ofstream out(dir / s.name, std::ios::binary | std::ios::trunc);
            out << expand(s.content, paths);
            if (!out)
                fail("cannot write " + (dir / s.name).string());
            break;
        }
        case InstallStep::Kind::Chmod: {
            auto f = dir / s.name;
            if (!fs::exists(f))
                fail("chmod target '" + join_rel(s.dir, s.name) + "' does not exist");
            fs::permissions(f, static_cast<fs::perms>(s.mode), fs::perm_options::replace);
            break;
        }
        case InstallStep::Kind::System: {
            std::vector<std::string> argv;
            for (const auto& a : s.argv)
                argv.push_back(expand_system_arg(a, paths));
            const auto shown = quote_argv(argv);
            if (argv.front().find('/') != std::string::npos) {
                auto cmd = fs::path(argv.front());
                auto resolved = cmd.is_absolute() ? cmd : cwd / cmd;
                if (!cmd.is_absolute() && !within(resolved, stage))
                    fail("`system " + shown + "` names a command outside the download");
                if (!fs::exists(resolved))
                    fail("`system " + shown + "`: '" + argv.front() + "' does not exist");
            }
            std::cout << "==> " << shown << "\n";
            int spawn_errno = 0;
            int rc = run_step_command(argv, cwd, spawn_errno);
            if (rc < 0)
                fail("`system " + shown + "` could not start: " + std::strerror(spawn_errno));
            if (rc != 0)
                fail("`system " + shown + "` failed (exit " + std::to_string(rc) + ")");
            break;
        }
        }
    }
}

fs::path install_url_formula(const Config& config, const Package& pkg) {
    const auto& ui = *pkg.url_install;
    auto refuse = [&](const std::string& why) {
        throw UserError("cannot install " + pkg.name + " " + pkg.version + ": " + why);
    };
    if (!ui.refusal.empty())
        refuse(ui.refusal);
    std::string why;
    auto steps = parse_install_steps(ui.install_lines, pkg.name, why);
    if (!steps)
        refuse(why);

    std::cout << "==> Downloading " << pkg.name << " " << pkg.version << "\n";
    auto cache_dir = config.cache / "archives";
    auto archive = fetch_archive(ui.url, ui.sha256, cache_dir);

    auto tmp = config.cache / ("tmp-url-" + pkg.name + "-" + pkg.version);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    auto stage = tmp / "stage";
    auto keg_tmp = tmp / "keg" / pkg.name / pkg.version;
    fs::create_directories(stage);
    fs::create_directories(keg_tmp);
    auto cleanup = [&] {
        std::error_code e;
        fs::remove_all(tmp, e);
    };

    const auto dest = package_path(config.store, pkg.name, pkg.version);
    try {
        auto fname = fs::path(ui.url).filename().string();
        if (auto q = fname.find('?'); q != std::string::npos)
            fname.resize(q);
        auto lower = fname;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        bool is_archive = lower.ends_with(".tar.gz") || lower.ends_with(".tgz") ||
                          lower.ends_with(".tar.xz") || lower.ends_with(".tar.bz2") ||
                          lower.ends_with(".tar") || lower.ends_with(".zip") ||
                          lower.ends_with(".txz") || lower.ends_with(".tbz");
        if (is_archive) {
            std::cout << "==> Extracting " << pkg.name << " " << pkg.version << "\n";
            extract_archive(archive, stage);
        } else {
            // A bare binary download is staged under its own file name.
            fs::copy_file(archive, stage / fname);
        }
        // Homebrew stages into the archive's single top-level directory.
        fs::path work = stage;
        std::vector<fs::path> entries;
        for (const auto& e : fs::directory_iterator(stage))
            entries.push_back(e.path());
        if (entries.size() == 1 && fs::is_directory(fs::symlink_status(entries[0])))
            work = entries[0];

        // Heredocs name the final keg; files go into the temporary keg,
        // which is renamed into the Cellar below.
        InstallPaths paths{dest, keg_tmp, config.store.parent_path(), pkg.name, pkg.version};
        std::cout << "==> Installing " << pkg.name << " " << pkg.version << "\n";
        apply_install_steps(*steps, work, paths);

        if (fs::exists(fs::symlink_status(dest)))
            fs::remove_all(dest);
        try {
            fs::create_directories(dest.parent_path());
        } catch (const fs::filesystem_error& e) {
            throw UserError("cannot create Cellar directory '" + dest.parent_path().string() +
                            "': " + e.code().message());
        }
        fs::rename(keg_tmp, dest);
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();

    mark_keg_owned(dest, pkg.name, pkg.version);
    ensure_prefix_opt_link(config.store, pkg.name, dest);
    return dest;
}

} // namespace den

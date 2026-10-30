// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "launchd_plist.h"

#include "../core/error.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>

namespace den {

namespace {

// XML-escape a string for interpolation into plist XML attribute values or
// character data (escapes &, <, >, ", ').
std::string xml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::string xml_unescape(const std::string& s) {
    static const std::pair<std::string_view, char> kEntities[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        bool matched = false;
        if (s[i] == '&') {
            for (const auto& [entity, c] : kEntities) {
                if (s.compare(i, entity.size(), entity) == 0) {
                    out += c;
                    i += entity.size();
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            out += s[i++];
        }
    }
    return out;
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        throw Error("cannot read " + p.string());
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void write_text_atomic(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << text;
        f.flush();
        if (!f) {
            throw Error("cannot write " + tmp.string());
        }
    }
    fs::rename(tmp, p);
}

/// Offsets of the <string> value that follows `<key>key</key>`: [open, close)
/// spans the raw (escaped) value. Nullopt when the key or its <string> is
/// missing.
std::optional<std::pair<std::size_t, std::size_t>> string_value_span(const std::string& plist,
                                                                     const std::string& key) {
    const std::string tag = "<key>" + key + "</key>";
    auto at = plist.find(tag);
    if (at == std::string::npos) {
        return std::nullopt;
    }
    auto open = plist.find_first_not_of(" \t\r\n", at + tag.size());
    constexpr std::string_view kOpen = "<string>";
    if (open == std::string::npos || plist.compare(open, kOpen.size(), kOpen) != 0) {
        return std::nullopt;
    }
    open += kOpen.size();
    auto close = plist.find("</string>", open);
    if (close == std::string::npos) {
        return std::nullopt;
    }
    return std::make_pair(open, close);
}

std::string normal_dir(const std::string& d) {
    auto s = fs::path(d).lexically_normal().string();
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (const auto& s : v) {
        if (!out.empty()) {
            out += sep;
        }
        out += s;
    }
    return out;
}

} // namespace

std::string launchd_path(const fs::path& den_home, const fs::path& prefix) {
    std::vector<std::string> dirs;
    if (!prefix.empty()) {
        dirs.push_back(normal_dir((prefix / "bin").string()));
    }
    if (!den_home.empty()) {
        dirs.push_back(normal_dir((den_home / "bin").string()));
    }
    for (const char* sys : {"/usr/bin", "/bin", "/usr/sbin", "/sbin"}) {
        dirs.emplace_back(sys);
    }
    // Drop repeats (e.g. a prefix of /usr), keeping the first.
    std::vector<std::string> out;
    for (auto& d : dirs) {
        if (std::find(out.begin(), out.end(), d) == out.end()) {
            out.push_back(std::move(d));
        }
    }
    return join(out, ":");
}

std::string launchd_plist(const fs::path& den_binary, const fs::path& den_home,
                          const fs::path& prefix) {
    std::string bin = xml_escape(den_binary.string());
    std::string home = xml_escape(den_home.string());
    // Carry an explicit DEN_PREFIX into the agent so the daemon maintains
    // the same store as the shell that installed it (store isolation).
    std::string prefix_env;
    if (const char* dp = std::getenv("DEN_PREFIX"); dp && *dp) {
        prefix_env =
            "        <key>DEN_PREFIX</key>\n        <string>" + xml_escape(dp) + "</string>\n";
    }

    const std::string path = launchd_path(den_home, prefix);

    std::ostringstream o;
    o << R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
    "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>dev.den.daemon</string>

    <key>ProgramArguments</key>
    <array>
        <string>)"
      << bin << R"(</string>
        <string>daemon</string>
        <string>run</string>
    </array>

    <key>EnvironmentVariables</key>
    <dict>
        <key>DEN_HOME</key>
        <string>)"
      << home << R"(</string>
)" << prefix_env
      << R"(        <key>PATH</key>
        <string>)"
      << xml_escape(path) << R"(</string>
    </dict>

    <key>RunAtLoad</key>
    <true/>

    <key>KeepAlive</key>
    <true/>

    <key>StandardOutPath</key>
    <string>)"
      << home << R"(/daemon.log</string>

    <key>StandardErrorPath</key>
    <string>)"
      << home << R"(/daemon.log</string>
</dict>
</plist>
)";
    return o.str();
}

std::optional<std::string> plist_env_value(const std::string& plist, const std::string& key) {
    auto span = string_value_span(plist, key);
    if (!span) {
        return std::nullopt;
    }
    return xml_unescape(plist.substr(span->first, span->second - span->first));
}

std::string plist_path_value(const std::string& plist) {
    return plist_env_value(plist, "PATH").value_or("");
}

std::optional<std::string> plist_with_path_value(const std::string& plist,
                                                 const std::string& path) {
    auto span = string_value_span(plist, "PATH");
    if (!span) {
        return std::nullopt;
    }
    return plist.substr(0, span->first) + xml_escape(path) + plist.substr(span->second);
}

std::vector<std::string> plist_unrecognised_keys(const std::string& plist) {
    static const std::set<std::string> kKnown = {
        "Label",    "ProgramArguments", "EnvironmentVariables", "DEN_HOME",        "DEN_PREFIX",
        "PATH",     "RunAtLoad",        "KeepAlive",            "StandardOutPath", "StandardErrorPath"};
    std::vector<std::string> out;
    constexpr std::string_view kOpen = "<key>";
    for (auto at = plist.find(kOpen); at != std::string::npos; at = plist.find(kOpen, at)) {
        at += kOpen.size();
        auto close = plist.find("</key>", at);
        if (close == std::string::npos) {
            break;
        }
        auto key = xml_unescape(plist.substr(at, close - at));
        if (!kKnown.count(key)) {
            out.push_back(std::move(key));
        }
        at = close;
    }
    return out;
}

std::vector<std::string> stale_homebrew_path_dirs(const std::string& path, const fs::path& prefix,
                                                  const fs::path& homebrew_prefix) {
    std::set<std::string> brew = {"/opt/homebrew/bin", "/usr/local/bin"};
    if (!homebrew_prefix.empty()) {
        brew.insert(normal_dir((homebrew_prefix / "bin").string()));
        brew.insert(normal_dir((homebrew_prefix / "sbin").string()));
    }
    if (!prefix.empty()) {
        brew.erase(normal_dir((prefix / "bin").string()));
        brew.erase(normal_dir((prefix / "sbin").string()));
    }
    std::vector<std::string> out;
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) {
            continue;
        }
        auto d = normal_dir(dir);
        if (brew.count(d) && std::find(out.begin(), out.end(), d) == out.end()) {
            out.push_back(d);
        }
    }
    return out;
}

std::string stale_daemon_path_problem(const fs::path& plist_file, const std::string& plist,
                                      const fs::path& prefix, const fs::path& homebrew_prefix) {
    fs::path den_prefix = prefix;
    if (auto dp = plist_env_value(plist, "DEN_PREFIX"); dp && !dp->empty()) {
        den_prefix = *dp;
    }
    const auto stale = stale_homebrew_path_dirs(plist_path_value(plist), den_prefix, homebrew_prefix);
    if (stale.empty()) {
        return {};
    }
    return "den daemon LaunchAgent PATH includes Homebrew's " + join(stale, ", ") +
           ", which is not den's prefix (" + den_prefix.string() +
           "), so the daemon and anything it runs can pick up Homebrew's tools: " +
           plist_file.string() + "\n  Regenerate it with `den daemon install` (`den self-update` "
           "also refreshes its PATH).";
}

PlistPathRefresh refresh_launchd_plist_path(const fs::path& plist_file, const std::string& path,
                                            const fs::path& backup, std::ostream& out) {
    const auto text = read_text(plist_file);
    const auto current = plist_env_value(text, "PATH");
    if (current && *current == path) {
        return PlistPathRefresh::UpToDate;
    }
    const auto unknown = plist_unrecognised_keys(text);
    if (!current || !unknown.empty()) {
        out << "Left the den daemon plist alone: "
            << (unknown.empty() ? std::string("it has no PATH entry")
                                : "it has keys den does not write (" + join(unknown, ", ") + ")")
            << ".\n  " << plist_file.string() << "\n  PATH is "
            << (current ? *current : std::string("unset")) << "\n  den would use " << path
            << "\n  Run `den daemon install` to regenerate it.\n";
        return PlistPathRefresh::LeftAlone;
    }
    const auto updated = plist_with_path_value(text, path);
    write_text_atomic(backup, text);
    write_text_atomic(plist_file, *updated);
    out << "Updated the den daemon's PATH in " << plist_file.string() << "\n  was " << *current
        << "\n  now " << path << "\n  (previous plist saved to " << backup.string() << ")\n";
    return PlistPathRefresh::Rewritten;
}

bool write_daemon_plist(const fs::path& plist_file, const std::string& generated,
                        const fs::path& backup, std::ostream& out) {
    std::error_code ec;
    if (fs::is_regular_file(plist_file, ec)) {
        const auto old = read_text(plist_file);
        if (old == generated) {
            return false;
        }
        write_text_atomic(backup, old);
        const auto old_path = plist_env_value(old, "PATH");
        const auto new_path = plist_path_value(generated);
        if (old_path && *old_path != new_path) {
            out << "Daemon PATH: " << *old_path << " -> " << new_path << "\n";
        }
        if (auto unknown = plist_unrecognised_keys(old); !unknown.empty()) {
            out << "warning: the regenerated plist drops keys den does not write ("
                << join(unknown, ", ") << "); they are kept in the backup.\n";
        }
        out << "Previous plist saved to " << backup.string() << "\n";
    }
    write_text_atomic(plist_file, generated);
    return true;
}

} // namespace den

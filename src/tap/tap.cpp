// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "tap.h"

#include "../core/error.h"
#include "../settings/settings.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string_view>
#include <utility>

#include <sys/wait.h>

namespace den {

namespace {

// Split "user/repo" into its two components. Returns nullopt unless the name is
// exactly two non-empty, traversal-free components.
std::optional<std::pair<std::string, std::string>> split_tap_name(const std::string& name) {
    auto slash = name.find('/');
    if (slash == std::string::npos)
        return std::nullopt;
    // Reject a second slash: tap names are exactly user/repo.
    if (name.find('/', slash + 1) != std::string::npos)
        return std::nullopt;
    std::string user = name.substr(0, slash);
    std::string repo = name.substr(slash + 1);
    if (user.empty() || repo.empty())
        return std::nullopt;
    // Reject path-traversal and absolute-ish components — these names build
    // filesystem paths and arrive from untrusted CLI input.
    for (const auto& comp : {user, repo}) {
        if (comp == "." || comp == ".." || comp.find('/') != std::string::npos ||
            comp.find('\\') != std::string::npos)
            return std::nullopt;
    }
    return std::make_pair(user, repo);
}

// Run a command, returning {exit_code, combined_output}. Used for `git clone`.
std::pair<int, std::string> run_capture(const std::string& cmd) {
    std::string output;
    std::array<char, 4096> buf{};
    FILE* pipe = ::popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe)
        return {-1, "popen failed"};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        output += buf.data();
    int status = ::pclose(pipe);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
}

// A source is treated as a git remote (cloned) when it has a URL scheme or the
// scp-like git form; otherwise it is treated as a local directory (copied).
bool is_remote_source(const std::string& source) {
    static const std::array<std::string_view, 5> kSchemes = {"https://", "http://", "git://",
                                                             "ssh://", "git@"};
    for (auto scheme : kSchemes) {
        if (source.rfind(scheme, 0) == 0)
            return true;
    }
    return false;
}

// Extract a single quoted string assigned to a top-level DSL keyword, e.g.
//   desc "Foo"  ->  "Foo"
// Returns empty when absent.
std::string extract_field(const std::string& src, const std::string& keyword) {
    std::regex re(keyword + R"RE(\s+"([^"]*)")RE");
    std::smatch m;
    if (std::regex_search(src, m, re))
        return m[1].str();
    return "";
}

// Derive a version string from a formula. Prefers an explicit `version "x"`;
// otherwise lifts the last path segment of the url, stripping common archive
// suffixes and a leading "v". Returns "0" when nothing is derivable.
std::string derive_version(const std::string& src) {
    auto explicit_ver = extract_field(src, "version");
    if (!explicit_ver.empty())
        return explicit_ver;

    auto url = extract_field(src, "url");
    if (url.empty())
        return "0";
    auto slash = url.find_last_of('/');
    std::string tail = slash == std::string::npos ? url : url.substr(slash + 1);
    // Strip archive suffixes.
    for (const auto& suffix : {".tar.gz", ".tar.bz2", ".tar.xz", ".tgz", ".zip", ".tar"}) {
        if (tail.size() > std::string(suffix).size() &&
            tail.compare(tail.size() - std::string(suffix).size(), std::string(suffix).size(),
                         suffix) == 0) {
            tail = tail.substr(0, tail.size() - std::string(suffix).size());
            break;
        }
    }
    // Strip a leading "v" on tag-style names ("v1.2.3" -> "1.2.3").
    if (tail.size() > 1 && (tail[0] == 'v' || tail[0] == 'V') &&
        std::isdigit((unsigned char)tail[1]))
        tail = tail.substr(1);
    return tail.empty() ? "0" : tail;
}

// Add a parsed formula file to the index under both its bare and qualified
// names. Never overwrites an existing core entry for the bare name.
void index_formula(PackageIndex& idx, const std::string& tap_name, const fs::path& rb_file) {
    std::ifstream f(rb_file);
    if (!f) {
        SPDLOG_WARN("tap: cannot read formula {}", rb_file.string());
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string src = ss.str();

    std::string bare = rb_file.stem().string();
    if (bare.empty())
        return;

    Package pkg;
    pkg.name = bare;
    pkg.version = derive_version(src);
    pkg.description = extract_field(src, "desc");
    pkg.homepage = extract_field(src, "homepage");
    pkg.license = extract_field(src, "license");
    pkg.source_url = extract_field(src, "url");
    pkg.ruby_source_path = fs::absolute(rb_file).string();

    // Qualified name is always added (tap-owned).
    Package qualified = pkg;
    qualified.name = tap_name + "/" + bare;
    idx.packages[qualified.name] = qualified;

    // Bare name: only fill it if a core formula has not already claimed it.
    if (!idx.find(bare))
        idx.packages[bare] = pkg;
}

} // namespace

bool is_valid_tap_name(const std::string& name) {
    return split_tap_name(name).has_value();
}

std::optional<fs::path> tap_dir(const Config& config, const std::string& name) {
    auto parts = split_tap_name(name);
    if (!parts)
        return std::nullopt;
    return config.den_home / "taps" / parts->first / parts->second;
}

Tap tap_add(const Config& config, const std::string& name, const std::string& source) {
    auto parts = split_tap_name(name);
    if (!parts)
        throw UserError("invalid tap name '" + name + "' (expected user/repo)");
    if (source.empty())
        throw UserError("tap '" + name + "' requires a source URL or path");

    auto dest = config.den_home / "taps" / parts->first / parts->second;

    if (fs::exists(dest)) {
        throw UserError("tap '" + name + "' is already tapped");
    }

    fs::create_directories(dest.parent_path());

    if (is_remote_source(source)) {
        auto [rc, out] = run_capture("git clone --depth 1 " + source + " " + dest.string());
        if (rc != 0) {
            std::error_code ec;
            fs::remove_all(dest, ec);
            throw UserError("failed to clone tap '" + name + "': " + out);
        }
    } else {
        // Local path: copy the directory tree. Validate the source exists and
        // is a directory before touching the filesystem.
        std::error_code ec;
        fs::path src_path = fs::absolute(source);
        if (!fs::is_directory(src_path, ec)) {
            throw UserError("tap source '" + source + "' is not a directory");
        }
        fs::copy(src_path, dest, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        if (ec) {
            fs::remove_all(dest, ec);
            throw UserError("failed to copy tap '" + name + "' from '" + source +
                            "': " + ec.message());
        }
    }

    // Register in config.json.
    auto settings = read_settings(config.den_home);
    settings.taps.taps[name] = source;
    write_settings(config.den_home, settings);

    SPDLOG_INFO("tapped {} from {}", name, source);
    return Tap{name, source, dest};
}

std::vector<Tap> tap_list(const Config& config) {
    auto settings = read_settings(config.den_home);
    std::vector<Tap> taps;
    for (const auto& [name, url] : settings.taps.taps) {
        auto dir = tap_dir(config, name);
        taps.push_back(Tap{name, url, dir ? *dir : fs::path{}});
    }
    std::sort(taps.begin(), taps.end(), [](const Tap& a, const Tap& b) { return a.name < b.name; });
    return taps;
}

void tap_remove(const Config& config, const std::string& name) {
    auto settings = read_settings(config.den_home);
    auto it = settings.taps.taps.find(name);
    if (it == settings.taps.taps.end()) {
        throw UserError("tap '" + name + "' is not tapped");
    }

    auto dir = tap_dir(config, name);
    if (dir) {
        std::error_code ec;
        fs::remove_all(*dir, ec);
        if (ec)
            SPDLOG_WARN("tap: could not remove {}: {}", dir->string(), ec.message());
    }

    settings.taps.taps.erase(it);
    write_settings(config.den_home, settings);
    SPDLOG_INFO("untapped {}", name);
}

void merge_taps_into_index(const Config& config, PackageIndex& idx) {
    for (const auto& tap : tap_list(config)) {
        auto formula_dir = tap.path / "Formula";
        std::error_code ec;
        if (!fs::is_directory(formula_dir, ec)) {
            // Homebrew also allows formulae at the tap root; fall back to it.
            formula_dir = tap.path;
            if (!fs::is_directory(formula_dir, ec))
                continue;
        }
        // recursive_directory_iterator with skip-on-error keeps a malformed tap
        // from aborting the whole merge.
        for (fs::recursive_directory_iterator it(formula_dir, ec), end; it != end;
             it.increment(ec)) {
            if (ec) {
                SPDLOG_WARN("tap: error walking {}: {}", formula_dir.string(), ec.message());
                break;
            }
            const auto& entry = *it;
            if (entry.is_regular_file(ec) && entry.path().extension() == ".rb") {
                index_formula(idx, tap.name, entry.path());
            }
        }
    }
}

} // namespace den

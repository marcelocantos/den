// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "opt_prefix_build.h"

#include "../core/error.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace den {

namespace {

std::string quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

std::vector<std::string> words(const std::string& s) {
    std::istringstream in(s);
    std::vector<std::string> out;
    for (std::string w; in >> w;)
        out.push_back(w);
    return out;
}

// Split on "&&" so each command of a chain is classified on its own.
std::vector<std::string> split_chain(const std::string& cmd) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (auto pos = cmd.find("&&"); pos != std::string::npos; pos = cmd.find("&&", start)) {
        parts.push_back(cmd.substr(start, pos - start));
        start = pos + 2;
    }
    parts.push_back(cmd.substr(start));
    return parts;
}

enum class InstallKind { None, Make, Env };

InstallKind classify(const std::string& segment) {
    auto w = words(segment);
    std::size_t i = 0;
    // Leading VAR=value assignments.
    while (i < w.size() && w[i].find('=') != std::string::npos && w[i][0] != '-')
        ++i;
    if (i >= w.size())
        return InstallKind::None;
    const auto tool = fs::path(w[i]).filename().string();
    const std::vector<std::string> args(w.begin() + static_cast<std::ptrdiff_t>(i) + 1, w.end());
    if (tool == "make" || tool == "gmake") {
        for (const auto& a : args) {
            if (a == "install" || a.starts_with("install-") || a.starts_with("install_"))
                return InstallKind::Make;
        }
        return InstallKind::None;
    }
    if (tool == "cmake") {
        for (std::size_t j = 0; j < args.size(); ++j) {
            if (args[j] == "--install" || args[j] == "--target=install")
                return InstallKind::Env;
            if ((args[j] == "--target" || args[j] == "-t") && j + 1 < args.size() &&
                args[j + 1] == "install")
                return InstallKind::Env;
        }
        return InstallKind::None;
    }
    if (tool == "ninja" || tool == "samu") {
        return std::find(args.begin(), args.end(), "install") != args.end() ? InstallKind::Env
                                                                            : InstallKind::None;
    }
    if (tool == "meson") {
        return !args.empty() && args[0] == "install" ? InstallKind::Env : InstallKind::None;
    }
    return InstallKind::None;
}

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\n");
    if (b == std::string::npos)
        return "";
    auto e = s.find_last_not_of(" \t\n");
    return s.substr(b, e - b + 1);
}

// Move `src` to `dst` (dst must not exist), across filesystems if need be.
void move_entry(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::rename(src, dst, ec);
    if (!ec)
        return;
    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    fs::remove_all(src);
}

// Merge directory `src` into `dst`. `replace` decides whether a staged file
// replaces one already at its destination.
void merge_tree(const fs::path& src, const fs::path& dst, bool replace) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    std::vector<fs::path> entries;
    for (const auto& e : fs::directory_iterator(src))
        entries.push_back(e.path());
    for (const auto& from : entries) {
        const auto to = dst / from.filename();
        const auto from_st = fs::symlink_status(from);
        const auto to_st = fs::symlink_status(to, ec);
        const bool to_exists = !ec && fs::exists(to_st);
        if (fs::is_directory(from_st) && to_exists && fs::is_directory(to_st)) {
            merge_tree(from, to, replace);
            continue;
        }
        if (to_exists) {
            if (!replace) {
                SPDLOG_INFO("keeping existing {} (not replacing with the staged copy)",
                            to.string());
                continue;
            }
            fs::remove_all(to);
        }
        move_entry(from, to);
    }
}

bool is_within(const fs::path& p, const fs::path& root) {
    auto [r, q] = std::mismatch(root.begin(), root.end(), p.begin(), p.end());
    return r == root.end();
}

std::vector<std::string> list_tree(const fs::path& root) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto& e = *it;
        std::string line = e.path().lexically_relative(root).string();
        std::error_code sec;
        auto st = e.symlink_status(sec);
        if (fs::is_symlink(st)) {
            line += " -> " + fs::read_symlink(e.path(), sec).string();
        } else if (fs::is_regular_file(st)) {
            line += " " + std::to_string(e.file_size(sec)) + " " +
                    std::to_string(
                        static_cast<long long>(e.last_write_time(sec).time_since_epoch().count()));
        }
        out.push_back(line);
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

fs::path opt_prefix_for(const fs::path& store, const std::string& name) {
    return store.parent_path() / "opt" / name;
}

std::string source_archive_suffix(const std::string& url) {
    auto end = url.find_first_of("?#");
    auto path = url.substr(0, end);
    auto slash = path.find_last_of('/');
    auto base = slash == std::string::npos ? path : path.substr(slash + 1);
    std::transform(base.begin(), base.end(), base.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::array<const char*, 14> kSuffixes = {
        ".tar.gz", ".tar.xz", ".tar.bz2", ".tar.zst", ".tar.lz", ".tar.lzma", ".tar.z",
        ".tgz",    ".txz",    ".tbz2",    ".tbz",     ".tar",    ".zip",      ".7z",
    };
    for (const char* s : kSuffixes) {
        if (base.size() > std::char_traits<char>::length(s) && base.ends_with(s))
            return s;
    }
    return ".src";
}

fs::path source_archive_path(const fs::path& cache, const std::string& name,
                             const std::string& version, const std::string& url) {
    return cache / "sources" / (name + "-" + version + source_archive_suffix(url));
}

bool is_install_step(const std::string& shell_cmd) {
    for (const auto& seg : split_chain(shell_cmd)) {
        if (classify(seg) != InstallKind::None)
            return true;
    }
    return false;
}

std::string with_destdir(const std::string& shell_cmd, const fs::path& stage) {
    const auto parts = split_chain(shell_cmd);
    std::string out;
    bool changed = false;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        std::string seg = parts[i];
        switch (classify(seg)) {
        case InstallKind::Make:
            seg = " " + trim(seg) + " DESTDIR=" + quote(stage.string()) + " ";
            changed = true;
            break;
        case InstallKind::Env:
            seg = " DESTDIR=" + quote(stage.string()) + " " + trim(seg) + " ";
            changed = true;
            break;
        case InstallKind::None:
            break;
        }
        if (i)
            out += "&&";
        out += seg;
    }
    return changed ? trim(out) : shell_cmd;
}

std::string with_staged_keg(const std::string& shell_cmd, const fs::path& opt_prefix,
                            const fs::path& staged_keg) {
    const auto from = opt_prefix.string();
    const auto to = staged_keg.string();
    std::string out;
    std::size_t pos = 0;
    while (true) {
        const auto hit = shell_cmd.find(from, pos);
        if (hit == std::string::npos)
            break;
        const auto end = hit + from.size();
        // Only a whole path component: `<opt>` or `<opt>/...`, not `<opt>x`.
        const bool whole =
            end == shell_cmd.size() || shell_cmd[end] == '/' ||
            !(std::isalnum(static_cast<unsigned char>(shell_cmd[end])) || shell_cmd[end] == '_' ||
              shell_cmd[end] == '-' || shell_cmd[end] == '.' || shell_cmd[end] == '@' ||
              shell_cmd[end] == '+');
        out += shell_cmd.substr(pos, hit - pos);
        out += whole ? to : from;
        pos = end;
    }
    out += shell_cmd.substr(pos);
    return out;
}

std::vector<std::string> staged_strays(const fs::path& stage, const fs::path& opt_prefix,
                                       const fs::path& prefix) {
    std::vector<std::string> stray;
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(stage, ec)))
        return stray;
    const auto staged_keg = stage / opt_prefix.relative_path();
    const auto staged_etc = stage / (prefix / "etc").relative_path();
    const auto staged_var = stage / (prefix / "var").relative_path();
    fs::recursive_directory_iterator it(stage, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto& p = it->path();
        if (is_within(p, staged_keg) || is_within(p, staged_etc) || is_within(p, staged_var)) {
            it.disable_recursion_pending();
            continue;
        }
        std::error_code sec;
        if (!fs::is_directory(it->symlink_status(sec)))
            stray.push_back("/" + p.lexically_relative(stage).string());
    }
    return stray;
}

void relocate_staged_keg(const fs::path& staged_keg, const fs::path& opt_prefix) {
    const auto from = staged_keg.string();
    const auto to = opt_prefix.string();
    std::vector<std::string> binary;
    std::error_code ec;
    fs::recursive_directory_iterator it(staged_keg, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto& p = it->path();
        std::error_code sec;
        const auto st = it->symlink_status(sec);
        if (fs::is_symlink(st)) {
            auto target = fs::read_symlink(p, sec).string();
            if (target.find(from) == std::string::npos)
                continue;
            for (std::size_t at; (at = target.find(from)) != std::string::npos;)
                target.replace(at, from.size(), to);
            fs::remove(p, sec);
            fs::create_symlink(target, p, sec);
            continue;
        }
        if (!fs::is_regular_file(st))
            continue;
        std::string data;
        {
            std::ifstream in(p, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            data = ss.str();
        }
        if (data.find(from) == std::string::npos)
            continue;
        if (data.find('\0') != std::string::npos) {
            binary.push_back(p.lexically_relative(staged_keg).string());
            continue;
        }
        for (std::size_t at = 0; (at = data.find(from, at)) != std::string::npos; at += to.size())
            data.replace(at, from.size(), to);
        const auto perms = fs::status(p, sec).permissions();
        {
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            out << data;
        }
        fs::permissions(p, perms, sec);
    }
    if (!binary.empty()) {
        std::string list;
        for (const auto& b : binary)
            list += "\n  " + b;
        throw UserError("source build baked its staging path into binary files, which den can't "
                        "relocate (see docs/source-builds.md):" +
                        list);
    }
}

void merge_destdir_stage(const fs::path& stage, const fs::path& opt_prefix, const fs::path& keg,
                         const fs::path& prefix) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(stage, ec)))
        return;

    const auto staged_keg = stage / opt_prefix.relative_path();
    const auto staged_etc = stage / (prefix / "etc").relative_path();
    const auto staged_var = stage / (prefix / "var").relative_path();

    // Anything staged outside the keg, etc and var is a stray write.
    const auto stray = staged_strays(stage, opt_prefix, prefix);
    if (!stray.empty()) {
        fs::remove_all(stage, ec);
        std::string list;
        for (const auto& s : stray)
            list += "\n  " + s;
        throw UserError("source build installed files outside its keg (expected everything under " +
                        opt_prefix.string() + "):" + list);
    }

    if (fs::is_directory(staged_keg))
        merge_tree(staged_keg, keg, /*replace=*/true);
    if (fs::is_directory(staged_etc))
        merge_tree(staged_etc, prefix / "etc", /*replace=*/false);
    if (fs::is_directory(staged_var))
        merge_tree(staged_var, prefix / "var", /*replace=*/false);
    fs::remove_all(stage, ec);
}

OptPathGuard::OptPathGuard(fs::path opt_path) : opt_path_(std::move(opt_path)) {
    std::error_code ec;
    auto st = fs::symlink_status(opt_path_, ec);
    existed_ = !ec && fs::exists(st);
    if (!existed_)
        return;
    was_symlink_ = fs::is_symlink(st);
    if (was_symlink_)
        link_target_ = fs::read_symlink(opt_path_, ec);
    listing_ = list_tree(opt_path_);
}

bool OptPathGuard::unchanged() const {
    std::error_code ec;
    auto st = fs::symlink_status(opt_path_, ec);
    const bool exists = !ec && fs::exists(st);
    if (!existed_ || !exists)
        return existed_ == exists;
    if (fs::is_symlink(st) != was_symlink_ ||
        (was_symlink_ && fs::read_symlink(opt_path_, ec) != link_target_))
        return false;
    return list_tree(opt_path_) == listing_;
}

bool OptPathGuard::settle() const {
    if (unchanged())
        return true;
    if (existed_)
        return false;
    std::error_code ec;
    fs::remove_all(opt_path_, ec);
    return !ec;
}

void OptPathGuard::check() const {
    std::error_code ec;
    auto st = fs::symlink_status(opt_path_, ec);
    const bool exists = !ec && fs::exists(st);
    const std::string hint = " (an install step ignored DESTDIR; see docs/source-builds.md)";
    if (!existed_) {
        if (!exists)
            return;
        fs::remove_all(opt_path_, ec);
        throw UserError("source build wrote " + opt_path_.string() + " directly" + hint +
                        "; removed it");
    }
    if (!exists)
        throw UserError("source build removed " + opt_path_.string() + hint);
    if (fs::is_symlink(st) != was_symlink_ ||
        (was_symlink_ && fs::read_symlink(opt_path_, ec) != link_target_))
        throw UserError("source build replaced " + opt_path_.string() + hint);
    if (list_tree(opt_path_) != listing_)
        throw UserError("source build modified files under " + opt_path_.string() + hint);
}

} // namespace den

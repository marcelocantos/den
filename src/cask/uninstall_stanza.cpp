// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "uninstall_stanza.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace den {

namespace {

std::string sh_quote(const std::string& s) {
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

/// Run a shell command, capture stdout+stderr, return exit status.
int run_capture(const std::string& cmd, std::string& out) {
    out.clear();
    FILE* p = ::popen((cmd + " 2>&1").c_str(), "r");
    if (!p)
        return -1;
    std::array<char, 512> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), p))
        out += buf.data();
    int rc = ::pclose(p);
    if (rc != -1 && WIFEXITED(rc))
        return WEXITSTATUS(rc);
    return -1;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    return s.substr(i);
}

bool all_chars(const std::string& s, std::string_view extra) {
    if (s.empty() || !std::isalnum(static_cast<unsigned char>(s[0])))
        return false;
    return std::all_of(s.begin(), s.end(), [&](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) ||
               extra.find(c) != std::string_view::npos;
    });
}

class RealCaskSystem final : public CaskSystem {
  public:
    bool app_running(const std::string& bundle_id) override {
        std::string out;
        run_capture("/usr/bin/lsappinfo find " + sh_quote("bundleid=" + bundle_id), out);
        return !trim(out).empty();
    }
    bool request_quit(const std::string& bundle_id, std::string& err) override {
        const std::string cmd = "/usr/bin/osascript -e 'with timeout of 30 seconds' -e " +
                                sh_quote("tell application id \"" + bundle_id + "\" to quit") +
                                " -e 'end timeout'";
        std::string out;
        int rc = run_capture(cmd, out);
        if (rc != 0) {
            err = trim(out).empty() ? ("osascript exit " + std::to_string(rc)) : trim(out);
            return false;
        }
        return true;
    }
    std::vector<int> app_pids(const std::string& bundle_id) override {
        std::vector<int> pids;
        std::string out;
        run_capture("/usr/bin/lsappinfo find " + sh_quote("bundleid=" + bundle_id), out);
        size_t pos = 0;
        while ((pos = out.find("ASN:", pos)) != std::string::npos) {
            size_t end = out.find_first_of(" \n\t", pos);
            std::string asn =
                out.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? out.size() : end;
            // Lines look like ASN:0x0-0x1234:"Name": → keep up to the quote.
            auto q = asn.find('"');
            if (q != std::string::npos)
                asn = asn.substr(0, q);
            std::string info;
            run_capture("/usr/bin/lsappinfo info -only pid " + sh_quote(asn), info);
            auto eq = info.find("\"pid\"=");
            if (eq != std::string::npos) {
                int pid = std::atoi(info.c_str() + eq + 6);
                if (pid > 1)
                    pids.push_back(pid);
            }
        }
        return pids;
    }
    bool send_signal(int pid, int sig) override { return pid > 1 && ::kill(pid, sig) == 0; }
    bool launchctl_loaded(const std::string& label, bool system_domain) override {
        std::string out;
        if (system_domain)
            return run_capture("/bin/launchctl print " + sh_quote("system/" + label), out) == 0;
        return run_capture("/bin/launchctl list " + sh_quote(label), out) == 0;
    }
    bool launchctl_remove(const std::string& label, bool system_domain, std::string& err) override {
        std::string out;
        const std::string cmd = system_domain
                                    ? "/bin/launchctl bootout " + sh_quote("system/" + label)
                                    : "/bin/launchctl remove " + sh_quote(label);
        if (run_capture(cmd, out) != 0) {
            err = trim(out);
            return false;
        }
        return true;
    }
    std::vector<std::string> pkgutil_match(const std::string& pattern) override {
        std::vector<std::string> ids;
        std::string out;
        if (run_capture("/usr/sbin/pkgutil " + sh_quote("--pkgs=" + pattern), out) != 0)
            return ids;
        size_t start = 0;
        while (start < out.size()) {
            size_t nl = out.find('\n', start);
            std::string line =
                trim(out.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
            if (!line.empty() && valid_bundle_id(line))
                ids.push_back(line);
            if (nl == std::string::npos)
                break;
            start = nl + 1;
        }
        return ids;
    }
    ForgetResult pkgutil_forget(const std::string& id, std::string& err) override {
        std::string out;
        int rc = run_capture("/usr/sbin/pkgutil --forget " + sh_quote(id), out);
        if (rc == 0)
            return ForgetResult::Ok;
        err = trim(out);
        if (!is_root())
            return ForgetResult::NeedsAdmin;
        return ForgetResult::Failed;
    }
    bool is_root() override { return ::geteuid() == 0; }
    void sleep_ms(int ms) override { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
};

CaskSystem* g_override = nullptr;

bool has_dotdot(const fs::path& p) {
    return std::any_of(p.begin(), p.end(), [](const fs::path& c) { return c == ".."; });
}

/// Directories a stanza may never name directly (they are parents of
/// everything). Children (e.g. /Library/Application Support/Foo) are fine.
bool is_protected(const fs::path& p, const StanzaOptions& opts) {
    static const std::array<std::string_view, 30> exact = {
        "/",
        "/Applications",
        "/Library",
        "/Library/Application Support",
        "/Library/Caches",
        "/Library/Preferences",
        "/Library/LaunchAgents",
        "/Library/LaunchDaemons",
        "/Library/PrivilegedHelperTools",
        "/Library/Frameworks",
        "/Library/Extensions",
        "/Users",
        "/Users/Shared",
        "/Volumes",
        "/private",
        "/private/var",
        "/private/tmp",
        "/private/etc",
        "/tmp",
        "/var",
        "/etc",
        "/opt",
        "/usr",
        "/usr/local",
        "/usr/local/bin",
        "/usr/local/lib",
        "/usr/local/share",
        "/cores",
        "/dev",
        "/Network",
    };
    const std::string s = p.lexically_normal().string();
    std::string norm = s.size() > 1 && s.back() == '/' ? s.substr(0, s.size() - 1) : s;
    for (auto e : exact)
        if (norm == e)
            return true;
    // Never under these trees at all.
    for (std::string_view pre : {"/System", "/bin", "/sbin", "/usr/bin", "/usr/sbin", "/usr/lib",
                                 "/usr/libexec", "/usr/share", "/opt/homebrew"}) {
        if (norm == pre || norm.starts_with(std::string(pre) + "/"))
            return true;
    }
    // The home directory and its top-level standard folders.
    if (!opts.home.empty()) {
        const fs::path h = opts.home.lexically_normal();
        for (const char* sub :
             {"", "Library", "Library/Application Support", "Library/Caches", "Library/Preferences",
              "Library/LaunchAgents", "Library/Containers", "Library/Group Containers",
              "Library/Logs", "Library/Saved Application State", "Applications", "Desktop",
              "Documents", "Downloads", ".Trash"}) {
            fs::path cand = std::string(sub).empty() ? h : (h / sub);
            std::string c = cand.lexically_normal().string();
            if (c.size() > 1 && c.back() == '/')
                c.pop_back();
            if (norm == c)
                return true;
        }
    }
    if (!opts.appdir.empty()) {
        std::string a = opts.appdir.lexically_normal().string();
        if (a.size() > 1 && a.back() == '/')
            a.pop_back();
        if (norm == a)
            return true;
    }
    return false;
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) {
        if (!s.empty())
            s += ", ";
        s += x;
    }
    return s;
}

void run_launchctl(const CaskUninstall& st, CaskSystem& sys, const StanzaOptions& opts,
                   bool remove_plists, StanzaReport& r) {
    for (const auto& label : st.launchctl) {
        if (!valid_launchd_label(label)) {
            r.warnings.push_back("launchctl: refusing invalid label '" + label + "'");
            continue;
        }
        bool acted = false;
        if (sys.launchctl_loaded(label, false)) {
            std::string err;
            if (sys.launchctl_remove(label, false, err))
                r.actions.push_back("launchctl: unloaded " + label);
            else
                r.warnings.push_back("launchctl: could not unload " + label + ": " + err);
            acted = true;
        }
        if (sys.launchctl_loaded(label, true)) {
            if (!sys.is_root()) {
                r.needs_admin.push_back("sudo launchctl bootout system/" + label);
            } else {
                std::string err;
                if (sys.launchctl_remove(label, true, err))
                    r.actions.push_back("launchctl: unloaded system/" + label);
                else
                    r.warnings.push_back("launchctl: could not unload system/" + label + ": " +
                                         err);
            }
            acted = true;
        }
        auto remove_plist = [&](const fs::path& dir, bool system) {
            const fs::path plist = dir / (label + ".plist");
            std::error_code ec;
            if (!fs::exists(fs::symlink_status(plist, ec)))
                return;
            acted = true;
            fs::remove(plist, ec);
            if (!ec) {
                r.actions.push_back("launchctl: removed " + plist.string());
            } else if (ec == std::errc::permission_denied ||
                       ec == std::errc::operation_not_permitted) {
                r.needs_admin.push_back("sudo rm " + sh_quote(plist.string()));
            } else {
                r.warnings.push_back("launchctl: could not remove " + plist.string() + ": " +
                                     ec.message());
            }
            (void)system;
        };
        if (remove_plists) {
            for (const auto& d : opts.user_agent_dirs)
                remove_plist(d, false);
            for (const auto& d : opts.system_agent_dirs)
                remove_plist(d, true);
        }
        if (!acted)
            r.actions.push_back("launchctl: " + label + " not loaded, no plist");
    }
}

/// Poll until `bundle_id` is no longer running or `timeout_ms` elapses.
bool wait_gone(CaskSystem& sys, const std::string& bundle_id, int timeout_ms) {
    const int step = 100;
    for (int waited = 0; waited < timeout_ms; waited += step) {
        if (!sys.app_running(bundle_id))
            return true;
        sys.sleep_ms(step);
    }
    return !sys.app_running(bundle_id);
}

void run_quit(const CaskUninstall& st, CaskSystem& sys, const StanzaOptions& opts,
              StanzaReport& r) {
    for (const auto& bid : st.quit) {
        if (!valid_bundle_id(bid)) {
            r.warnings.push_back("quit: refusing invalid bundle id '" + bid + "'");
            continue;
        }
        if (!sys.app_running(bid)) {
            r.actions.push_back("quit: " + bid + " not running");
            continue;
        }
        std::string err;
        if (!sys.request_quit(bid, err)) {
            r.warnings.push_back("quit: could not ask " + bid + " to quit: " + err);
            continue;
        }
        if (wait_gone(sys, bid, opts.quit_timeout_ms)) {
            r.actions.push_back("quit: " + bid + " quit");
        } else {
            r.warnings.push_back("quit: " + bid + " did not quit within " +
                                 std::to_string(opts.quit_timeout_ms / 1000) +
                                 "s; continuing (it may still be running)");
        }
    }
}

void run_signal(const CaskUninstall& st, CaskSystem& sys, const StanzaOptions& opts,
                StanzaReport& r) {
    for (const auto& [signame, bid] : st.signal) {
        const int sig = parse_signal(signame);
        if (sig <= 0) {
            r.warnings.push_back("signal: refusing unknown signal '" + signame + "'");
            continue;
        }
        if (!valid_bundle_id(bid)) {
            r.warnings.push_back("signal: refusing invalid bundle id '" + bid + "'");
            continue;
        }
        auto pids = sys.app_pids(bid);
        if (pids.empty()) {
            r.actions.push_back("signal: " + bid + " not running");
            continue;
        }
        for (int pid : pids) {
            if (sys.send_signal(pid, sig))
                r.actions.push_back("signal: sent " + signame + " to " + bid + " (pid " +
                                    std::to_string(pid) + ")");
            else
                r.warnings.push_back("signal: could not send " + signame + " to pid " +
                                     std::to_string(pid));
        }
        if (!wait_gone(sys, bid, opts.signal_wait_ms))
            r.warnings.push_back("signal: " + bid + " still running after " + signame);
    }
}

void run_pkgutil(const CaskUninstall& st, CaskSystem& sys, StanzaReport& r) {
    for (const auto& pattern : st.pkgutil) {
        if (!all_chars(pattern, "._-*")) {
            r.warnings.push_back("pkgutil: refusing invalid package id '" + pattern + "'");
            continue;
        }
        auto ids = sys.pkgutil_match(pattern);
        if (ids.empty()) {
            r.actions.push_back("pkgutil: no receipts match " + pattern);
            continue;
        }
        for (const auto& id : ids) {
            std::string err;
            switch (sys.pkgutil_forget(id, err)) {
            case CaskSystem::ForgetResult::Ok:
                r.actions.push_back("pkgutil: forgot " + id);
                break;
            case CaskSystem::ForgetResult::NeedsAdmin:
                r.needs_admin.push_back("sudo pkgutil --forget " + id);
                break;
            case CaskSystem::ForgetResult::Failed:
                r.warnings.push_back("pkgutil: --forget " + id + " failed: " + err);
                break;
            }
        }
    }
}

void run_paths(const std::vector<std::string>& paths, const char* what, CaskSystem& sys,
               const StanzaOptions& opts, StanzaReport& r) {
    (void)sys;
    const std::string kind(what);
    for (const auto& raw : paths) {
        std::string why;
        auto p = resolve_stanza_path(raw, opts, why);
        if (!p) {
            r.warnings.push_back(kind + ": refusing '" + raw + "': " + why);
            continue;
        }
        std::error_code ec;
        const auto st = fs::symlink_status(*p, ec);
        if (!fs::exists(st)) {
            r.actions.push_back(kind + ": " + p->string() + " not present");
            continue;
        }
        auto perm_or_warn = [&](const std::error_code& e, const std::string& cmd) {
            if (e == std::errc::permission_denied || e == std::errc::operation_not_permitted)
                r.needs_admin.push_back(cmd);
            else
                r.warnings.push_back(kind + ": " + p->string() + ": " + e.message());
        };
        if (kind == "delete") {
            fs::remove_all(*p, ec); // does not follow symlinks
            if (ec)
                perm_or_warn(ec, "sudo rm -rf " + sh_quote(p->string()));
            else
                r.actions.push_back("delete: removed " + p->string());
        } else if (kind == "trash") {
            fs::create_directories(opts.trash_dir, ec);
            fs::path dest = opts.trash_dir / p->filename();
            if (fs::exists(fs::symlink_status(dest, ec)))
                dest = opts.trash_dir /
                       (p->filename().string() + " " + std::to_string(std::time(nullptr)));
            ec.clear();
            fs::rename(*p, dest, ec);
            if (ec) {
                if (ec == std::errc::cross_device_link)
                    r.warnings.push_back("trash: " + p->string() +
                                         " is on another volume; left in place");
                else
                    perm_or_warn(ec, "sudo mv " + sh_quote(p->string()) + " ~/.Trash/");
            } else {
                r.actions.push_back("trash: moved " + p->string() + " to " + dest.string());
            }
        } else { // rmdir
            if (!fs::is_directory(st)) {
                r.warnings.push_back("rmdir: " + p->string() + " is not a directory; left");
                continue;
            }
            if (!fs::is_empty(*p, ec)) {
                r.actions.push_back("rmdir: " + p->string() + " not empty; left");
                continue;
            }
            fs::remove(*p, ec);
            if (ec)
                perm_or_warn(ec, "sudo rmdir " + sh_quote(p->string()));
            else
                r.actions.push_back("rmdir: removed " + p->string());
        }
    }
}

} // namespace

CaskSystem& real_cask_system() {
    static RealCaskSystem sys;
    return sys;
}

CaskSystem& cask_system() {
    return g_override ? *g_override : real_cask_system();
}

void set_cask_system(CaskSystem* sys) {
    g_override = sys;
}

StanzaOptions default_stanza_options(const fs::path& appdir) {
    StanzaOptions o;
    if (const char* h = std::getenv("HOME"); h && *h)
        o.home = h;
    o.trash_dir = o.home.empty() ? fs::path{} : o.home / ".Trash";
    o.appdir = appdir;
    if (const char* t = std::getenv("DEN_CASK_QUIT_TIMEOUT"); t && *t) {
        int secs = std::atoi(t);
        if (secs >= 0)
            o.quit_timeout_ms = secs * 1000;
    }
    if (!o.home.empty())
        o.user_agent_dirs.push_back(o.home / "Library" / "LaunchAgents");
    o.system_agent_dirs = {"/Library/LaunchAgents", "/Library/LaunchDaemons"};
    return o;
}

bool valid_bundle_id(const std::string& s) {
    return s.size() <= 255 && all_chars(s, "._-");
}

bool valid_launchd_label(const std::string& s) {
    return s.size() <= 255 && all_chars(s, "._-@");
}

int parse_signal(const std::string& s) {
    std::string n = s;
    if (n.starts_with("SIG"))
        n = n.substr(3);
    static const std::array<std::pair<std::string_view, int>, 9> names = {{
        {"TERM", SIGTERM},
        {"KILL", SIGKILL},
        {"INT", SIGINT},
        {"HUP", SIGHUP},
        {"QUIT", SIGQUIT},
        {"USR1", SIGUSR1},
        {"USR2", SIGUSR2},
        {"ABRT", SIGABRT},
        {"ALRM", SIGALRM},
    }};
    for (auto [name, v] : names)
        if (n == name)
            return v;
    if (!n.empty() && std::all_of(n.begin(), n.end(), ::isdigit)) {
        int v = std::atoi(n.c_str());
        return (v >= 1 && v <= 31) ? v : -1;
    }
    return -1;
}

std::optional<fs::path> resolve_stanza_path(const std::string& raw, const StanzaOptions& opts,
                                            std::string& why) {
    if (raw.empty()) {
        why = "empty path";
        return std::nullopt;
    }
    if (raw.find_first_of("*?[]{}$`\n") != std::string::npos) {
        why = "globs and variables are not supported";
        return std::nullopt;
    }
    std::string s = raw;
    if (s == "~" || s.starts_with("~/")) {
        if (opts.home.empty()) {
            why = "HOME is not set";
            return std::nullopt;
        }
        s = (opts.home / s.substr(s.size() > 1 ? 2 : 1)).string();
    } else if (s.starts_with("~")) {
        why = "~user paths are not supported";
        return std::nullopt;
    }
    fs::path p(s);
    if (!p.is_absolute()) {
        why = "relative path";
        return std::nullopt;
    }
    if (has_dotdot(p)) {
        why = "path contains '..'";
        return std::nullopt;
    }
    p = p.lexically_normal();
    // Sandboxed appdir: never reach into the real /Applications.
    if (!opts.appdir.empty() && opts.appdir.lexically_normal() != fs::path("/Applications")) {
        auto rel = p.lexically_relative("/Applications");
        if (!rel.empty() && *rel.begin() != ".." && rel != ".") {
            p = (opts.appdir / rel).lexically_normal();
        }
    }
    if (is_protected(p, opts)) {
        why = "protected location";
        return std::nullopt;
    }
    return p;
}

StanzaReport run_uninstall_stanza(const CaskUninstall& st, StanzaPhase phase, CaskSystem& sys,
                                  const StanzaOptions& opts) {
    StanzaReport r;
    if (phase == StanzaPhase::Stop || phase == StanzaPhase::PreRemove) {
        // Homebrew order: early_script, launchctl, quit, signal, login_item,
        // kext, script, pkgutil, delete, trash, rmdir.
        // Upgrade (Stop) only unloads: the plist may belong to the install the
        // new version keeps, and the swap's rollback could not restore it.
        run_launchctl(st, sys, opts, phase == StanzaPhase::PreRemove, r);
        run_quit(st, sys, opts, r);
        run_signal(st, sys, opts, r);
    }
    if (phase == StanzaPhase::PreRemove) {
        run_pkgutil(st, sys, r);
    }
    if (phase == StanzaPhase::PostRemove) {
        run_paths(st.delete_paths, "delete", sys, opts, r);
        run_paths(st.trash, "trash", sys, opts, r);
        run_paths(st.rmdir, "rmdir", sys, opts, r);
    }
    if (phase != StanzaPhase::PostRemove && !st.unsupported.empty()) {
        r.warnings.push_back("unsupported uninstall directive(s) not run: " + join(st.unsupported));
    }
    return r;
}

} // namespace den

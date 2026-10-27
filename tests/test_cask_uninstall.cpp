// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T12 — cask `uninstall` stanzas: parsing, each directive, path safety,
// quit timeout, and the install/upgrade/uninstall integration.

#include <doctest.h>

#include "cask/cask.h"
#include "cask/uninstall_stanza.h"
#include "core/config.h"
#include "index/cask_uninstall_json.h"
#include "index/index.h"
#include "index/package.h"
#include "platform/platform.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <archive.h>
#include <archive_entry.h>
#endif

namespace den {
namespace test {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

struct Tmp {
    fs::path path;
    Tmp() {
        path = fs::temp_directory_path() /
               ("den_t12_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void touch(const fs::path& p, const std::string& body = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}

bool has(const std::vector<std::string>& v, const std::string& needle) {
    for (const auto& s : v)
        if (s.find(needle) != std::string::npos)
            return true;
    return false;
}

/// Fake OS: apps are "running" bundle ids; quit removes them unless they
/// ignore quit. Time only advances through sleep_ms.
struct FakeSystem : CaskSystem {
    std::set<std::string> running;
    std::set<std::string> ignores_quit;
    std::set<std::string> quit_fails;
    std::map<std::string, std::vector<int>> pids;
    std::set<std::string> user_loaded, system_loaded;
    std::vector<std::string> pkgs;
    bool root = false;
    bool forget_needs_admin = false;
    long slept_ms = 0;
    std::vector<std::string> calls;
    std::function<void(const std::string&)> on_quit;

    bool app_running(const std::string& b) override { return running.count(b) > 0; }
    bool request_quit(const std::string& b, std::string& err) override {
        calls.push_back("quit " + b);
        if (on_quit)
            on_quit(b);
        if (quit_fails.count(b)) {
            err = "not authorised to send Apple events";
            return false;
        }
        if (!ignores_quit.count(b))
            running.erase(b);
        return true;
    }
    std::vector<int> app_pids(const std::string& b) override {
        auto it = pids.find(b);
        return running.count(b) && it != pids.end() ? it->second : std::vector<int>{};
    }
    bool send_signal(int pid, int sig) override {
        calls.push_back("kill " + std::to_string(pid) + " " + std::to_string(sig));
        for (auto& [b, ps] : pids)
            for (int p : ps)
                if (p == pid)
                    running.erase(b);
        return true;
    }
    bool launchctl_loaded(const std::string& l, bool sys) override {
        return sys ? system_loaded.count(l) > 0 : user_loaded.count(l) > 0;
    }
    bool launchctl_remove(const std::string& l, bool sys, std::string&) override {
        calls.push_back(std::string("launchctl remove ") + (sys ? "system/" : "") + l);
        (sys ? system_loaded : user_loaded).erase(l);
        return true;
    }
    std::vector<std::string> pkgutil_match(const std::string& pattern) override {
        std::vector<std::string> out;
        for (const auto& p : pkgs) {
            if (pattern.ends_with("*") ? p.starts_with(pattern.substr(0, pattern.size() - 1))
                                       : p == pattern)
                out.push_back(p);
        }
        return out;
    }
    ForgetResult pkgutil_forget(const std::string& id, std::string& err) override {
        calls.push_back("pkgutil --forget " + id);
        if (forget_needs_admin) {
            err = "Insufficient permissions";
            return ForgetResult::NeedsAdmin;
        }
        return ForgetResult::Ok;
    }
    bool is_root() override { return root; }
    void sleep_ms(int ms) override { slept_ms += ms; }
};

StanzaOptions opts_for(const fs::path& root) {
    StanzaOptions o;
    o.home = root / "home";
    o.trash_dir = o.home / ".Trash";
    o.appdir = root / "Applications";
    o.quit_timeout_ms = 2000;
    o.signal_wait_ms = 500;
    o.user_agent_dirs = {o.home / "Library" / "LaunchAgents"};
    o.system_agent_dirs = {root / "SystemLaunchDaemons"};
    fs::create_directories(o.home);
    fs::create_directories(o.appdir);
    return o;
}

} // namespace

TEST_SUITE("cask::uninstall_stanza") {

    TEST_CASE("Homebrew uninstall stanzas parse, unsupported directives are recorded") {
        auto arts = json::parse(R"([
          {"app": ["Demo.app"]},
          {"uninstall": [
            {"quit": "com.example.demo", "launchctl": ["com.example.agent", "com.example.helper"],
             "signal": ["TERM", "com.example.demo"], "on_upgrade": "quit"},
            {"pkgutil": "com.example.pkg.*", "delete": ["~/Library/Demo", "/Library/Demo"],
             "trash": "~/Library/Caches/Demo", "rmdir": "~/Library/DemoDir",
             "script": {"executable": "uninstall.sh"}, "early_script": "x", "kext": "com.example.kext",
             "login_item": "Demo", "signal_extra": 1}
          ]},
          {"zap": [{"trash": "~/Library/Preferences/com.example.demo.plist"}]}
        ])");
        CaskUninstall u;
        parse_cask_uninstall(arts, u);
        CHECK(u.quit == std::vector<std::string>{"com.example.demo"});
        CHECK(u.launchctl.size() == 2);
        REQUIRE(u.signal.size() == 1);
        CHECK(u.signal[0].first == "TERM");
        CHECK(u.pkgutil == std::vector<std::string>{"com.example.pkg.*"});
        CHECK(u.delete_paths.size() == 2);
        CHECK(u.trash == std::vector<std::string>{"~/Library/Caches/Demo"});
        CHECK(u.rmdir == std::vector<std::string>{"~/Library/DemoDir"});
        // zap is out of scope and must not leak into the uninstall stanza.
        CHECK_FALSE(has(u.trash, "Preferences"));
        for (const char* n : {"script", "early_script", "kext", "login_item", "signal_extra"})
            CHECK_MESSAGE(has(u.unsupported, n), n);
        CHECK_FALSE(has(u.unsupported, "on_upgrade"));

        // Round-trip through den's serialised form.
        CaskUninstall back;
        merge_uninstall_directives(uninstall_to_json(u), back);
        CHECK(back.quit == u.quit);
        CHECK(back.signal == u.signal);
        CHECK(back.delete_paths == u.delete_paths);
        CHECK(back.unsupported == u.unsupported);
    }

    TEST_CASE("signal accepts a list of pairs; parse_signal maps names and numbers") {
        CaskUninstall u;
        merge_uninstall_directives(json::parse(R"({"signal": [["TERM","a.b"],["KILL","c.d"]]})"),
                                   u);
        CHECK(u.signal.size() == 2);
        CHECK(parse_signal("TERM") == SIGTERM);
        CHECK(parse_signal("SIGKILL") == SIGKILL);
        CHECK(parse_signal("15") == 15);
        CHECK(parse_signal("BOGUS") == -1);
        CHECK(parse_signal("99") == -1);
    }

    TEST_CASE("cask_uninstall survives the index cache round trip") {
        Tmp t;
        PackageIndex idx;
        Package p;
        p.name = "demo";
        p.version = "1.0";
        p.artifact_type = ArtifactType::App;
        p.cask_uninstall.quit = {"com.example.demo"};
        p.cask_uninstall.unsupported = {"script"};
        idx.packages["demo"] = p;
        save_index(idx, t.path / "index.json");
        auto loaded = load_index(t.path / "index.json");
        REQUIRE(loaded.find("demo"));
        CHECK(loaded.find("demo")->cask_uninstall.quit == p.cask_uninstall.quit);
        CHECK(loaded.find("demo")->cask_uninstall.unsupported == p.cask_uninstall.unsupported);
    }

    TEST_CASE("quit asks a running app to quit and waits for it") {
        Tmp t;
        auto o = opts_for(t.path);
        FakeSystem sys;
        sys.running = {"com.example.demo"};
        CaskUninstall u;
        u.quit = {"com.example.demo", "com.example.notrunning"};
        auto r = run_uninstall_stanza(u, StanzaPhase::Stop, sys, o);
        CHECK(has(r.actions, "quit: com.example.demo quit"));
        CHECK(has(r.actions, "com.example.notrunning not running"));
        CHECK(sys.calls == std::vector<std::string>{"quit com.example.demo"});
        CHECK(r.warnings.empty());
    }

    TEST_CASE("quit times out on an app that ignores it, warns, and does not hang") {
        Tmp t;
        auto o = opts_for(t.path);
        o.quit_timeout_ms = 3000;
        FakeSystem sys;
        sys.running = {"com.example.stubborn"};
        sys.ignores_quit = {"com.example.stubborn"};
        CaskUninstall u;
        u.quit = {"com.example.stubborn"};
        auto r = run_uninstall_stanza(u, StanzaPhase::Stop, sys, o);
        CHECK(has(r.warnings, "did not quit within 3s"));
        CHECK(sys.slept_ms >= 3000);
        CHECK(sys.slept_ms <= 3100);
        CHECK(r.actions.empty());
    }

    TEST_CASE("quit reports when the quit request itself fails") {
        Tmp t;
        auto o = opts_for(t.path);
        FakeSystem sys;
        sys.running = {"com.example.demo"};
        sys.quit_fails = {"com.example.demo"};
        CaskUninstall u;
        u.quit = {"com.example.demo"};
        auto r = run_uninstall_stanza(u, StanzaPhase::Stop, sys, o);
        CHECK(has(r.warnings, "could not ask com.example.demo to quit"));
    }

    TEST_CASE("signal sends the parsed signal to the app's pids only") {
        Tmp t;
        auto o = opts_for(t.path);
        FakeSystem sys;
        sys.running = {"com.example.demo", "com.example.other"};
        sys.pids = {{"com.example.demo", {4242}}, {"com.example.other", {5151}}};
        CaskUninstall u;
        u.signal = {{"TERM", "com.example.demo"}, {"NOPE", "com.example.other"}};
        auto r = run_uninstall_stanza(u, StanzaPhase::Stop, sys, o);
        CHECK(has(r.actions, "sent TERM to com.example.demo (pid 4242)"));
        CHECK(sys.calls == std::vector<std::string>{"kill 4242 " + std::to_string(SIGTERM)});
        CHECK(has(r.warnings, "unknown signal 'NOPE'"));
        CHECK(sys.running.count("com.example.other") == 1);
    }

    TEST_CASE("invalid bundle ids and labels are refused before reaching the OS") {
        Tmp t;
        auto o = opts_for(t.path);
        FakeSystem sys;
        CaskUninstall u;
        u.quit = {"com.x\"; do shell script \"rm -rf ~"};
        u.launchctl = {"../evil", "a;rm -rf /"};
        u.pkgutil = {"com.x; rm -rf /"};
        auto r = run_uninstall_stanza(u, StanzaPhase::PreRemove, sys, o);
        CHECK(sys.calls.empty());
        CHECK(has(r.warnings, "quit: refusing invalid bundle id"));
        CHECK(has(r.warnings, "launchctl: refusing invalid label '../evil'"));
        CHECK(has(r.warnings, "pkgutil: refusing invalid package id"));
        CHECK_FALSE(valid_bundle_id("-flag"));
        CHECK(valid_launchd_label("com.example.agent"));
    }

    TEST_CASE("launchctl unloads and removes only the stanza's labels") {
        Tmp t;
        auto o = opts_for(t.path);
        const auto ua = o.user_agent_dirs[0];
        touch(ua / "com.example.agent.plist");
        touch(ua / "com.example.unrelated.plist");
        touch(o.system_agent_dirs[0] / "com.example.helper.plist");
        FakeSystem sys;
        sys.user_loaded = {"com.example.agent", "com.example.unrelated"};
        sys.system_loaded = {"com.example.helper"};
        CaskUninstall u;
        u.launchctl = {"com.example.agent", "com.example.helper", "com.example.absent"};
        auto r = run_uninstall_stanza(u, StanzaPhase::PreRemove, sys, o);
        CHECK(has(r.actions, "launchctl: unloaded com.example.agent"));
        CHECK_FALSE(fs::exists(ua / "com.example.agent.plist"));
        CHECK(fs::exists(ua / "com.example.unrelated.plist"));
        CHECK(sys.user_loaded.count("com.example.unrelated") == 1);
        // System domain without root: reported, never escalated.
        CHECK(has(r.needs_admin, "sudo launchctl bootout system/com.example.helper"));
        CHECK(sys.system_loaded.count("com.example.helper") == 1);
        CHECK(has(r.actions, "com.example.absent not loaded, no plist"));
        for (const auto& c : sys.calls)
            CHECK(c.find("system/") == std::string::npos);
    }

    TEST_CASE("pkgutil forgets matching ids and reports when admin is needed") {
        Tmp t;
        auto o = opts_for(t.path);
        FakeSystem sys;
        sys.pkgs = {"com.example.pkg.core", "com.example.pkg.extra", "com.other.pkg"};
        CaskUninstall u;
        u.pkgutil = {"com.example.pkg.*"};
        auto r = run_uninstall_stanza(u, StanzaPhase::PreRemove, sys, o);
        CHECK(has(r.actions, "pkgutil: forgot com.example.pkg.core"));
        CHECK(has(r.actions, "pkgutil: forgot com.example.pkg.extra"));
        CHECK_FALSE(has(sys.calls, "com.other.pkg"));

        FakeSystem sys2;
        sys2.pkgs = {"com.example.pkg.core"};
        sys2.forget_needs_admin = true;
        auto r2 = run_uninstall_stanza(u, StanzaPhase::PreRemove, sys2, o);
        CHECK(r2.needs_admin ==
              std::vector<std::string>{"sudo pkgutil --forget com.example.pkg.core"});
        CHECK_FALSE(has(r2.actions, "forgot"));
    }

    TEST_CASE("delete removes only the listed paths, with ~ expanded, never following symlinks") {
        Tmp t;
        auto o = opts_for(t.path);
        touch(o.home / "Library" / "Demo" / "data.db");
        touch(o.home / "Library" / "DemoSibling" / "keep.txt");
        touch(t.path / "outside" / "precious.txt");
        fs::create_directories(o.home / "Library" / "Links");
        fs::create_directory_symlink(t.path / "outside", o.home / "Library" / "Links" / "out");
        FakeSystem sys;
        CaskUninstall u;
        u.delete_paths = {"~/Library/Demo", "~/Library/Links/out", "~/Library/Missing"};
        auto r = run_uninstall_stanza(u, StanzaPhase::PostRemove, sys, o);
        CHECK_FALSE(fs::exists(o.home / "Library" / "Demo"));
        CHECK(fs::exists(o.home / "Library" / "DemoSibling" / "keep.txt"));
        CHECK_FALSE(fs::exists(fs::symlink_status(o.home / "Library" / "Links" / "out")));
        CHECK(fs::exists(t.path / "outside" / "precious.txt")); // symlink target untouched
        CHECK(has(r.actions, "Missing not present"));
        CHECK(r.warnings.empty());
    }

    TEST_CASE("trash moves listed paths into the Trash, renaming on collision") {
        Tmp t;
        auto o = opts_for(t.path);
        touch(o.home / "Library" / "Caches" / "Demo" / "c");
        touch(o.trash_dir / "Demo" / "older");
        FakeSystem sys;
        CaskUninstall u;
        u.trash = {"~/Library/Caches/Demo"};
        auto r = run_uninstall_stanza(u, StanzaPhase::PostRemove, sys, o);
        CHECK_FALSE(fs::exists(o.home / "Library" / "Caches" / "Demo"));
        CHECK(fs::exists(o.trash_dir / "Demo" / "older"));
        CHECK(has(r.actions, "trash: moved"));
        int demo_entries = 0;
        for (const auto& e : fs::directory_iterator(o.trash_dir))
            if (e.path().filename().string().starts_with("Demo"))
                ++demo_entries;
        CHECK(demo_entries == 2);
    }

    TEST_CASE("rmdir removes only empty directories") {
        Tmp t;
        auto o = opts_for(t.path);
        fs::create_directories(o.home / "Library" / "EmptyDir");
        touch(o.home / "Library" / "FullDir" / "f");
        FakeSystem sys;
        CaskUninstall u;
        u.rmdir = {"~/Library/EmptyDir", "~/Library/FullDir"};
        auto r = run_uninstall_stanza(u, StanzaPhase::PostRemove, sys, o);
        CHECK_FALSE(fs::exists(o.home / "Library" / "EmptyDir"));
        CHECK(fs::exists(o.home / "Library" / "FullDir" / "f"));
        CHECK(has(r.actions, "FullDir not empty; left"));
    }

    TEST_CASE("path safety refuses anything outside the listed, well-formed paths") {
        Tmp t;
        auto o = opts_for(t.path);
        std::string why;
        for (const char* bad : {"relative/path",
                                "~/../etc",
                                "/Users/../etc/passwd",
                                "~/Library/*",
                                "/tmp/x?",
                                "$HOME/Library/Demo",
                                "~root/x",
                                "/",
                                "/Applications",
                                "/Library",
                                "/Users",
                                "/System/Library/Foo",
                                "/usr/bin/foo",
                                "/bin/sh",
                                "/opt/homebrew/opt/gettext",
                                "/opt/homebrew",
                                "~",
                                "~/Library",
                                "~/Library/Application Support",
                                "~/Library/LaunchAgents",
                                "/Library/LaunchDaemons",
                                "~/.Trash",
                                ""}) {
            CHECK_MESSAGE(!resolve_stanza_path(bad, o, why), bad);
        }
        auto ok = resolve_stanza_path("~/Library/Application Support/Demo", o, why);
        REQUIRE(ok);
        CHECK(*ok == o.home / "Library" / "Application Support" / "Demo");
        CHECK(resolve_stanza_path("/Library/Application Support/Demo", o, why));
        // Sandboxed appdir: /Applications/... is rebased, never the real one.
        auto app = resolve_stanza_path("/Applications/Demo.app", o, why);
        REQUIRE(app);
        CHECK(*app == o.appdir / "Demo.app");
        StanzaOptions real = o;
        real.appdir = "/Applications";
        auto app2 = resolve_stanza_path("/Applications/Demo.app", real, why);
        REQUIRE(app2);
        CHECK(*app2 == fs::path("/Applications/Demo.app"));
    }

    TEST_CASE("refused paths are never touched, even when they exist") {
        Tmp t;
        auto o = opts_for(t.path);
        touch(o.home / "Library" / "keep.txt");
        touch(t.path / "victim.txt");
        FakeSystem sys;
        CaskUninstall u;
        u.delete_paths = {"~/Library", "~/../victim.txt", "~/Library/*"};
        u.trash = {"~"};
        u.rmdir = {"~/Library"};
        auto r = run_uninstall_stanza(u, StanzaPhase::PostRemove, sys, o);
        CHECK(fs::exists(o.home / "Library" / "keep.txt"));
        CHECK(fs::exists(t.path / "victim.txt"));
        CHECK(r.actions.empty());
        CHECK(r.warnings.size() == 5);
    }

    TEST_CASE("phases: Stop never runs destructive directives; unsupported ones warn") {
        Tmp t;
        auto o = opts_for(t.path);
        touch(o.home / "Library" / "Demo" / "d");
        FakeSystem sys;
        sys.pkgs = {"com.example.pkg"};
        CaskUninstall u;
        u.delete_paths = {"~/Library/Demo"};
        u.pkgutil = {"com.example.pkg"};
        u.unsupported = {"script", "kext"};
        auto r = run_uninstall_stanza(u, StanzaPhase::Stop, sys, o);
        CHECK(fs::exists(o.home / "Library" / "Demo" / "d"));
        CHECK(sys.calls.empty());
        CHECK(has(r.warnings, "unsupported uninstall directive(s) not run: script, kext"));
        auto pre = run_uninstall_stanza(u, StanzaPhase::PreRemove, sys, o);
        CHECK(fs::exists(o.home / "Library" / "Demo" / "d")); // delete is PostRemove
        CHECK(has(pre.actions, "pkgutil: forgot com.example.pkg"));
        CHECK(has(pre.warnings, "unsupported"));
    }

} // TEST_SUITE

#ifdef __APPLE__

namespace {

fs::path make_demo_zip(const fs::path& dir, const std::string& marker) {
    const fs::path staging = dir / "content";
    const fs::path app = staging / "Demo.app";
    touch(app / "Contents" / "Info.plist", "<?xml version=\"1.0\"?><plist></plist>\n");
    touch(app / "Contents" / "VERSION", marker + "\n");
    const fs::path zip_path = dir / "payload.zip";
    struct archive* a = archive_write_new();
    archive_write_set_format_zip(a);
    archive_write_open_filename(a, zip_path.c_str());
    for (auto it = fs::recursive_directory_iterator(staging);
         it != fs::recursive_directory_iterator(); ++it) {
        const auto rel = fs::relative(it->path(), staging).string();
        struct archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, rel.c_str());
        archive_entry_set_perm(e, 0755);
        if (it->is_directory()) {
            archive_entry_set_filetype(e, AE_IFDIR);
            archive_write_header(a, e);
        } else {
            archive_entry_set_filetype(e, AE_IFREG);
            archive_entry_set_size(e, static_cast<la_int64_t>(fs::file_size(it->path())));
            archive_write_header(a, e);
            std::ifstream in(it->path(), std::ios::binary);
            std::string data((std::istreambuf_iterator<char>(in)), {});
            archive_write_data(a, data.data(), data.size());
        }
        archive_entry_free(e);
    }
    archive_write_close(a);
    archive_write_free(a);
    return zip_path;
}

Package demo_cask(const std::string& version, const fs::path& zip) {
    Package p;
    p.name = "demo";
    p.version = version;
    p.artifact_type = ArtifactType::App;
    Package::Archive ar;
    ar.url = "file://" + zip.string();
    p.archives["default"] = ar;
    CaskArtifact app;
    app.kind = CaskArtifactKind::App;
    app.source = "Demo.app";
    p.cask_artifacts.push_back(app);
    p.cask_uninstall.quit = {"com.example.demo"};
    p.cask_uninstall.launchctl = {"com.example.demo.agent"};
    p.cask_uninstall.delete_paths = {"~/Library/Application Support/Demo"};
    p.cask_uninstall.unsupported = {"script"};
    return p;
}

std::string marker(const fs::path& app) {
    std::ifstream in(app / "Contents" / "VERSION");
    std::string s;
    std::getline(in, s);
    return s;
}

struct CaskEnv {
    Tmp t;
    Config cfg;
    std::string old_home;
    FakeSystem sys;
    CaskEnv() {
        cfg.den_home = t.path / "den";
        cfg.cache = cfg.den_home / "cache";
        cfg.store = t.path / "cellar";
        cfg.homebrew_prefix = t.path / "opt";
        cfg.homebrew_cellar = cfg.store;
        cfg.homebrew_caskroom = t.path / "hb-caskroom";
        cfg.appdir = t.path / "Applications";
        cfg.arch = Arch::Arm64;
        cfg.macos_version = MacOsVersion{15, 0};
        for (const auto& d : {cfg.den_home / "envs" / "ROOT" / "bin", cfg.cache, cfg.store,
                              cfg.appdir, t.path / "home"})
            fs::create_directories(d);
        touch(cfg.den_home / "active_env", "/\n");
        if (const char* h = std::getenv("HOME"))
            old_home = h;
        setenv("HOME", (t.path / "home").c_str(), 1);
        setenv("DEN_APPDIR", cfg.appdir.c_str(), 1);
        setenv("DEN_HOME", cfg.den_home.c_str(), 1);
        setenv("DEN_CASK_QUIT_TIMEOUT", "2", 1);
        set_cask_system(&sys);
    }
    ~CaskEnv() {
        set_cask_system(nullptr);
        setenv("HOME", old_home.c_str(), 1);
        unsetenv("DEN_APPDIR");
        unsetenv("DEN_HOME");
        unsetenv("DEN_CASK_QUIT_TIMEOUT");
    }
    fs::path home() const { return t.path / "home"; }
};

} // namespace

TEST_SUITE("cask::uninstall_integration") {

    TEST_CASE("receipt records the stanza, upgrade quits the running app before the swap") {
        CaskEnv env;
        install_cask(env.cfg, demo_cask("1.0", make_demo_zip(env.t.path / "v1", "v1")));
        auto rec = read_cask_receipt(cask_version_path(env.cfg, "demo", "1.0"));
        REQUIRE(rec);
        CHECK(rec->uninstall.quit == std::vector<std::string>{"com.example.demo"});
        CHECK(rec->uninstall.unsupported == std::vector<std::string>{"script"});

        touch(env.home() / "Library" / "Application Support" / "Demo" / "state");
        touch(env.home() / "Library" / "LaunchAgents" / "com.example.demo.agent.plist");
        env.sys.running = {"com.example.demo"};
        env.sys.user_loaded = {"com.example.demo.agent"};
        std::string seen_at_quit;
        env.sys.on_quit = [&](const std::string&) {
            seen_at_quit = marker(env.cfg.appdir / "Demo.app");
        };
        install_cask(env.cfg, demo_cask("2.0", make_demo_zip(env.t.path / "v2", "v2")));
        CHECK(seen_at_quit == "v1"); // quit happened before the swap
        CHECK(marker(env.cfg.appdir / "Demo.app") == "v2");
        CHECK(env.sys.running.empty());
        CHECK(env.sys.user_loaded.empty());
        // Upgrade unloads the agent but keeps its plist.
        CHECK(fs::exists(env.home() / "Library" / "LaunchAgents" / "com.example.demo.agent.plist"));
        // Destructive directives do not run on upgrade.
        CHECK(fs::exists(env.home() / "Library" / "Application Support" / "Demo" / "state"));
    }

    TEST_CASE("uninstall runs the stanza: quit, launchctl, delete; logs unsupported") {
        CaskEnv env;
        install_cask(env.cfg, demo_cask("1.0", make_demo_zip(env.t.path / "v1", "v1")));
        touch(env.home() / "Library" / "Application Support" / "Demo" / "state");
        touch(env.home() / "Library" / "Application Support" / "Other" / "keep");
        touch(env.home() / "Library" / "LaunchAgents" / "com.example.demo.agent.plist");
        env.sys.running = {"com.example.demo"};
        env.sys.user_loaded = {"com.example.demo.agent"};
        uninstall_cask(env.cfg, "demo");
        CHECK(env.sys.running.empty());
        CHECK(env.sys.user_loaded.empty());
        CHECK_FALSE(fs::exists(env.cfg.appdir / "Demo.app"));
        CHECK_FALSE(fs::exists(env.home() / "Library" / "Application Support" / "Demo"));
        CHECK(fs::exists(env.home() / "Library" / "Application Support" / "Other" / "keep"));
        CHECK_FALSE(
            fs::exists(env.home() / "Library" / "LaunchAgents" / "com.example.demo.agent.plist"));
        std::ifstream log(env.cfg.den_home / "cask.log");
        std::string all((std::istreambuf_iterator<char>(log)), {});
        CHECK(all.find("WARNING unsupported uninstall directive(s) not run: script") !=
              std::string::npos);
        CHECK(all.find("quit: com.example.demo quit") != std::string::npos);
    }

} // TEST_SUITE

#endif // __APPLE__

} // namespace test
} // namespace den

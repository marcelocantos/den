// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// An upgrade is transactional per package. When the new version cannot be
// linked into the prefix (paths there belong to something den does not
// manage), the package is left exactly as it was: the manifest still records
// the old version, the old version stays linked and recorded, <prefix>/opt
// still points at the old keg, the new keg den poured is removed from the
// Cellar, and the command fails with a message naming the paths. Covers
// `den upgrade` (named and all), the daemon's upgrade pass, `den install` of a
// newer version, and `den use`.

#include <doctest.h>

#include "activity/activity.h"
#include "cli/install.h"
#include "core/error.h"
#include "daemon/daemon.h"
#include "daemon/upgrade_pass.h"
#include "env/environment.h"
#include "env/manifest.h"
#include "env/version_change.h"
#include "index/package.h"
#include "provider/package_provider.h"
#include "store/link.h"
#include "store/store.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct RbTmp {
    fs::path path;
    RbTmp() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("den-upgrade-rollback-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~RbTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    RbTmp(const RbTmp&) = delete;
    RbTmp& operator=(const RbTmp&) = delete;
};

Config scratch_config(const fs::path& root) {
    Config c;
    c.den_home = root / "den";
    c.prefix = root / "prefix";
    c.homebrew_prefix = c.prefix;
    c.store = c.homebrew_prefix / "Cellar";
    c.homebrew_cellar = c.store;
    c.homebrew_caskroom = c.homebrew_prefix / "Caskroom";
    c.homebrew_taps = c.homebrew_prefix / "Library" / "Taps";
    c.cache = c.den_home / "cache";
    c.appdir = root / "Applications";
    c.arch = Arch::Arm64;
    fs::create_directories(c.den_home);
    fs::create_directories(c.store);
    return c;
}

void write_text(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << text;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string link_text(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_symlink(p, ec))
        return "<not a link>";
    return fs::read_symlink(p, ec).string();
}

// Pour a keg the way the homebrew provider does: files, den's receipt, and
// the shared opt link (moved along from an older den keg).
void pour(const Config& cfg, const std::string& name, const std::string& version,
          const std::vector<std::string>& files) {
    auto keg = cfg.store / name / version;
    for (const auto& f : files) {
        write_text(keg / f, name + " " + version + "\n");
    }
    mark_keg_owned(keg, name, version);
    ensure_prefix_opt_link(cfg.store, name, keg);
}

// A provider that pours like HomebrewProvider ("homebrew"), so the real
// homebrew layout, ownership and opt-link rules apply, without a network.
class PourProvider : public PackageProvider {
  public:
    std::map<std::string, std::vector<std::string>> files; // version -> files
    bool fail = false;
    int installs = 0;
    std::string_view provider_name() const override { return "homebrew"; }
    bool accepts(std::string_view) const override { return true; }
    InstallResult install(const Config& cfg, std::string_view name,
                          std::string_view version) override {
        ++installs;
        std::string v(version.empty() ? "2.0" : version);
        if (!is_installed(cfg, name, v))
            pour(cfg, std::string(name), v, files.at(v));
        if (fail)
            throw UserError("download failed: curl: (56) LibreSSL SSL_read: bad decrypt");
        return {v, {}};
    }
    void uninstall(const Config&, std::string_view) override {}
    std::vector<InstalledPackage> list_installed(const Config& cfg) const override {
        return ::den::list_installed(cfg.store);
    }
    fs::path package_root(const Config& cfg, std::string_view name,
                          std::string_view version) const override {
        return cfg.store / std::string(name) / std::string(version);
    }
    std::vector<fs::path> binary_paths(const Config&, const InstalledPackage& pkg) const override {
        std::vector<fs::path> out;
        for (const char* d : {"bin", "lib"})
            if (fs::is_directory(pkg.path / d))
                out.push_back(pkg.path / d);
        return out;
    }
};

PackageIndex index_tool(const std::string& version) {
    PackageIndex idx;
    Package p;
    p.name = "tool";
    p.version = version;
    idx.packages[p.name] = p;
    return idx;
}

// Everything `den list`, `den outdated` and the prefix say about "tool".
struct ToolState {
    std::string manifest;          // den list (the active env's record)
    std::vector<std::string> kegs; // den outdated reads the Cellar
    std::string opt, bin_tool, linked_version, ledger;
    bool operator==(const ToolState&) const = default;
};

ToolState tool_state(const Config& cfg) {
    ToolState s;
    auto r = resolve(cfg.den_home, active_env_path(cfg.den_home));
    s.manifest = r.count("tool") ? r.at("tool") : "";
    for (const auto& k : ::den::list_installed(cfg.store))
        if (k.name == "tool")
            s.kegs.push_back(k.version);
    std::sort(s.kegs.begin(), s.kegs.end());
    s.opt = link_text(cfg.homebrew_prefix / "opt" / "tool");
    s.bin_tool = link_text(cfg.homebrew_prefix / "bin" / "tool");
    auto state = cfg.den_home / "envs" / "ROOT" / ".den";
    s.linked_version = read_file(state / "prefix-linked" / "tool");
    s.ledger = read_file(state / "prefix-links" / "tool");
    return s;
}

std::string str(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v)
        out += s + " ";
    return out;
}

// tool 1.0 installed and linked by den; 2.0 in the index; 2.0 adds files
// whose prefix paths already belong to Homebrew (a link into a keg den did
// not pour) and to nobody (a real file).
struct Fixture {
    RbTmp t;
    Config cfg = scratch_config(t.path);
    PourProvider prov;
    PackageIndex idx = index_tool("2.0");
    ToolState before;

    explicit Fixture(bool conflict = true) {
        prov.files["1.0"] = {"bin/tool", "lib/libtool.1.dylib"};
        prov.files["2.0"] = {"bin/tool", "bin/tool-extra", "lib/libtool.2.dylib"};
        pour(cfg, "tool", "1.0", prov.files["1.0"]);
        with_manifest(cfg.den_home, "/",
                      [](Manifest& m) { m.packages["homebrew"]["tool"] = "1.0"; });
        materialise(cfg.den_home, cfg.store, "/", &idx);
        if (conflict) {
            // brew's keg, not den's, with a link into it.
            write_text(cfg.store / "other" / "9" / "bin" / "tool-extra", "brew\n");
            fs::create_symlink("../Cellar/other/9/bin/tool-extra",
                               cfg.homebrew_prefix / "bin" / "tool-extra");
            write_text(cfg.homebrew_prefix / "lib" / "libtool.2.dylib", "stray\n");
        }
        before = tool_state(cfg);
        REQUIRE(before.manifest == "1.0");
        REQUIRE(before.kegs == std::vector<std::string>{"1.0"});
        REQUIRE(before.opt == "../Cellar/tool/1.0");
        REQUIRE(before.bin_tool == "../Cellar/tool/1.0/bin/tool");
        REQUIRE(before.linked_version == "1.0\n");
    }

    void check_unchanged() {
        auto after = tool_state(cfg);
        CHECK(after.manifest == before.manifest);
        CHECK(str(after.kegs) == str(before.kegs));
        CHECK(after.opt == before.opt);
        CHECK(after.bin_tool == before.bin_tool);
        CHECK(after.linked_version == before.linked_version);
        CHECK(after.ledger == before.ledger);
        CHECK_FALSE(fs::exists(cfg.store / "tool" / "2.0"));
        // Homebrew's link and the stray file are untouched.
        CHECK(link_text(cfg.homebrew_prefix / "bin" / "tool-extra") ==
              "../Cellar/other/9/bin/tool-extra");
        CHECK(read_file(cfg.homebrew_prefix / "lib" / "libtool.2.dylib") == "stray\n");
        CHECK(read_activity(cfg.den_home).empty());
    }
};

std::string run_expect_error(const std::function<void()>& fn) {
    std::ostringstream sink;
    auto* old = std::cout.rdbuf(sink.rdbuf());
    std::string msg;
    try {
        fn();
    } catch (const UserError& e) {
        msg = e.what();
    }
    std::cout.rdbuf(old);
    return msg;
}

} // namespace

TEST_SUITE("upgrade_rollback") {

    TEST_CASE("den upgrade <name>: a conflicting link rolls the upgrade back") {
        Fixture f;
        auto msg = run_expect_error([&] { upgrade_packages(f.cfg, f.prov, f.idx, {"tool"}); });
        CHECK(f.prov.installs == 1);
        REQUIRE_FALSE(msg.empty()); // non-zero exit
        CHECK(msg.find("upgrade of tool 1.0 -> 2.0 was rolled back") != std::string::npos);
        CHECK(msg.find("2 path(s)") != std::string::npos);
        CHECK(msg.find("bin/tool-extra -> ../Cellar/other/9/bin/tool-extra") != std::string::npos);
        CHECK(msg.find("libtool.2.dylib (file)") != std::string::npos);
        CHECK(msg.find("tool stays at 1.0") != std::string::npos);
        CHECK(msg.find("2.0 keg den poured was removed") != std::string::npos);
        f.check_unchanged();
    }

    TEST_CASE("den upgrade (all): same rollback, and a rerun does the same") {
        Fixture f;
        for (int i = 0; i < 2; ++i) {
            auto msg = run_expect_error([&] { upgrade_packages(f.cfg, f.prov, f.idx, {}); });
            CHECK(msg.find("was rolled back") != std::string::npos);
            f.check_unchanged();
        }
    }

    TEST_CASE("a keg that was already in the Cellar is never removed by a rollback") {
        Fixture f;
        // Homebrew already has tool 2.0 (no den receipt).
        for (const auto& file : f.prov.files["2.0"])
            write_text(f.cfg.store / "tool" / "2.0" / file, "brew\n");
        auto msg = run_expect_error([&] { upgrade_packages(f.cfg, f.prov, f.idx, {"tool"}); });
        CHECK(msg.find("was rolled back") != std::string::npos);
        CHECK(msg.find("keg den poured was removed") == std::string::npos);
        CHECK(fs::exists(f.cfg.store / "tool" / "2.0" / "bin" / "tool"));
        auto after = tool_state(f.cfg);
        CHECK(after.manifest == "1.0");
        CHECK(after.opt == f.before.opt);
        CHECK(after.bin_tool == f.before.bin_tool);
        CHECK(after.linked_version == f.before.linked_version);
    }

    TEST_CASE("daemon upgrade pass: rolled back, kept pending, error recorded, no activity") {
        Fixture f;
        DaemonState state;
        state.pending.push_back({"tool", "1.0", "2.0"});
        auto res =
            run_upgrade_pass(f.cfg.store, f.cfg.den_home, state, [&](const PendingUpgrade& pu) {
                apply_daemon_upgrade(f.cfg, f.prov, f.idx, pu);
            });
        CHECK(res.applied.empty());
        REQUIRE(state.pending.size() == 1);
        CHECK(state.pending[0].name == "tool");
        REQUIRE(state.errors.size() == 1);
        CHECK(state.errors[0].find("auto-upgrade of tool 1.0 -> 2.0 was rolled back") !=
              std::string::npos);
        f.check_unchanged();
    }

    TEST_CASE("den install of a newer version of an installed package rolls back too") {
        Fixture f;
        auto msg = run_expect_error([&] { install_packages(f.cfg, f.prov, f.idx, {"tool"}); });
        CHECK(msg.find("install of tool 1.0 -> 2.0 was rolled back") != std::string::npos);
        f.check_unchanged();
    }

    TEST_CASE("den use: switching to a version that cannot be linked keeps the old one") {
        Fixture f;
        for (const auto& file : f.prov.files["2.0"])
            write_text(f.cfg.store / "tool" / "2.0" / file, "x\n");
        mark_keg_owned(f.cfg.store / "tool" / "2.0", "tool", "2.0");
        VersionChangeOptions o;
        o.install = false;
        auto res = apply_version_changes(f.cfg, f.prov, nullptr, {{"tool", "1.0", "2.0"}}, o);
        REQUIRE(res.rolled_back.size() == 1);
        CHECK(res.applied.empty());
        CHECK_FALSE(res.rolled_back[0].keg_removed);
        CHECK(fs::exists(f.cfg.store / "tool" / "2.0")); // use never removes kegs
        auto after = tool_state(f.cfg);
        CHECK(after.manifest == "1.0");
        CHECK(after.bin_tool == f.before.bin_tool);
        CHECK(after.linked_version == f.before.linked_version);
    }

    TEST_CASE("an install that fails undoes its pour and records nothing") {
        Fixture f(/*conflict=*/false);
        f.prov.fail = true;
        CHECK_THROWS_AS(upgrade_packages(f.cfg, f.prov, f.idx, {"tool"}), UserError);
        auto after = tool_state(f.cfg);
        CHECK(after == f.before);
        CHECK_FALSE(fs::exists(f.cfg.store / "tool" / "2.0"));
    }

    TEST_CASE("without conflicts the upgrade applies") {
        Fixture f(/*conflict=*/false);
        std::ostringstream sink;
        auto* old = std::cout.rdbuf(sink.rdbuf());
        upgrade_packages(f.cfg, f.prov, f.idx, {"tool"});
        std::cout.rdbuf(old);
        auto after = tool_state(f.cfg);
        CHECK(after.manifest == "2.0");
        CHECK(after.opt == "../Cellar/tool/2.0");
        CHECK(after.bin_tool == "../Cellar/tool/2.0/bin/tool");
        CHECK(after.linked_version == "2.0\n");
        CHECK(link_text(f.cfg.homebrew_prefix / "bin" / "tool-extra") ==
              "../Cellar/tool/2.0/bin/tool-extra");
        CHECK_FALSE(fs::exists(f.cfg.homebrew_prefix / "lib" / "libtool.1.dylib"));
        auto act = read_activity(f.cfg.den_home);
        REQUIRE(act.size() == 1);
        CHECK(sink.str().find("1 package(s) upgraded") != std::string::npos);
    }
}

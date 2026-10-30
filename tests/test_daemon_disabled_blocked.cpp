// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// The daemon's auto-upgrade never pours a package the index marks disabled:
// not the package itself, a dependency the upgrade would pour, or a
// revision-bumped dependent that is disabled or would pour one. It uses the
// same check `den install` / `den upgrade` refuse with (disabled_pours,
// #177/#181) with no --force. A blocked upgrade pours nothing (a refused
// dependent holds the whole batch), is logged, and is recorded in
// daemon_state.json's `disabled_blocked` (package, disabled package, reason)
// until it applies or is no longer pending; `den status`, `den outdated`,
// `den doctor` and `den daemon status` show it.

#include <doctest.h>

#include "cli/cli.h"
#include "cli/daemon_status.h"
#include "daemon/daemon.h"
#include "daemon/upgrade_pass.h"
#include "doctor/doctor.h"
#include "env/disabled.h"
#include "env/environment.h"
#include "env/manifest.h"
#include "index/index.h"
#include "index/package.h"
#include "provider/stub_provider.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct DbTmp {
    fs::path path;
    DbTmp() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("den-daemon-disabled-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~DbTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    DbTmp(const DbTmp&) = delete;
    DbTmp& operator=(const DbTmp&) = delete;
};

Config scratch_config(const fs::path& root) {
    Config c;
    c.den_home = root / "den";
    c.homebrew_prefix = root / "prefix";
    c.prefix = c.homebrew_prefix;
    c.store = c.homebrew_prefix / "Cellar";
    c.homebrew_cellar = c.store;
    c.homebrew_caskroom = c.homebrew_prefix / "Caskroom";
    c.homebrew_taps = c.homebrew_prefix / "Library" / "Taps";
    c.cache = c.den_home / "cache";
    c.appdir = root / "Applications";
    c.arch = Arch::Arm64;
    fs::create_directories(c.den_home);
    fs::create_directories(c.store);
    fs::create_directories(c.cache / "archives");
    return c;
}

class RecordingProvider : public StubProvider {
  public:
    std::string_view provider_name() const override { return "homebrew"; }
    InstallResult install(const Config& config, std::string_view name,
                          std::string_view version_hint) override {
        installs.push_back(std::string(name) + "@" + std::string(version_hint));
        return StubProvider::install(config, name, version_hint);
    }
    std::vector<std::string> installs;
};

Package pkg(const std::string& name, const std::string& version,
            std::vector<std::string> deps = {}) {
    Package p;
    p.name = name;
    p.version = version;
    p.dependencies = std::move(deps);
    return p;
}

Package disabled_formula(const std::string& name, const std::string& version) {
    auto p = pkg(name, version);
    p.disabled = true;
    p.disable_date = "2026-08-18";
    p.disable_reason = "unmaintained";
    p.disable_replacement = "libnew";
    return p;
}

bool has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

const std::string kTail =
    "disabled upstream: it is not maintained upstream (disabled on 2026-08-18; "
    "replacement: libnew)";
const std::string kRye = "'rye' is " + kTail;
const std::string kJqNeeds = "'jq' needs 'libold', which is " + kTail;

// The active environment's homebrew bucket starts as jq 1.7.1 plus `extra`;
// the index has jq 1.8.2 and libold 1.0, disabled (replacement libnew).
// cycle() runs one maintenance cycle with auto-upgrade on and the daemon's
// real apply (apply_daemon_upgrade) against the stub provider.
struct Fixture {
    DbTmp t;
    Config cfg = scratch_config(t.path);
    RecordingProvider brew;
    PackageIndex idx;
    DaemonState state;
    MaintenanceSettings settings{/*auto_download=*/false, /*auto_upgrade=*/true, ""};

    explicit Fixture(std::vector<std::pair<std::string, std::string>> extra = {}) {
        std::vector<std::pair<std::string, std::string>> installed{{"jq", "1.7.1"}};
        installed.insert(installed.end(), extra.begin(), extra.end());
        Manifest m;
        for (const auto& [name, version] : installed) {
            brew.install(cfg, name, version);
            m.packages["homebrew"][name] = version;
        }
        write_manifest(cfg.den_home, active_env_path(cfg.den_home), m);
        brew.installs.clear();
        add(pkg("jq", "1.8.2"));
        add(disabled_formula("libold", "1.0"));
    }
    void add(Package p) { idx.packages[p.name] = std::move(p); }

    MaintenanceReport cycle() {
        MaintenanceHooks h;
        h.refresh_index = [this] { return idx; };
        h.installed = [this] { return resolve(cfg.den_home, active_env_path(cfg.den_home)); };
        h.apply = [this](const PendingUpgrade& pu) { apply_daemon_upgrade(cfg, brew, idx, pu); };
        return run_maintenance_cycle(cfg.store, cfg.den_home, settings, state, h);
    }
    std::string manifest(const std::string& name) const {
        auto r = resolve(cfg.den_home, active_env_path(cfg.den_home));
        auto it = r.find(name);
        return it == r.end() ? "" : it->second;
    }
    bool pending(const std::string& name) const {
        for (const auto& p : state.pending)
            if (p.name == name)
                return true;
        return false;
    }
    std::string log() const {
        std::ifstream f(cfg.den_home / "daemon.log");
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
};

void check_block(const DisabledBlocked& b, const std::string& name, const std::string& installed,
                 const std::string& available, const std::string& disabled,
                 const std::string& reason) {
    CHECK(b.name == name);
    CHECK(b.installed == installed);
    CHECK(b.available == available);
    CHECK(b.disabled == disabled);
    CHECK(b.reason == reason);
}

} // namespace

TEST_SUITE("daemon_disabled_blocked") {

    TEST_CASE("the shared check: the package itself first, then the dependencies it would pour") {
        Fixture f;
        f.add(pkg("tree", "2.2.1", {"libold"}));
        auto rye = disabled_formula("rye", "0.44.0");
        rye.dependencies = {"libold"};
        f.add(rye);
        CHECK(disabled_pours(f.cfg, f.brew, f.idx, "jq", true).empty());
        auto tree = disabled_pours(f.cfg, f.brew, f.idx, "tree", true);
        REQUIRE(tree.size() == 1);
        CHECK(tree[0].pkg->name == "libold");
        CHECK(tree[0].message == "'tree' needs 'libold', which is " + kTail);
        auto both = disabled_pours(f.cfg, f.brew, f.idx, "rye", true);
        REQUIRE(both.size() == 2);
        CHECK(both[0].message == "'rye' is " + kTail);
        CHECK(both[1].message == "'rye' needs 'libold', which is " + kTail);
        CHECK(disabled_pours(f.cfg, f.brew, f.idx, "rye", false).size() == 1);
        // A disabled dependency already installed at the index version is
        // satisfied, as in the CLI.
        f.brew.install(f.cfg, "libold", "1.0");
        CHECK(disabled_pours(f.cfg, f.brew, f.idx, "tree", true).empty());
    }

    TEST_CASE("a package itself disabled is not upgraded, is recorded, and clears once enabled") {
        Fixture f({{"rye", "0.43.0"}});
        auto rye = disabled_formula("rye", "0.44.0");
        f.add(rye);
        auto r = f.cycle();
        CHECK(f.brew.installs == std::vector<std::string>{"jq@1.8.2"});
        CHECK(f.manifest("rye") == "0.43.0");
        CHECK_FALSE(f.pending("rye"));
        CHECK(r.disabled_blocked == 1);
        REQUIRE(f.state.disabled_blocked.size() == 1);
        check_block(f.state.disabled_blocked[0], "rye", "0.43.0", "0.44.0", "rye", kRye);
        CHECK(f.state.errors.empty());
        CHECK(has(f.log(), "not upgrading rye 0.43.0 -> 0.44.0: " + kRye));

        // Still disabled next cycle: still recorded, once.
        f.cycle();
        CHECK(f.state.disabled_blocked.size() == 1);

        // Upstream lifts the disable: it is upgraded and the record clears.
        rye.disabled = false;
        f.add(rye);
        f.cycle();
        CHECK(f.brew.installs == std::vector<std::string>{"jq@1.8.2", "rye@0.44.0"});
        CHECK(f.manifest("rye") == "0.44.0");
        CHECK(f.state.disabled_blocked.empty());
    }

    TEST_CASE("an upgrade that would pour a disabled dependency pours nothing and stays pending") {
        Fixture f;
        f.add(pkg("jq", "1.8.2", {"libold"}));
        auto r = f.cycle();
        CHECK(f.brew.installs.empty());
        CHECK(f.manifest("jq") == "1.7.1");
        CHECK(f.pending("jq"));
        CHECK(r.applied == 0);
        CHECK(r.disabled_blocked == 1);
        CHECK(f.state.errors.empty());
        REQUIRE(f.state.disabled_blocked.size() == 1);
        check_block(f.state.disabled_blocked[0], "jq", "1.7.1", "1.8.2", "libold", kJqNeeds);
        CHECK(has(f.log(), "not upgrading jq 1.7.1 -> 1.8.2: " + kJqNeeds));

        SUBCASE("cleared when the upgrade applies") {
            f.add(pkg("jq", "1.8.2"));
            f.cycle();
            CHECK(f.brew.installs == std::vector<std::string>{"jq@1.8.2"});
            CHECK(f.state.disabled_blocked.empty());
        }
        SUBCASE("held while pending at the same version, dropped when that version moves on") {
            f.settings.auto_upgrade = false; // no pass re-checks it
            f.cycle();
            REQUIRE(f.state.disabled_blocked.size() == 1);
            CHECK(f.state.disabled_blocked[0].disabled == "libold");
            f.add(pkg("jq", "1.8.3", {"libold"}));
            f.cycle();
            CHECK(f.state.disabled_blocked.empty());
            CHECK(f.brew.installs.empty());
        }
        SUBCASE("dropped when no longer pending") {
            f.add(pkg("jq", "1.7.1"));
            f.cycle();
            CHECK(f.state.disabled_blocked.empty());
            CHECK(f.brew.installs.empty());
        }
    }

    TEST_CASE("a disabled revision-bumped dependent holds the whole batch") {
        Fixture f({{"jqx", "1.0"}});
        auto jqx = disabled_formula("jqx", "1.0_1");
        jqx.revision = 1;
        jqx.dependencies = {"jq"};
        f.add(jqx);
        f.cycle();
        // The CLI would move jq and leave jqx; unattended, the daemon does
        // not split the chain, and never pours jqx.
        CHECK(f.brew.installs.empty());
        CHECK(f.manifest("jq") == "1.7.1");
        CHECK(f.manifest("jqx") == "1.0");
        CHECK(f.pending("jq"));
        CHECK(f.state.errors.empty());
        REQUIRE(f.state.disabled_blocked.size() == 2);
        check_block(f.state.disabled_blocked[0], "jq", "1.7.1", "1.8.2", "jqx",
                    "dependent jqx 1.0 (1.0_1 available): 'jqx' is " + kTail);
        check_block(f.state.disabled_blocked[1], "jqx", "1.0", "1.0_1", "jqx", "'jqx' is " + kTail);
        CHECK(has(f.log(), "not upgrading jq 1.7.1 -> 1.8.2: dependent jqx 1.0"));

        // Once jqx is no longer disabled the batch moves together and both
        // records clear.
        jqx.disabled = false;
        f.add(jqx);
        f.cycle();
        CHECK(f.manifest("jq") == "1.8.2");
        CHECK(f.manifest("jqx") == "1.0_1");
        CHECK(f.state.disabled_blocked.empty());
    }

    TEST_CASE("a revision-bumped dependent that would pour a disabled dependency holds the batch") {
        Fixture f({{"jqx", "1.0"}});
        auto jqx = pkg("jqx", "1.0_1", {"jq", "libold"});
        jqx.revision = 1;
        f.add(jqx);
        f.cycle();
        CHECK(f.brew.installs.empty());
        CHECK(f.manifest("jq") == "1.7.1");
        CHECK(f.manifest("jqx") == "1.0");
        REQUIRE(f.state.disabled_blocked.size() == 2);
        check_block(f.state.disabled_blocked[0], "jq", "1.7.1", "1.8.2", "libold",
                    "dependent jqx 1.0 (1.0_1 available): 'jqx' needs 'libold', which is " + kTail);
        // jqx's own pending upgrade is refused by the same check.
        check_block(f.state.disabled_blocked[1], "jqx", "1.0", "1.0_1", "libold",
                    "'jqx' needs 'libold', which is " + kTail);
    }

    TEST_CASE("apply_daemon_upgrade throws UpgradeDisabled and has no bypass") {
        Fixture f;
        f.add(pkg("jq", "1.8.2", {"libold"}));
        try {
            apply_daemon_upgrade(f.cfg, f.brew, f.idx, {"jq", "1.7.1", "1.8.2"});
            FAIL("expected UpgradeDisabled");
        } catch (const UpgradeDisabled& e) {
            CHECK(e.disabled == "libold");
            CHECK(std::string(e.what()) == kJqNeeds);
        }
        CHECK(f.brew.installs.empty());
    }

    TEST_CASE("disabled_blocked round-trips through daemon_state.json") {
        DbTmp t;
        DaemonState s;
        s.disabled_blocked.push_back({"jq", "1.7.1", "1.8.2", "libold", kJqNeeds});
        write_daemon_state(t.path, s);
        auto back = read_daemon_state(t.path);
        REQUIRE(back.disabled_blocked.size() == 1);
        check_block(back.disabled_blocked[0], "jq", "1.7.1", "1.8.2", "libold", kJqNeeds);
        std::ifstream f(t.path / "daemon_state.json");
        std::stringstream ss;
        ss << f.rdbuf();
        CHECK(has(ss.str(), "\"disabled_blocked\""));
        CHECK(read_daemon_state(t.path / "missing").disabled_blocked.empty());
    }
}

namespace {

class EnvGuard {
  public:
    EnvGuard(const char* name, const std::string& value) : name_(name) {
        if (const char* old = std::getenv(name))
            old_ = old;
        ::setenv(name, value.c_str(), 1);
    }
    ~EnvGuard() {
        if (old_)
            ::setenv(name_, old_->c_str(), 1);
        else
            ::unsetenv(name_);
    }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

  private:
    const char* name_;
    std::optional<std::string> old_;
};

class StreamCapture {
  public:
    StreamCapture() : out_(std::cout.rdbuf(buf_.rdbuf())), err_(std::cerr.rdbuf(buf_.rdbuf())) {}
    ~StreamCapture() {
        std::cout.rdbuf(out_);
        std::cerr.rdbuf(err_);
    }
    StreamCapture(const StreamCapture&) = delete;
    StreamCapture& operator=(const StreamCapture&) = delete;
    std::string str() const { return buf_.str(); }

  private:
    std::ostringstream buf_;
    std::streambuf* out_;
    std::streambuf* err_;
};

// A scratch den home whose daemon state records two blocked upgrades; the
// commands run in-process through Cli::run.
struct Home {
    DbTmp tmp;
    fs::path prefix = tmp.path / "prefix";
    EnvGuard home{"HOME", tmp.path.string()};
    EnvGuard den_home{"DEN_HOME", (tmp.path / "den").string()};
    EnvGuard den_prefix{"DEN_PREFIX", prefix.string()};
    EnvGuard brew_prefix{"HOMEBREW_PREFIX", prefix.string()};
    EnvGuard brew_cellar{"HOMEBREW_CELLAR", (prefix / "Cellar").string()};

    explicit Home(bool blocked = true) {
        const fs::path dh = tmp.path / "den";
        fs::create_directories(prefix / "Cellar");
        Manifest m;
        m.packages["homebrew"]["jq"] = "1.7.1";
        m.packages["homebrew"]["rye"] = "0.43.0";
        write_manifest(dh, active_env_path(dh), m);
        PackageIndex idx;
        idx.packages["jq"] = pkg("jq", "1.8.2", {"libold"});
        idx.packages["libold"] = disabled_formula("libold", "1.0");
        auto rye = disabled_formula("rye", "0.44.0");
        idx.packages["rye"] = rye;
        fs::create_directories(dh / "cache");
        save_index(idx, dh / "cache" / "index.json");
        DaemonState s;
        s.pending.push_back({"jq", "1.7.1", "1.8.2"});
        if (blocked) {
            s.disabled_blocked.push_back({"jq", "1.7.1", "1.8.2", "libold", kJqNeeds});
            s.disabled_blocked.push_back({"rye", "0.43.0", "0.44.0", "rye", kRye});
        }
        write_daemon_state(dh, s);
    }

    std::string run(std::vector<std::string> args) const {
        args.insert(args.begin(), "den");
        std::vector<char*> argv;
        for (auto& a : args)
            argv.push_back(a.data());
        argv.push_back(nullptr);
        StreamCapture cap;
        den::Cli cli;
        (void)cli.run(static_cast<int>(args.size()), argv.data());
        return cap.str();
    }
};

const std::string kSection = "Not upgrading (would pour a package disabled upstream):\n"
                             "  jq 1.7.1 -> 1.8.2: " +
                             kJqNeeds + "\n  rye 0.43.0 -> 0.44.0: " + kRye + "\n";

} // namespace

TEST_SUITE("daemon_disabled_blocked") {

    TEST_CASE("den status, den outdated and den daemon status show blocked upgrades") {
        Home h;
        auto status = h.run({"status"});
        CAPTURE(status);
        CHECK(has(status, "Pending upgrades: 1\n  jq 1.7.1 -> 1.8.2\n" + kSection));

        auto outdated = h.run({"outdated"});
        CAPTURE(outdated);
        CHECK(
            has(outdated, "jq  1.7.1 -> 1.8.2  [daemon: would pour libold, disabled upstream]\n"));
        CHECK(has(outdated, kSection));
        CHECK_FALSE(has(outdated, "up to date"));

        auto dstatus = h.run({"daemon", "status"});
        CAPTURE(dstatus);
        CHECK(has(dstatus, "Pending upgrades:\n  jq 1.7.1 -> 1.8.2\n" + kSection));
    }

    TEST_CASE("den doctor warns about blocked upgrades") {
        Home h;
        auto out = h.run({"doctor"});
        CAPTURE(out);
        CHECK(has(out, "[warning] 2 upgrade(s) not applied by the daemon: each would pour a "
                       "package disabled upstream, which den never does unattended:\n"
                       "    jq 1.7.1 -> 1.8.2: " +
                           kJqNeeds + "\n    rye 0.43.0 -> 0.44.0: " + kRye +
                           "\n  Switch to the replacement, or install it deliberately with "
                           "`den upgrade <name> --force` (formulae only).\n"));
    }

    TEST_CASE("nothing blocked: no section and no doctor warning") {
        Home h(/*blocked=*/false);
        CHECK_FALSE(has(h.run({"status"}), "Not upgrading"));
        CHECK_FALSE(has(h.run({"outdated"}), "Not upgrading"));
        CHECK_FALSE(has(h.run({"daemon", "status"}), "Not upgrading"));
        CHECK_FALSE(has(h.run({"doctor"}), "not applied by the daemon"));
        CHECK(format_disabled_blocked({}).empty());
        std::ostringstream os;
        format_disabled_blocked_section(os, DaemonState{});
        CHECK(os.str().empty());
    }
}

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// `den upgrade --dry-run` reports exactly what the real run would do, and
// exits the same way: it takes the real run's path (the unattended trust
// screen, the batch plan in apply_version_changes with plan_only, the same
// reports and the same UserError) and stops short of every side effect.
// Each scenario runs the dry run and the real run on identical fixtures and
// checks the dry output is the real output with only the action headings
// changed ("Would upgrade", "Would also upgrade") and the pour/link progress
// lines dropped, plus "Dry run: nothing changed.", with the same stderr and
// the same error; and that the dry run changed nothing.

#include <doctest.h>

#include "cli/install.h"
#include "core/error.h"
#include "env/environment.h"
#include "env/manifest.h"
#include "index/index.h"
#include "index/package.h"
#include "provider/stub_provider.h"
#include "settings/settings.h"
#include "store/pins.h"
#include "trust/degraded_installs.h"
#include "trust/unattended.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct DrTmp {
    fs::path path;
    DrTmp() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("den-dry-parity-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~DrTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    DrTmp(const DrTmp&) = delete;
    DrTmp& operator=(const DrTmp&) = delete;
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

Package disabled(Package p) {
    p.disabled = true;
    p.disable_date = "2026-08-18";
    p.disable_reason = "unmaintained";
    p.disable_replacement = "libnew";
    return p;
}

Package rev_bump(Package p) {
    p.revision = 1;
    return p;
}

Package bottled(Package p, char sha) {
    Package::Archive ar;
    ar.url = "https://bottles.invalid/" + p.name + ".tar.gz";
    ar.sha256 = std::string(64, sha);
    ar.relocation = RelocationType::Portable;
    p.archives["default"] = ar;
    return p;
}

bool has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

const std::string kTail =
    "disabled upstream: it is not maintained upstream (disabled on 2026-08-18; "
    "replacement: libnew)";

// Active environment: jq 1.7.1 plus `extra` (homebrew bucket). Index: jq
// 1.8.2 and libold 1.0, disabled; a scenario adds or replaces entries.
struct Fixture {
    DrTmp t;
    Config cfg = scratch_config(t.path);
    RecordingProvider brew;
    PackageIndex idx;

    explicit Fixture(const std::vector<std::pair<std::string, std::string>>& extra) {
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
        add(disabled(pkg("libold", "1.0")));
    }
    void add(Package p) { idx.packages[p.name] = std::move(p); }
    std::map<std::string, std::string> manifest() const {
        return resolve(cfg.den_home, active_env_path(cfg.den_home));
    }
    size_t ledger() const { return summarize_degraded_installs(cfg).refused_installs.size(); }
};

struct Run {
    std::string out;
    std::string err;
    std::string thrown; // the UserError `den` exits 1 with; empty: exit 0
};

struct Unattended {
    Unattended() { set_unattended("--unattended"); }
    ~Unattended() { set_unattended(std::nullopt); }
    Unattended(const Unattended&) = delete;
    Unattended& operator=(const Unattended&) = delete;
};

Run run_upgrade(Fixture& f, const std::vector<std::string>& names, bool dry_run, bool force,
                bool unattended) {
    std::optional<Unattended> u;
    if (unattended)
        u.emplace();
    Run r;
    std::ostringstream out, err;
    auto* old_out = std::cout.rdbuf(out.rdbuf());
    auto* old_err = std::cerr.rdbuf(err.rdbuf());
    try {
        upgrade_packages(f.cfg, f.brew, f.idx, names, dry_run, force);
    } catch (const UserError& e) {
        r.thrown = e.what();
    } catch (...) {
        std::cout.rdbuf(old_out);
        std::cerr.rdbuf(old_err);
        throw;
    }
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    r.out = out.str();
    r.err = err.str();
    return r;
}

// The real run's stdout as its dry run must print it.
std::string as_dry_run(const std::string& real) {
    static const std::regex progress(R"(^(==> .*|  \d+ symlinks)$)");
    static const std::regex heading(R"(^(\d+) package\(s\) to upgrade:$)");
    std::istringstream in(real);
    std::string out;
    for (std::string line; std::getline(in, line);) {
        if (std::regex_match(line, progress))
            continue;
        line = std::regex_replace(line, heading, "Would upgrade $1 package(s):");
        if (line.starts_with("Also upgraded "))
            line = "Would also upgrade " + line.substr(std::string("Also upgraded ").size());
        out += line + "\n";
    }
    return out + "Dry run: nothing changed.\n";
}

struct Scenario {
    std::vector<std::pair<std::string, std::string>> extra;
    std::function<void(Fixture&)> setup;
    std::vector<std::string> names;
    bool force = false;
    bool unattended = false;
};

// Runs `s` dry and for real on identical fixtures, checks parity and that
// the dry run changed nothing, and returns the dry run.
Run check_parity(const Scenario& s) {
    Fixture dry_f(s.extra), real_f(s.extra);
    if (s.setup) {
        s.setup(dry_f);
        s.setup(real_f);
    }
    const auto before = dry_f.manifest();
    auto dry = run_upgrade(dry_f, s.names, /*dry_run=*/true, s.force, s.unattended);
    auto real = run_upgrade(real_f, s.names, /*dry_run=*/false, s.force, s.unattended);
    CAPTURE(dry.out);
    CAPTURE(dry.err);
    CAPTURE(real.out);
    CAPTURE(real.err);
    CHECK(dry.out == as_dry_run(real.out));
    CHECK(dry.err == real.err);
    CHECK(dry.thrown == real.thrown);
    CHECK(dry_f.brew.installs.empty());
    CHECK(dry_f.manifest() == before);
    CHECK(dry_f.ledger() == 0);
    return dry;
}

} // namespace

TEST_SUITE("upgrade_dry_run_parity") {

    TEST_CASE("a revision-bumped dependent: listed as joining the batch, exit 0") {
        auto dry = check_parity({{{"jqx", "1.0"}},
                                 [](Fixture& f) { f.add(rev_bump(pkg("jqx", "1.0_1", {"jq"}))); },
                                 {"jq"}});
        CHECK(dry.out == "Would upgrade 1 package(s):\n  jq 1.7.1 -> 1.8.2\n"
                         "Would also upgrade 1 dependent(s) rebuilt for the new version:\n"
                         "  jqx 1.0 -> 1.0_1\n"
                         "Dry run: nothing changed.\n");
        CHECK(dry.thrown.empty());
    }

    TEST_CASE("a disabled revision-bumped dependent: refused list and exit 1") {
        auto dry = check_parity(
            {{{"jqx", "1.0"}},
             [](Fixture& f) { f.add(disabled(rev_bump(pkg("jqx", "1.0_1", {"jq"})))); },
             {"jq"}});
        CHECK(dry.out == "Would upgrade 1 package(s):\n  jq 1.7.1 -> 1.8.2\n"
                         "Not upgrading 1 dependent(s) (disabled upstream):\n"
                         "  jqx 1.0 (1.0_1 available)\n"
                         "Dry run: nothing changed.\n");
        CHECK(dry.err == "error: 'jqx' is " + kTail + "\n");
        CHECK(dry.thrown == "1 dependent(s) are disabled upstream or need a dependency that is");
    }

    TEST_CASE("a dependent that would pour a disabled dependency: refused, exit 1") {
        auto dry = check_parity(
            {{{"jqx", "1.0"}},
             [](Fixture& f) { f.add(rev_bump(pkg("jqx", "1.0_1", {"jq", "libold"}))); },
             {"jq"}});
        CHECK(has(dry.out, "Not upgrading 1 dependent(s) (disabled upstream):\n"
                           "  jqx 1.0 (1.0_1 available)\n"));
        CHECK(dry.err == "error: 'jqx' needs 'libold', which is " + kTail + "\n");
        CHECK(dry.thrown == "1 dependent(s) are disabled upstream or need a dependency that is");
    }

    TEST_CASE("--force lets a disabled dependent through with a warning, exit 0") {
        Scenario s{{{"jqx", "1.0"}},
                   [](Fixture& f) { f.add(disabled(rev_bump(pkg("jqx", "1.0_1", {"jq"})))); },
                   {"jq"}};
        s.force = true;
        auto dry = check_parity(s);
        CHECK(has(dry.out, "Would also upgrade 1 dependent(s) rebuilt for the new version:\n"
                           "  jqx 1.0 -> 1.0_1\n"));
        CHECK(dry.err == "warning: 'jqx' is " + kTail + "\n");
        CHECK(dry.thrown.empty());
    }

    TEST_CASE("a pinned dependent: listed and left, exit 0") {
        auto dry = check_parity({{{"jqx", "1.0"}},
                                 [](Fixture& f) {
                                     f.add(rev_bump(pkg("jqx", "1.0_1", {"jq"})));
                                     write_pins(f.cfg.den_home, {{"jqx", "1.0"}});
                                 },
                                 {"jq"}});
        CHECK(has(dry.out, "Not upgrading 1 pinned dependent(s) (they may need the old version):\n"
                           "  jqx 1.0 (pinned; 1.0_1 available)\n"));
        CHECK(dry.thrown.empty());
    }

    TEST_CASE("only refusals: no 'Nothing to upgrade', same error") {
        SUBCASE("a named pinned package") {
            auto dry = check_parity(
                {{}, [](Fixture& f) { write_pins(f.cfg.den_home, {{"jq", "1.7.1"}}); }, {"jq"}});
            CHECK(dry.out == "Dry run: nothing changed.\n");
            CHECK(dry.thrown == "1 requested package(s) are pinned");
        }
        SUBCASE("an upgrade that would pour a disabled dependency") {
            auto dry = check_parity(
                {{}, [](Fixture& f) { f.add(pkg("jq", "1.8.2", {"libold"})); }, {"jq"}});
            CHECK(dry.out == "Dry run: nothing changed.\n");
            CHECK(dry.err == "error: 'jq' needs 'libold', which is " + kTail + "\n");
            CHECK_FALSE(dry.thrown.empty());
        }
    }

    TEST_CASE("--unattended: the trust screen holds the same upgrades, nothing in the ledger") {
        // jq 1.8.2 is verified; probe 1.0 and the revision-bumped jqx 1.0_1
        // are single-source, so the refuse policy holds both back.
        auto setup = [](Fixture& f) {
            f.add(bottled(pkg("jq", "1.8.2"), 'c'));
            f.add(bottled(pkg("probe", "1.0"), 'a'));
            f.add(bottled(rev_bump(pkg("jqx", "1.0_1", {"jq"})), 'b'));
            fs::create_directories(f.cfg.den_home / "trust");
            std::ofstream(f.cfg.den_home / "trust" / "known_hashes.json")
                << R"({"schema_version": 1, "hashes": {"jq--1.8.2--default": ")" +
                       std::string(64, 'c') + R"("}})";
            set_setting(f.cfg.den_home, "trust.unattended", "refuse");
        };
        Scenario s{{{"jqx", "1.0"}, {"probe", "0.9"}}, setup, {"jq", "probe"}};
        s.unattended = true;
        auto dry = check_parity(s);
        CHECK(has(dry.out,
                  "Held back 1 package(s) (unattended trust policy):\n  probe 0.9 -> 1.0: "));
        CHECK(has(dry.out, "Would upgrade 1 package(s):\n  jq 1.7.1 -> 1.8.2\n"));
        CHECK(has(dry.out,
                  "Held back 1 dependent(s) (unattended trust policy):\n  jqx 1.0 -> 1.0_1: "));
        CHECK(dry.thrown == "2 package(s) held back by the unattended trust policy; review with "
                            "`den trust review`");
        // The real run records both holds; the dry run (checked above) none.
        Fixture real_f(s.extra);
        setup(real_f);
        (void)run_upgrade(real_f, s.names, /*dry_run=*/false, false, /*unattended=*/true);
        CHECK(real_f.ledger() == 2);
    }

    TEST_CASE("--unattended, bare: a held package that is also a held dependent is listed once") {
        // Regression for #185. With no names, jqx 1.0_1 (a revision bump, so
        // outdated in its own right) is screened and held as a candidate; when
        // jq's batch then reaches it as a dependent it is already held, so it
        // is not listed again under held dependents.
        auto setup = [](Fixture& f) {
            f.add(bottled(pkg("jq", "1.8.2"), 'c'));
            f.add(bottled(pkg("probe", "1.0"), 'a'));
            f.add(bottled(rev_bump(pkg("jqx", "1.0_1", {"jq"})), 'b'));
            fs::create_directories(f.cfg.den_home / "trust");
            std::ofstream(f.cfg.den_home / "trust" / "known_hashes.json")
                << R"({"schema_version": 1, "hashes": {"jq--1.8.2--default": ")" +
                       std::string(64, 'c') + R"("}})";
            set_setting(f.cfg.den_home, "trust.unattended", "refuse");
        };
        Scenario s{{{"jqx", "1.0"}, {"probe", "0.9"}}, setup, {}};
        s.unattended = true;
        auto dry = check_parity(s);
        const std::string held = "Held back 2 package(s) (unattended trust policy):\n"
                                 "  jqx 1.0 -> 1.0_1: single-source";
        CHECK(dry.out.starts_with(held));
        CHECK(has(dry.out, "\n  probe 0.9 -> 1.0: single-source"));
        CHECK(has(dry.out, "Would upgrade 1 package(s):\n  jq 1.7.1 -> 1.8.2\n"));
        CHECK_FALSE(has(dry.out, "dependent(s)"));
        size_t listed = 0;
        for (size_t at = 0; (at = dry.out.find("  jqx 1.0 -> 1.0_1", at)) != std::string::npos;
             ++at)
            ++listed;
        CHECK(listed == 1);
        CHECK(dry.thrown == "2 package(s) held back by the unattended trust policy; review with "
                            "`den trust review`");
        Fixture real_f(s.extra);
        setup(real_f);
        auto real = run_upgrade(real_f, s.names, /*dry_run=*/false, false, /*unattended=*/true);
        CHECK(real.thrown == dry.thrown);
        CHECK(real_f.ledger() == 2);
        CHECK(real_f.manifest().at("jq") == "1.8.2");
        CHECK(real_f.manifest().at("jqx") == "1.0");
    }

    TEST_CASE("plain cases keep parity: one upgrade, and nothing to do") {
        auto one = check_parity({});
        CHECK(one.out == "Would upgrade 1 package(s):\n  jq 1.7.1 -> 1.8.2\n"
                         "Dry run: nothing changed.\n");
        auto none = check_parity({{}, [](Fixture& f) { f.add(pkg("jq", "1.7.1")); }});
        CHECK(none.out == "All packages are up to date.\nDry run: nothing changed.\n");
    }
}

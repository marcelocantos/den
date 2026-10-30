// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// #178: as brew does, `den install` and `den upgrade` refuse a package whose
// install would pour a dependency the index marks disabled ("error: 'tree'
// needs 'libold', which is disabled upstream: ..."), and say nothing about a
// disabled dependency already installed at the index version. The other
// names proceed and the run exits 1. A revision-bumped dependent that is
// disabled stays put and fails the upgrade. `--force` lets a disabled
// formula through (named or a dependency) with the message as a warning;
// a disabled cask is refused regardless.

#include <doctest.h>

#include "build/source_plan.h"
#include "cli/install.h"
#include "core/error.h"
#include "env/environment.h"
#include "env/manifest.h"
#include "index/index.h"
#include "index/package.h"
#include "provider/stub_provider.h"
#include "store/store.h"

#include <algorithm>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct DdTmp {
    fs::path path;
    DdTmp() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("den-disabled-dep-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~DdTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    DdTmp(const DdTmp&) = delete;
    DdTmp& operator=(const DdTmp&) = delete;
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

struct Captured {
    std::string out;
    std::string err;
    std::string thrown; // UserError text, empty when nothing was thrown
};

template <typename Fn> Captured captured(Fn&& fn) {
    Captured c;
    std::ostringstream out, err;
    auto* old_out = std::cout.rdbuf(out.rdbuf());
    auto* old_err = std::cerr.rdbuf(err.rdbuf());
    try {
        fn();
    } catch (const UserError& e) {
        c.thrown = e.what();
    } catch (...) {
        std::cout.rdbuf(old_out);
        std::cerr.rdbuf(old_err);
        throw;
    }
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    c.out = out.str();
    c.err = err.str();
    return c;
}

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

const std::string kNeeds = "'tree' needs 'libold', which is disabled upstream: it is not "
                           "maintained upstream (disabled on 2026-08-18; replacement: libnew)";
const std::string kJqNeeds = "'jq' needs 'libold', which is disabled upstream: it is not "
                             "maintained upstream (disabled on 2026-08-18; replacement: libnew)";
const std::string kNeedsCount =
    "1 requested package(s) need a dependency that is disabled upstream";
const std::string kRye = "'rye' is disabled upstream: it is not maintained upstream (disabled on "
                         "2026-08-18; replacement: uv)";
const std::string kApp = "'oldapp' is disabled upstream: it is discontinued upstream (disabled "
                         "on 2026-09-01; replacement: newapp)";

// homebrew bucket: jq 1.7.1 and rye 0.43.0, plus whatever `extra` adds.
// Index: jq 1.8.2; tree 2.2.1 needing libold 1.0, disabled as unmaintained
// (replacement libnew); rye 0.44.0, disabled (replacement uv); oldapp 2.0,
// a disabled cask (replacement newapp); wrapper 1.0, a formula needing it.
struct Fixture {
    DdTmp t;
    Config cfg = scratch_config(t.path);
    RecordingProvider brew;
    PackageIndex idx;

    explicit Fixture(std::vector<std::pair<std::string, std::string>> extra = {}) {
        std::vector<std::pair<std::string, std::string>> installed{{"jq", "1.7.1"},
                                                                   {"rye", "0.43.0"}};
        installed.insert(installed.end(), extra.begin(), extra.end());
        Manifest m;
        for (const auto& [name, version] : installed) {
            brew.install(cfg, name, version);
            m.packages["homebrew"][name] = version;
        }
        write_manifest(cfg.den_home, active_env_path(cfg.den_home), m);
        brew.installs.clear();
        add(pkg("jq", "1.8.2"));
        add(pkg("tree", "2.2.1", {"libold"}));
        add(disabled_formula("libold", "1.0"));
        auto rye = disabled_formula("rye", "0.44.0");
        rye.disable_replacement = "uv";
        add(rye);
        auto app = pkg("oldapp", "2.0");
        app.artifact_type = ArtifactType::App;
        app.disabled = true;
        app.disable_date = "2026-09-01";
        app.disable_reason = "discontinued";
        app.disable_replacement = "newapp";
        add(app);
        add(pkg("wrapper", "1.0", {"oldapp"}));
    }
    void add(Package p) { idx.packages[p.name] = std::move(p); }
};

} // namespace

TEST_SUITE("disabled_dependency_refusal") {

    TEST_CASE("den install refuses a package that would pour a disabled dependency") {
        Fixture f;
        auto r = captured([&] { install_packages(f.cfg, f.brew, f.idx, {"tree"}); });
        CAPTURE(r.out);
        CAPTURE(r.err);
        CHECK(f.brew.installs.empty());
        CHECK(has(r.err, "error: " + kNeeds + "\n"));
        CHECK(r.thrown == kNeedsCount);
    }

    TEST_CASE("den install installs the other names, then fails") {
        Fixture f;
        auto r = captured([&] { install_packages(f.cfg, f.brew, f.idx, {"tree", "jq"}); });
        CAPTURE(r.err);
        CHECK(f.brew.installs == std::vector<std::string>{"jq@"});
        CHECK(has(r.err, "error: " + kNeeds));
        CHECK(r.thrown == kNeedsCount);
    }

    TEST_CASE("a disabled dependency deeper in the closure is refused too") {
        Fixture f;
        f.idx.packages["tree"].dependencies = {"mid"};
        f.add(pkg("mid", "1.0", {"libold"}));
        auto r = captured([&] { install_packages(f.cfg, f.brew, f.idx, {"tree"}); });
        CHECK(f.brew.installs.empty());
        CHECK(has(r.err, "error: " + kNeeds));
        CHECK(r.thrown == kNeedsCount);
    }

    TEST_CASE("a disabled dependency already installed at the index version is not mentioned") {
        Fixture f({{"libold", "1.0"}});
        auto r = captured([&] { install_packages(f.cfg, f.brew, f.idx, {"tree"}); });
        CAPTURE(r.err);
        CHECK(f.brew.installs == std::vector<std::string>{"tree@"});
        CHECK(r.thrown.empty());
        CHECK_FALSE(has(r.err, "libold"));
    }

    TEST_CASE("a disabled dependency installed at another version would be upgraded: refused") {
        Fixture f({{"libold", "0.9"}});
        auto r = captured([&] { install_packages(f.cfg, f.brew, f.idx, {"tree"}); });
        CHECK(f.brew.installs.empty());
        CHECK(has(r.err, "error: " + kNeeds));
        CHECK(r.thrown == kNeedsCount);
    }

    TEST_CASE("den install --force installs past a disabled formula dependency, with a warning") {
        Fixture f;
        auto r =
            captured([&] { install_packages(f.cfg, f.brew, f.idx, {"tree"}, {}, /*force=*/true); });
        CAPTURE(r.err);
        CHECK(f.brew.installs == std::vector<std::string>{"tree@"});
        CHECK(r.thrown.empty());
        CHECK(has(r.err, "warning: " + kNeeds + "\n"));
        CHECK_FALSE(has(r.err, "error:"));
    }

    TEST_CASE("den install --force installs a named disabled formula, with a warning") {
        Fixture f;
        auto r =
            captured([&] { install_packages(f.cfg, f.brew, f.idx, {"rye"}, {}, /*force=*/true); });
        CAPTURE(r.err);
        CHECK(f.brew.installs == std::vector<std::string>{"rye@"});
        CHECK(r.thrown.empty());
        CHECK(has(r.err, "warning: " + kRye + "\n"));
        CHECK_FALSE(has(r.err, "error:"));
    }

    TEST_CASE("--force does not bypass a disabled cask, named or as a dependency") {
        Fixture f;
        auto named = captured(
            [&] { install_packages(f.cfg, f.brew, f.idx, {"oldapp"}, {}, /*force=*/true); });
        CHECK(has(named.err, "error: " + kApp));
        CHECK(named.thrown == "1 requested package(s) are disabled upstream");
        auto dep = captured(
            [&] { install_packages(f.cfg, f.brew, f.idx, {"wrapper"}, {}, /*force=*/true); });
        CHECK(has(dep.err, "error: 'wrapper' needs 'oldapp', which is disabled upstream: it is "
                           "discontinued upstream (disabled on 2026-09-01; replacement: newapp)"));
        CHECK(dep.thrown == kNeedsCount);
        CHECK(f.brew.installs.empty());
    }

    TEST_CASE("a named disabled package and a disabled dependency are counted apart") {
        Fixture f;
        auto r = captured([&] { install_packages(f.cfg, f.brew, f.idx, {"rye", "tree"}); });
        CHECK(f.brew.installs.empty());
        CHECK(r.thrown == "1 requested package(s) are disabled upstream; " + kNeedsCount);
    }

    TEST_CASE("den upgrade refuses an upgrade whose new version needs a disabled dependency") {
        Fixture f;
        f.idx.packages["jq"].dependencies = {"libold"};
        auto named = captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {"jq"}); });
        CAPTURE(named.out);
        CAPTURE(named.err);
        CHECK(f.brew.installs.empty());
        CHECK(has(named.err, "error: " + kJqNeeds + "\n"));
        CHECK(named.thrown == kNeedsCount);
        // The bare form selects jq the same way, and refuses it the same way.
        auto bare = captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {}); });
        CHECK(f.brew.installs.empty());
        CHECK(has(bare.err, "error: " + kJqNeeds));
        CHECK(bare.thrown == kNeedsCount);
        // --dry-run reports it, and does not list it as an upgrade.
        auto dry = captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {"jq"}, true); });
        CHECK(has(dry.err, "error: " + kJqNeeds));
        CHECK_FALSE(has(dry.out, "jq 1.7.1 -> 1.8.2"));
        CHECK(dry.thrown == kNeedsCount);
    }

    TEST_CASE("den upgrade proceeds when the disabled dependency is already satisfied") {
        Fixture f({{"libold", "1.0"}});
        f.idx.packages["jq"].dependencies = {"libold"};
        auto r = captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {"jq"}); });
        CAPTURE(r.err);
        CHECK(f.brew.installs == std::vector<std::string>{"jq@1.8.2"});
        CHECK(r.thrown.empty());
        CHECK_FALSE(has(r.err, "libold"));
    }

    TEST_CASE("den upgrade --force upgrades past a disabled dependency and a named formula") {
        Fixture f;
        f.idx.packages["jq"].dependencies = {"libold"};
        auto r = captured(
            [&] { upgrade_packages(f.cfg, f.brew, f.idx, {"jq", "rye"}, false, /*force=*/true); });
        CAPTURE(r.out);
        CAPTURE(r.err);
        std::sort(f.brew.installs.begin(), f.brew.installs.end());
        CHECK(f.brew.installs == std::vector<std::string>{"jq@1.8.2", "rye@0.44.0"});
        CHECK(r.thrown.empty());
        CHECK(has(r.err, "warning: " + kJqNeeds + "\n"));
        CHECK(has(r.err, "warning: " + kRye + "\n"));
        CHECK_FALSE(has(r.err, "error:"));
    }

    TEST_CASE("den upgrade --force still refuses a disabled cask") {
        Fixture f({{"oldapp", "1.0"}});
        auto r = captured(
            [&] { upgrade_packages(f.cfg, f.brew, f.idx, {"oldapp"}, false, /*force=*/true); });
        CHECK(f.brew.installs.empty());
        CHECK(has(r.err, "error: " + kApp));
        CHECK(r.thrown == "1 requested package(s) are disabled upstream");
    }

    TEST_CASE("a disabled revision-bumped dependent is left alone and fails the upgrade") {
        Fixture f({{"jqx", "1.0"}});
        auto jqx = disabled_formula("jqx", "1.0_1");
        jqx.revision = 1;
        jqx.dependencies = {"jq"};
        f.add(jqx);
        auto r = captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {"jq"}); });
        CAPTURE(r.out);
        CAPTURE(r.err);
        CHECK(f.brew.installs == std::vector<std::string>{"jq@1.8.2"});
        CHECK(has(r.out, "Not upgrading 1 dependent(s) (disabled upstream):\n  jqx 1.0 (1.0_1"));
        CHECK(has(r.err, "error: 'jqx' is disabled upstream"));
        CHECK(r.thrown == "1 dependent(s) are disabled upstream or need a dependency that is");

        // With --force it moves with jq, as any revision-bumped dependent does.
        Fixture g({{"jqx", "1.0"}});
        g.add(jqx);
        auto forced = captured(
            [&] { upgrade_packages(g.cfg, g.brew, g.idx, {"jq"}, false, /*force=*/true); });
        CAPTURE(forced.err);
        CHECK(g.brew.installs == std::vector<std::string>{"jq@1.8.2", "jqx@1.0_1"});
        CHECK(forced.thrown.empty());
        CHECK(has(forced.err, "warning: 'jqx' is disabled upstream"));
    }

    TEST_CASE("a revision-bumped dependent that would pour a disabled dependency is refused") {
        Fixture f({{"jqx", "1.0"}});
        auto jqx = pkg("jqx", "1.0_1", {"jq", "libold"});
        jqx.revision = 1;
        f.add(jqx);
        auto r = captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {"jq"}); });
        CAPTURE(r.err);
        CHECK(f.brew.installs == std::vector<std::string>{"jq@1.8.2"});
        CHECK(has(r.err, "error: 'jqx' needs 'libold', which is disabled upstream"));
        CHECK(r.thrown == "1 dependent(s) are disabled upstream or need a dependency that is");
    }
}

namespace {

Package::Archive any_bottle() {
    Package::Archive ar;
    ar.url = "https://bottles.invalid/x.tar.gz";
    ar.sha256 = std::string(64, 'a');
    ar.relocation = RelocationType::NeedsSubstitution;
    return ar;
}

Package formula(const std::string& name, const std::string& version,
                std::vector<std::string> deps = {}, std::vector<std::string> build_deps = {}) {
    Package p = pkg(name, version, std::move(deps));
    p.archives["default"] = any_bottle();
    p.source_url = "https://example.invalid/" + name + "-" + version + ".tar.gz";
    p.source_sha256 = std::string(64, 'b');
    p.build_dependencies = std::move(build_deps);
    return p;
}

struct Recorder {
    std::vector<std::string> log;
    SourcePlanRunner runner() {
        SourcePlanRunner r;
        r.pour = [this](const Config& c, const Package& p) {
            log.push_back("pour " + p.name);
            fs::create_directories(c.store / p.name / p.version);
            mark_keg_owned(c.store / p.name / p.version, p.name, p.version);
        };
        r.build = [this](const Config& c, const PackageIndex&, const std::string& n,
                         const std::string& v) {
            log.push_back("build " + n);
            fs::create_directories(c.store / n / v);
            mark_keg_owned(c.store / n / v, n, v);
            return c.store / n / v;
        };
        return r;
    }
};

// tool 1.0 (built from source) needs libold 1.0 at runtime, and oldbuild
// 1.0 to build; both of those are disabled formulae.
PackageIndex tool_index() {
    PackageIndex idx;
    auto add = [&](Package p) { idx.packages[p.name] = std::move(p); };
    add(formula("tool", "1.0", {"libold"}, {"oldbuild"}));
    for (const char* name : {"libold", "oldbuild"}) {
        auto p = formula(name, "1.0");
        p.disabled = true;
        p.disable_reason = "unmaintained";
        add(p);
    }
    return idx;
}

} // namespace

TEST_SUITE("disabled_dependency_refusal") {

    TEST_CASE("den install -s refuses a disabled runtime or build-only dependency") {
        DdTmp t;
        auto cfg = scratch_config(t.path);
        auto idx = tool_index();
        Recorder rec;
        auto r = captured([&] { install_from_source(cfg, idx, {"tool"}, {}, rec.runner()); });
        CAPTURE(r.out);
        CAPTURE(r.err);
        CHECK(rec.log.empty());
        CHECK_FALSE(has(r.out, "Source build plan"));
        CHECK(has(r.err, "error: 'tool' needs 'libold', which is disabled upstream: it is not "
                         "maintained upstream\n"));
        CHECK(has(r.err, "error: 'tool' needs 'oldbuild', which is disabled upstream"));
        CHECK(r.thrown == kNeedsCount);
    }

    TEST_CASE("den install -s: satisfied disabled dependencies are not mentioned") {
        DdTmp t;
        auto cfg = scratch_config(t.path);
        auto idx = tool_index();
        for (const char* name : {"libold", "oldbuild"}) {
            fs::create_directories(cfg.store / name / "1.0");
            mark_keg_owned(cfg.store / name / "1.0", name, "1.0");
        }
        Recorder rec;
        auto r = captured([&] { install_from_source(cfg, idx, {"tool"}, {}, rec.runner()); });
        CAPTURE(r.err);
        CHECK(rec.log == std::vector<std::string>{"build tool"});
        CHECK(r.thrown.empty());
        CHECK_FALSE(has(r.err, "disabled"));
    }

    TEST_CASE("den install -s --force builds past disabled formulae, with warnings") {
        DdTmp t;
        auto cfg = scratch_config(t.path);
        auto idx = tool_index();
        idx.packages["tool"].disabled = true;
        Recorder rec;
        auto r = captured(
            [&] { install_from_source(cfg, idx, {"tool"}, {}, rec.runner(), /*force=*/true); });
        CAPTURE(r.out);
        CAPTURE(r.err);
        CHECK(std::find(rec.log.begin(), rec.log.end(), "build tool") != rec.log.end());
        CHECK(std::find(rec.log.begin(), rec.log.end(), "pour libold") != rec.log.end());
        CHECK(r.thrown.empty());
        CHECK(has(r.err, "warning: 'tool' is disabled upstream\n"));
        CHECK(has(r.err, "warning: 'tool' needs 'libold', which is disabled upstream"));
        CHECK(has(r.err, "warning: 'tool' needs 'oldbuild', which is disabled upstream"));
        CHECK_FALSE(has(r.err, "error:"));
    }
}

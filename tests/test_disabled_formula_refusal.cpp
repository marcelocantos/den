// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// A formula or cask the index marks disabled is refused when named and the
// run would install or upgrade it. A bare `den upgrade` already left
// disabled formulae out (outdated_plan), but a named `den upgrade <name>`
// and `den install <name>` went ahead with it. Both now print "error:
// '<name>' is disabled upstream: it <reason> (disabled on <date>;
// replacement: <name>)" from the index's disable! fields, leave the other
// names to proceed, and exit 1. One already installed at the index version
// has nothing to do: it is reported up to date with "warning: <same>" and
// does not fail the run.

#include <doctest.h>

#include "cli/install.h"
#include "core/error.h"
#include "daemon/daemon.h"
#include "env/environment.h"
#include "env/manifest.h"
#include "index/index.h"
#include "index/package.h"
#include "provider/stub_provider.h"

#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct DfTmp {
    fs::path path;
    DfTmp() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("den-disabled-formula-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~DfTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    DfTmp(const DfTmp&) = delete;
    DfTmp& operator=(const DfTmp&) = delete;
};

Config scratch_config(const fs::path& root) {
    Config c;
    c.den_home = root / "den";
    c.homebrew_prefix = root / "prefix";
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

// Runs fn with std::cout and std::cerr captured. doctest reports to
// std::cout too, so assertions go after this returns.
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

Package pkg(const std::string& name, const std::string& version) {
    Package p;
    p.name = name;
    p.version = version;
    return p;
}

const char* kRyeError = "error: 'rye' is disabled upstream: it is not maintained upstream "
                        "(disabled on 2026-08-18; replacement: uv)";
const char* kRyeWarning = "warning: 'rye' is disabled upstream: it is not maintained upstream "
                          "(disabled on 2026-08-18; replacement: uv)";
const char* kAppError = "error: 'oldapp' is disabled upstream: it is discontinued upstream "
                        "(disabled on 2026-09-01; replacement: newapp)";
const char* kAppWarning = "warning: 'oldapp' is disabled upstream: it is discontinued upstream "
                          "(disabled on 2026-09-01; replacement: newapp)";

bool has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// homebrew bucket: jq 1.7.1 (index 1.8.2) and rye at `rye_version` (index
// 0.44.0, disabled 2026-08-18 as unmaintained, replacement uv), plus the
// cask oldapp at `app_version` when given (index 2.0, disabled 2026-09-01 as
// discontinued, replacement newapp). tree 2.2.1 is in the index only.
struct Fixture {
    DfTmp t;
    Config cfg = scratch_config(t.path);
    RecordingProvider brew;
    PackageIndex idx;

    explicit Fixture(const std::string& rye_version = "0.43.0",
                     const std::string& app_version = "") {
        std::vector<std::pair<std::string, std::string>> installed{{"jq", "1.7.1"},
                                                                   {"rye", rye_version}};
        if (!app_version.empty())
            installed.emplace_back("oldapp", app_version);
        Manifest m;
        for (const auto& [name, version] : installed) {
            brew.install(cfg, name, version);
            m.packages["homebrew"][name] = version;
        }
        write_manifest(cfg.den_home, active_env_path(cfg.den_home), m);
        brew.installs.clear();
        idx.packages["jq"] = pkg("jq", "1.8.2");
        idx.packages["tree"] = pkg("tree", "2.2.1");
        auto rye = pkg("rye", "0.44.0");
        rye.disabled = true;
        rye.disable_date = "2026-08-18";
        rye.disable_reason = "unmaintained";
        rye.disable_replacement = "uv";
        idx.packages["rye"] = rye;
        auto app = pkg("oldapp", "2.0");
        app.artifact_type = ArtifactType::App;
        app.disabled = true;
        app.disable_date = "2026-09-01";
        app.disable_reason = "discontinued";
        app.disable_replacement = "newapp";
        idx.packages["oldapp"] = app;
    }
};

} // namespace

TEST_SUITE("disabled_formula_refusal") {

    TEST_CASE("a named disabled formula is refused by den upgrade") {
        Fixture f;
        auto r = captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {"rye"}); });
        CAPTURE(r.out);
        CAPTURE(r.err);
        CHECK(f.brew.installs.empty());
        CHECK(r.err.find(kRyeError) != std::string::npos);
        CHECK(r.thrown == "1 requested package(s) are disabled upstream");
        CHECK(r.out.find("All packages are up to date.") == std::string::npos);
    }

    TEST_CASE("the other named packages still upgrade, then den upgrade fails") {
        Fixture f;
        auto r = captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {"rye", "jq"}); });
        CAPTURE(r.out);
        CAPTURE(r.err);
        CHECK(f.brew.installs == std::vector<std::string>{"jq@1.8.2"});
        CHECK(r.err.find(kRyeError) != std::string::npos);
        CHECK(r.thrown == "1 requested package(s) are disabled upstream");
    }

    TEST_CASE("den upgrade --dry-run refuses a named disabled formula too") {
        Fixture f;
        auto r =
            captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {"rye"}, /*dry_run=*/true); });
        CAPTURE(r.out);
        CHECK(r.err.find(kRyeError) != std::string::npos);
        CHECK(r.thrown == "1 requested package(s) are disabled upstream");
        CHECK(r.out.find("rye 0.43.0 -> 0.44.0") == std::string::npos);
    }

    TEST_CASE("a named disabled formula is refused by den install") {
        Fixture f;
        auto r = captured([&] { install_packages(f.cfg, f.brew, f.idx, {"rye"}); });
        CAPTURE(r.out);
        CAPTURE(r.err);
        CHECK(f.brew.installs.empty());
        CHECK(r.err.find(kRyeError) != std::string::npos);
        CHECK(r.thrown == "1 requested package(s) are disabled upstream");
    }

    TEST_CASE("den install installs the other names, then fails") {
        Fixture f;
        auto r = captured([&] { install_packages(f.cfg, f.brew, f.idx, {"tree", "rye"}); });
        CAPTURE(r.out);
        CAPTURE(r.err);
        REQUIRE(f.brew.installs.size() == 1);
        CHECK(f.brew.installs[0].rfind("tree@", 0) == 0);
        CHECK(r.err.find(kRyeError) != std::string::npos);
        CHECK(r.thrown == "1 requested package(s) are disabled upstream");
    }

    TEST_CASE("den install -s refuses a disabled formula before planning") {
        Fixture f;
        bool ran = false;
        SourcePlanRunner runner;
        runner.pour = [&](const Config&, const Package&) { ran = true; };
        runner.build = [&](const Config&, const PackageIndex&, const std::string&,
                           const std::string&) {
            ran = true;
            return fs::path();
        };
        auto r = captured([&] { install_from_source(f.cfg, f.idx, {"rye"}, {}, runner); });
        CHECK_FALSE(ran);
        CHECK(r.out.find("Source build plan") == std::string::npos);
        CHECK(r.err.find(kRyeError) != std::string::npos);
        CHECK(r.thrown == "1 requested package(s) are disabled upstream");
    }

    TEST_CASE("the message uses only the disable! fields the index has") {
        auto p = pkg("oldtool", "1.0");
        p.disabled = true;
        CHECK(disabled_package_message(p) == "'oldtool' is disabled upstream");
        p.disable_date = "2025-01-01";
        CHECK(disabled_package_message(p) ==
              "'oldtool' is disabled upstream (disabled on 2025-01-01)");
        p.disable_reason = "needs EOL `pcre`";
        CHECK(disabled_package_message(p) ==
              "'oldtool' is disabled upstream: it needs EOL `pcre` (disabled on 2025-01-01)");
        p.disable_reason = "repo_archived";
        p.disable_date.clear();
        CHECK(disabled_package_message(p) ==
              "'oldtool' is disabled upstream: it has an archived upstream repository");
    }

    TEST_CASE("the index keeps Homebrew's disable_date, disable_reason and replacement") {
        const std::string formulae = R"([{
          "name": "rye", "desc": "", "homepage": "", "license": "MIT",
          "versions": {"stable": "0.44.0"}, "revision": 0,
          "deprecated": true, "disabled": true,
          "disable_date": "2026-08-18", "disable_reason": "unmaintained",
          "disable_replacement_formula": "uv", "disable_replacement_cask": null,
          "dependencies": [], "build_dependencies": [], "bottle": {}
        }, {
          "name": "fine", "desc": "", "homepage": "", "license": "MIT",
          "versions": {"stable": "1.0"}, "revision": 0,
          "deprecated": false, "disabled": false,
          "disable_date": null, "disable_reason": null,
          "dependencies": [], "build_dependencies": [], "bottle": {}
        }])";
        auto idx = transform_homebrew(formulae, "[]");
        const auto* rye = idx.find("rye");
        REQUIRE(rye != nullptr);
        CHECK(rye->disabled);
        CHECK(rye->disable_date == "2026-08-18");
        CHECK(rye->disable_reason == "unmaintained");
        CHECK(rye->disable_replacement == "uv");
        const auto* fine = idx.find("fine");
        REQUIRE(fine != nullptr);
        CHECK(fine->disable_date.empty());
        CHECK(fine->disable_reason.empty());

        // ... and they survive index.json.
        DfTmp t;
        const auto path = t.path / "index.json";
        save_index(idx, path);
        auto loaded = load_index_json(path);
        const auto* back = loaded.find("rye");
        REQUIRE(back != nullptr);
        CHECK(back->disable_date == "2026-08-18");
        CHECK(back->disable_reason == "unmaintained");
        CHECK(back->disable_replacement == "uv");
    }
    TEST_CASE("den upgrade: a disabled formula already at the index version is up to date") {
        Fixture f("0.44.0");
        auto r = captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {"rye"}); });
        CAPTURE(r.out);
        CAPTURE(r.err);
        CHECK(f.brew.installs.empty());
        CHECK(r.thrown.empty());
        CHECK(has(r.out, "rye is already up to date (0.44.0).\n"));
        CHECK(has(r.err, kRyeWarning));
        CHECK_FALSE(has(r.err, "error:"));
    }

    TEST_CASE("den upgrade: an up-to-date disabled formula does not fail the other names") {
        Fixture f("0.44.0");
        auto r = captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {"rye", "jq"}); });
        CAPTURE(r.out);
        CAPTURE(r.err);
        CHECK(f.brew.installs == std::vector<std::string>{"jq@1.8.2"});
        CHECK(r.thrown.empty());
        CHECK(has(r.err, kRyeWarning));
    }

    TEST_CASE("den upgrade --dry-run: an up-to-date disabled formula is up to date") {
        Fixture f("0.44.0");
        auto r =
            captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {"rye"}, /*dry_run=*/true); });
        CAPTURE(r.out);
        CHECK(r.thrown.empty());
        CHECK(has(r.out, "All packages are up to date."));
        CHECK(has(r.err, kRyeWarning));
    }

    TEST_CASE("den upgrade: a disabled formula the index is older than is not refused") {
        // Nothing would be applied (an upgrade never downgrades), so it is
        // reported as index-older, with the warning.
        Fixture f("0.45.0");
        auto r = captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {"rye"}); });
        CAPTURE(r.out);
        CAPTURE(r.err);
        CHECK(f.brew.installs.empty());
        CHECK(r.thrown.empty());
        CHECK(has(r.out, "rye 0.45.0 (index has 0.44.0"));
        CHECK(has(r.err, kRyeWarning));
    }

    TEST_CASE("den install: a disabled formula already at the index version is up to date") {
        Fixture f("0.44.0");
        auto r = captured([&] { install_packages(f.cfg, f.brew, f.idx, {"rye"}); });
        CAPTURE(r.out);
        CAPTURE(r.err);
        CHECK(f.brew.installs.empty());
        CHECK(r.thrown.empty());
        CHECK(has(r.out, "rye is already up to date (0.44.0).\n"));
        CHECK(has(r.err, kRyeWarning));
        CHECK_FALSE(has(r.err, "error:"));
    }

    TEST_CASE("den install: an up-to-date disabled formula does not fail the other names") {
        Fixture f("0.44.0");
        auto r = captured([&] { install_packages(f.cfg, f.brew, f.idx, {"tree", "rye"}); });
        CAPTURE(r.out);
        CAPTURE(r.err);
        REQUIRE(f.brew.installs.size() == 1);
        CHECK(f.brew.installs[0].rfind("tree@", 0) == 0);
        CHECK(r.thrown.empty());
        CHECK(has(r.err, kRyeWarning));
    }

    TEST_CASE("den upgrade refuses a named disabled cask with a newer index version") {
        Fixture f("0.43.0", "1.0");
        auto r = captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {"oldapp", "jq"}); });
        CAPTURE(r.out);
        CAPTURE(r.err);
        CHECK(f.brew.installs == std::vector<std::string>{"jq@1.8.2"});
        CHECK(has(r.err, kAppError));
        CHECK(r.thrown == "1 requested package(s) are disabled upstream");
    }

    TEST_CASE("den install refuses a disabled cask that is not installed") {
        Fixture f;
        auto r = captured([&] { install_packages(f.cfg, f.brew, f.idx, {"oldapp"}); });
        CAPTURE(r.out);
        CAPTURE(r.err);
        CHECK(f.brew.installs.empty());
        CHECK(has(r.err, kAppError));
        CHECK(r.thrown == "1 requested package(s) are disabled upstream");
    }

    TEST_CASE("den install refuses a disabled cask installed at an older version") {
        Fixture f("0.43.0", "1.0");
        auto r = captured([&] { install_packages(f.cfg, f.brew, f.idx, {"oldapp"}); });
        CHECK(f.brew.installs.empty());
        CHECK(has(r.err, kAppError));
        CHECK(r.thrown == "1 requested package(s) are disabled upstream");
    }

    TEST_CASE("a disabled cask already at the index version is up to date") {
        Fixture f("0.43.0", "2.0");
        auto up = captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {"oldapp"}); });
        CAPTURE(up.out);
        CAPTURE(up.err);
        CHECK(up.thrown.empty());
        CHECK(has(up.out, "oldapp is already up to date (2.0).\n"));
        CHECK(has(up.err, kAppWarning));
        auto in = captured([&] { install_packages(f.cfg, f.brew, f.idx, {"oldapp"}); });
        CAPTURE(in.out);
        CAPTURE(in.err);
        CHECK(in.thrown.empty());
        CHECK(has(in.out, "oldapp is already up to date (2.0).\n"));
        CHECK(has(in.err, kAppWarning));
        CHECK(f.brew.installs.empty());
    }

    TEST_CASE("a bare den upgrade does not offer a disabled cask for upgrade by name") {
        Fixture f("0.43.0", "1.0");
        CHECK(select_upgrade("oldapp", "1.0", f.idx.find("oldapp")).skip == UpgradeSkip::Disabled);
        auto r = captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {}, /*dry_run=*/true); });
        CAPTURE(r.out);
        CHECK(has(r.out, "jq 1.7.1 -> 1.8.2"));
        CHECK_FALSE(has(r.out, "oldapp"));
        CHECK_FALSE(has(r.out, "cask(s)"));
    }

    TEST_CASE("a disabled cask's reason reads as brew words cask reasons") {
        auto p = pkg("someapp", "1.0");
        p.artifact_type = ArtifactType::App;
        p.disabled = true;
        p.disable_reason = "fails_gatekeeper_check";
        CHECK(disabled_package_message(p) ==
              "'someapp' is disabled upstream: it does not pass the macOS Gatekeeper check");
        p.disable_reason = "moved_to_mas";
        CHECK(disabled_package_message(p) == "'someapp' is disabled upstream: it is now "
                                             "exclusively distributed on the Mac App Store");
    }

    TEST_CASE("the index keeps a cask's disable_date, disable_reason and replacement") {
        const std::string casks = R"([{
          "token": "dataspell", "name": ["DataSpell"], "desc": "", "homepage": "",
          "version": "2026.1.3", "url": "https://example.com/dataspell.dmg",
          "sha256": "no_check", "artifacts": [{"app": ["DataSpell.app"]}],
          "deprecated": true, "disabled": true,
          "disable_date": "2026-09-01", "disable_reason": "discontinued",
          "disable_replacement_formula": null, "disable_replacement_cask": "pycharm"
        }])";
        auto idx = transform_homebrew("[]", casks);
        const auto* ds = idx.find("dataspell");
        REQUIRE(ds != nullptr);
        CHECK(ds->disabled);
        CHECK(ds->disable_date == "2026-09-01");
        CHECK(ds->disable_reason == "discontinued");
        CHECK(ds->disable_replacement == "pycharm");
        CHECK(disabled_package_message(*ds) ==
              "'dataspell' is disabled upstream: it is discontinued upstream (disabled on "
              "2026-09-01; replacement: pycharm)");

        DfTmp t;
        const auto path = t.path / "index.json";
        save_index(idx, path);
        auto check_back = [](const PackageIndex& loaded) {
            const auto* back = loaded.find("dataspell");
            REQUIRE(back != nullptr);
            CHECK(back->disable_date == "2026-09-01");
            CHECK(back->disable_reason == "discontinued");
            CHECK(back->disable_replacement == "pycharm");
        };
        check_back(load_index_json(path)); // straight from JSON
        check_back(load_index(path));      // through the binary lookup cache
    }
}

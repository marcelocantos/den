// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// G3/G9: planning and running a source-build chain. Kegs whose bottle pours
// into den's Cellar pour; kegs whose bottle needs a fixed Cellar den isn't
// using are built from source, after their runtime and build dependencies;
// unattended runs build only what trust.unattended_source_builds allows and
// defer the rest, with everything that needs it.

#include "build/source_plan.h"
#include "core/config.h"
#include "core/error.h"
#include "index/index.h"
#include "index/package.h"
#include "provider/homebrew_provider.h"
#include "trust/degraded_installs.h"
#include "trust/unattended.h"

#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace den;

namespace {

fs::path fresh_tmp(const std::string& tag) {
    auto tmp = fs::temp_directory_path() / ("den-" + tag + "-" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    return tmp;
}

Config tmp_config(const fs::path& tmp) {
    Config cfg;
    cfg.den_home = tmp / "den";
    cfg.cache = cfg.den_home / "cache";
    cfg.store = tmp / "prefix" / "Cellar";
    cfg.prefix = cfg.store.parent_path();
    cfg.homebrew_cellar = cfg.store;
    fs::create_directories(cfg.cache / "archives");
    fs::create_directories(cfg.store);
    return cfg;
}

Package::Archive any_bottle() {
    Package::Archive ar;
    ar.url = "https://bottles.invalid/x.tar.gz";
    ar.sha256 = std::string(64, 'a');
    ar.relocation = RelocationType::NeedsSubstitution;
    return ar;
}

Package::Archive fixed_bottle() {
    Package::Archive ar = any_bottle();
    ar.relocation = RelocationType::HardcodedPrefix;
    ar.cellar = "/opt/homebrew/Cellar";
    return ar;
}

Package formula(const std::string& name, const std::string& version, bool fixed,
                std::vector<std::string> deps = {}, std::vector<std::string> build_deps = {},
                bool source = true) {
    Package p;
    p.name = name;
    p.version = version;
    p.archives["default"] = fixed ? fixed_bottle() : any_bottle();
    if (source) {
        p.source_url = "https://example.invalid/" + name + "-" + version + ".tar.gz";
        p.source_sha256 = std::string(64, 'b');
    }
    p.dependencies = std::move(deps);
    p.build_dependencies = std::move(build_deps);
    return p;
}

// The git chain from the scope doc, cut down: :any bottles pour, the
// /opt/homebrew-only ones build.
PackageIndex git_chain() {
    PackageIndex idx;
    auto add = [&](Package p) { idx.packages[p.name] = std::move(p); };
    add(formula("ca-certificates", "2026-01-01", false));
    add(formula("libunistring", "1.3", false));
    add(formula("json-c", "0.18", false));
    add(formula("pcre2", "10.45", false));
    add(formula("gettext", "0.26", true, {"libunistring", "json-c"}));
    add(formula("openssl@3", "3.5.9", true, {"ca-certificates"}));
    add(formula("openssl@4", "4.0.0", true, {"ca-certificates"}));
    add(formula("pkgconf", "2.5.1", true));
    add(formula("curl", "8.15.0", false, {"openssl@4"}, {"pkgconf"}));
    add(formula("git", "2.51.0", true, {"pcre2", "gettext", "curl"}, {"gettext", "pkgconf"}));
    return idx;
}

size_t pos(const SourcePlan& plan, const std::string& name) {
    for (size_t i = 0; i < plan.steps.size(); ++i)
        if (plan.steps[i].name == name)
            return i;
    FAIL("not in plan: " << name);
    return 0;
}

std::string ledger(const Config& cfg) {
    std::ifstream in(degraded_installs_path(cfg));
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Recorder {
    std::vector<std::string> log;
    SourcePlanRunner runner() {
        SourcePlanRunner r;
        r.pour = [this](const Config& c, const Package& p) {
            log.push_back("pour " + p.name);
            fs::create_directories(c.store / p.name / p.version);
        };
        r.build = [this](const Config& c, const PackageIndex&, const std::string& n,
                         const std::string& v) {
            log.push_back("build " + n);
            fs::create_directories(c.store / n / v);
            return c.store / n / v;
        };
        return r;
    }
};

TrustPolicy unattended_policy(bool allowlist, std::set<std::string> names = {}) {
    TrustPolicy p;
    p.unattended = "daemon";
    p.source_builds = allowlist;
    p.source_build_allowlist = std::move(names);
    return p;
}

} // namespace

TEST_SUITE("build::source_plan") {

    TEST_CASE("plan orders the git chain dependencies first and builds only fixed-Cellar kegs") {
        auto tmp = fresh_tmp("source-plan-order");
        auto cfg = tmp_config(tmp);
        auto idx = git_chain();
        auto plan = plan_source_build(cfg, idx, {"git"});

        // openssl@4 is built before the curl that links it, curl before git.
        CHECK(pos(plan, "openssl@4") < pos(plan, "curl"));
        CHECK(pos(plan, "curl") < pos(plan, "git"));
        CHECK(pos(plan, "gettext") < pos(plan, "git"));
        CHECK(pos(plan, "libunistring") < pos(plan, "gettext"));
        CHECK(pos(plan, "ca-certificates") < pos(plan, "openssl@4"));
        CHECK(pos(plan, "pkgconf") < pos(plan, "git"));

        CHECK(plan.find("git")->action == SourceStepAction::Build);
        CHECK(plan.find("git")->why == "bottle needs /opt/homebrew/Cellar");
        CHECK(plan.find("gettext")->action == SourceStepAction::Build);
        CHECK(plan.find("openssl@4")->action == SourceStepAction::Build);
        for (auto n : {"ca-certificates", "libunistring", "json-c", "pcre2", "curl"})
            CHECK_MESSAGE(plan.find(n)->action == SourceStepAction::Pour, n);

        // pkgconf is a build-only dependency built from source; curl's own
        // build dependency on it is not what put it here (curl pours).
        CHECK(plan.find("pkgconf")->action == SourceStepAction::Build);
        CHECK(plan.find("pkgconf")->build_only);
        CHECK_FALSE(plan.find("gettext")->build_only); // also a runtime dep of git
        CHECK_FALSE(plan.find("curl")->build_only);
        // openssl@3 is in the index but nothing here needs it.
        CHECK(plan.find("openssl@3") == nullptr);
        CHECK(plan.steps.size() == 9);
        fs::remove_all(tmp);
    }

    TEST_CASE("build dependencies of a keg that pours are not followed") {
        auto tmp = fresh_tmp("source-plan-pour-bdeps");
        auto cfg = tmp_config(tmp);
        auto idx = git_chain();
        auto plan = plan_source_build(cfg, idx, {"curl"});
        CHECK(plan.find("pkgconf") == nullptr);
        CHECK(plan.find("curl")->action == SourceStepAction::Pour);
        CHECK(plan.find("openssl@4")->action == SourceStepAction::Build);

        // ...but they are when the keg is built on request.
        SourcePlanOptions opts;
        opts.force_source = {"curl"};
        auto forced = plan_source_build(cfg, idx, {"curl"}, opts);
        CHECK(forced.find("curl")->action == SourceStepAction::Build);
        CHECK(forced.find("curl")->why == "requested");
        REQUIRE(forced.find("pkgconf") != nullptr);
        CHECK(forced.find("pkgconf")->build_only);
        fs::remove_all(tmp);
    }

    TEST_CASE("installed kegs are skipped") {
        auto tmp = fresh_tmp("source-plan-installed");
        auto cfg = tmp_config(tmp);
        auto idx = git_chain();
        fs::create_directories(cfg.store / "openssl@4" / "4.0.0");
        auto plan = plan_source_build(cfg, idx, {"curl"});
        CHECK(plan.find("openssl@4")->action == SourceStepAction::Installed);

        Recorder rec;
        auto res = run_source_plan(cfg, idx, plan, TrustPolicy{}, rec.runner());
        CHECK(rec.log == std::vector<std::string>{"pour ca-certificates", "pour curl"});
        CHECK(res.built.empty());
        fs::remove_all(tmp);
    }

    TEST_CASE("plan errors: cycles, missing dependencies, no source") {
        auto tmp = fresh_tmp("source-plan-errors");
        auto cfg = tmp_config(tmp);
        PackageIndex idx;
        idx.packages["a"] = formula("a", "1", true, {"b"});
        idx.packages["b"] = formula("b", "1", true, {}, {"a"});
        idx.packages["c"] = formula("c", "1", true, {"ghost"});
        idx.packages["d"] = formula("d", "1", false, {"e"});
        idx.packages["e"] = formula("e", "1", true, {}, {}, false);

        CHECK_THROWS_WITH_AS(plan_source_build(cfg, idx, {"a"}),
                             doctest::Contains("dependency cycle: a -> b -> a"), UserError);
        CHECK_THROWS_WITH_AS(plan_source_build(cfg, idx, {"c"}),
                             doctest::Contains("'ghost' is not in the package index (needed by c)"),
                             UserError);
        CHECK_THROWS_WITH_AS(
            plan_source_build(cfg, idx, {"d"}),
            doctest::Contains("cannot install e 1: bottle needs /opt/homebrew/Cellar and no "
                              "source build is available (needed by d)"),
            UserError);
        SourcePlanOptions opts;
        opts.force_source = {"e"};
        CHECK_THROWS_WITH_AS(plan_source_build(cfg, idx, {"e"}, opts),
                             doctest::Contains("no source build is available for e"), UserError);
        fs::remove_all(tmp);
    }

    TEST_CASE("an interactive run pours and builds the whole chain in order") {
        auto tmp = fresh_tmp("source-plan-run");
        auto cfg = tmp_config(tmp);
        auto idx = git_chain();
        auto plan = plan_source_build(cfg, idx, {"git"});
        Recorder rec;
        auto res = run_source_plan(cfg, idx, plan, TrustPolicy{}, rec.runner());
        CHECK(res.deferred.empty());
        CHECK(res.built.size() == 4); // gettext, openssl@4, pkgconf, git
        CHECK(rec.log.back() == "build git");
        auto at = [&](const std::string& s) {
            return std::find(rec.log.begin(), rec.log.end(), s) - rec.log.begin();
        };
        CHECK(at("build openssl@4") < at("pour curl"));
        CHECK(at("pour curl") < at("build git"));
        fs::remove_all(tmp);
    }

    TEST_CASE("unattended with source builds off defers the build and everything that needs it") {
        auto tmp = fresh_tmp("source-plan-defer");
        auto cfg = tmp_config(tmp);
        auto idx = git_chain();
        auto plan = plan_source_build(cfg, idx, {"git"});
        Recorder rec;
        auto res = run_source_plan(cfg, idx, plan, unattended_policy(false), rec.runner());
        CHECK(res.built.empty());
        CHECK(res.is_deferred("openssl@4"));
        CHECK(res.is_deferred("curl")); // pours, but needs the deferred openssl@4
        CHECK(res.is_deferred("git"));
        // Bottles that need nothing deferred still pour.
        CHECK(std::find(rec.log.begin(), rec.log.end(), "pour pcre2") != rec.log.end());
        CHECK(std::find(rec.log.begin(), rec.log.end(), "pour curl") == rec.log.end());

        auto l = ledger(cfg);
        CHECK(l.find("unattended-source-build") != std::string::npos);
        CHECK(l.find("trust.unattended_source_builds=off") != std::string::npos);
        CHECK(l.find("depends on openssl@4: deferred") != std::string::npos);
        fs::remove_all(tmp);
    }

    TEST_CASE("unattended allowlisted builds still need a cross-check or an accept") {
        auto tmp = fresh_tmp("source-plan-allow");
        auto cfg = tmp_config(tmp);
        // Names the shipped replica knows nothing about.
        PackageIndex idx;
        idx.packages["probe-listed"] = formula("probe-listed", "1.0", true);
        idx.packages["probe-other"] = formula("probe-other", "1.0", true);
        auto plan = plan_source_build(cfg, idx, {"probe-listed"});
        auto policy = unattended_policy(true, {"probe-listed"});
        Recorder rec;

        // No replica entry to cross-check against: deferred, not built.
        auto res = run_source_plan(cfg, idx, plan, policy, rec.runner());
        CHECK(res.is_deferred("probe-listed"));
        CHECK(rec.log.empty());
        CHECK(ledger(cfg).find("not in trust.source_build_allowlist") == std::string::npos);

        // Not on the allowlist at all: deferred and said so.
        auto plan2 = plan_source_build(cfg, idx, {"probe-other"});
        auto res2 = run_source_plan(cfg, idx, plan2, policy, rec.runner());
        CHECK(res2.is_deferred("probe-other"));
        CHECK(ledger(cfg).find("not in trust.source_build_allowlist") != std::string::npos);

        // `den trust accept` lets it through; a build clears the entry.
        REQUIRE(accept_refused_install(cfg, "probe-listed", "1.0"));
        auto res3 = run_source_plan(cfg, idx, plan, policy, rec.runner());
        CHECK(res3.built == std::vector<std::string>{"probe-listed"});
        CHECK_FALSE(is_install_accepted(cfg, "probe-listed", "1.0"));
        fs::remove_all(tmp);
    }

    TEST_CASE("the provider routes a fixed-Cellar keg with a source to a source build") {
        auto tmp = fresh_tmp("source-plan-provider");
        auto cfg = tmp_config(tmp);
        auto idx = git_chain();
        // Its :any runtime dependencies are already here.
        fs::create_directories(cfg.store / "libunistring" / "1.3");
        fs::create_directories(cfg.store / "json-c" / "0.18");
        Recorder rec;
        auto runner = rec.runner();
        set_source_plan_runner_for_testing(&runner);
        HomebrewProvider provider(&idx);
        std::string err;
        try {
            provider.install(cfg, "gettext", "");
        } catch (const std::exception& e) {
            err = e.what();
        }
        set_source_plan_runner_for_testing(nullptr);
        CHECK(err.empty());
        CHECK(std::find(rec.log.begin(), rec.log.end(), "build gettext") != rec.log.end());
        CHECK(fs::exists(cfg.store / "gettext" / "0.26"));
        // Nothing was downloaded for the bottle it can't pour.
        CHECK(fs::is_empty(cfg.cache / "archives"));
        fs::remove_all(tmp);
    }
}

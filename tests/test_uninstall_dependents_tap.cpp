// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// The uninstall dependents check with tap formulae: manifest entries and
// dependencies may be bare ("helper"), qualified ("user/repo/helper") or in
// the user/homebrew-repo form, and all of those must match each other when
// they mean the same formula, but never a core formula that merely shares
// the bare name.

#include <doctest.h>

#include "cli/uninstall_deps.h"
#include "env/manifest.h"
#include "index/index.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace den {
namespace uninstall_tap_test {

namespace fs = std::filesystem;

namespace {

struct Tmp {
    fs::path path;
    Tmp() {
        path = fs::temp_directory_path() /
               ("den_test_uninstall_tap_" +
                std::to_string(static_cast<size_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    Tmp(const Tmp&) = delete;
    Tmp& operator=(const Tmp&) = delete;
};

struct Fx {
    Tmp tmp;
    Config cfg;
    PackageIndex idx;

    Fx() {
        cfg.den_home = tmp.path / "den";
        cfg.cache = cfg.den_home / "cache";
        cfg.store = tmp.path / "prefix" / "Cellar";
        fs::create_directories(cfg.store);
    }

    // A core formula: relative ruby_source_path, as in the API index.
    void core(const std::string& name, std::vector<std::string> deps = {}) {
        Package p;
        p.name = name;
        p.version = "9.9";
        p.ruby_source_path = "Formula/" + name.substr(0, 1) + "/" + name + ".rb";
        p.dependencies = std::move(deps);
        idx.packages[name] = p;
    }

    // A formula in tap test/tap, indexed the way merge_taps_into_index does:
    // always under the qualified name, and under the bare name only when no
    // core formula already claims it.
    void tap(const std::string& name, std::vector<std::string> deps = {}) {
        Package p;
        p.name = name;
        p.version = "1.0";
        p.ruby_source_path = (tmp.path / "den/taps/test/tap/Formula" / (name + ".rb")).string();
        p.dependencies = std::move(deps);
        Package q = p;
        q.name = "test/tap/" + name;
        idx.packages[q.name] = q;
        if (!idx.find(name)) {
            idx.packages[name] = p;
        }
    }

    void env(const std::string& path, const std::vector<std::string>& names) {
        Manifest m;
        for (const auto& n : names) {
            m.packages["homebrew"][n] = "1.0";
        }
        write_manifest(cfg.den_home, path, m);
    }

    std::map<std::string, std::vector<RuntimeDependent>>
    check(const std::vector<std::string>& names, const std::string& active = "/") {
        return find_runtime_dependents(cfg, idx, active, names);
    }

    // Names of the dependents blocking `target`.
    std::vector<std::string> blockers(const std::vector<std::string>& names,
                                      const std::string& target, const std::string& active = "/") {
        std::vector<std::string> out;
        auto d = check(names, active);
        if (d.count(target)) {
            for (const auto& r : d[target]) {
                out.push_back(r.env + ":" + r.name);
            }
        }
        return out;
    }
};

struct Run {
    std::string out;
    int code;
};

Run run_den(const std::string& args, const fs::path& root) {
    const auto prefix = (root / "prefix").string();
    std::string cmd = "HOME=" + root.string() + " DEN_HOME=" + (root / "den").string() +
                      " DEN_PREFIX=" + prefix + " HOMEBREW_PREFIX=" + prefix +
                      " HOMEBREW_CELLAR=" + prefix + "/Cellar ./den " + args + " 2>&1";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    REQUIRE(pipe != nullptr);
    std::string out;
    std::array<char, 512> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        out += buf.data();
    }
    int raw = ::pclose(pipe);
    return {out, WIFEXITED(raw) ? WEXITSTATUS(raw) : -1};
}

const std::vector<std::string> kApp{"/:test/tap/app"};

} // namespace

TEST_SUITE("uninstall::dependents::tap") {

    TEST_CASE("a tap dependent with a qualified dep blocks every form of the target") {
        Fx fx;
        fx.tap("helper");
        fx.tap("app", {"test/tap/helper"});
        fx.env("/", {"test/tap/app", "test/tap/helper"});
        CHECK(fx.blockers({"helper"}, "helper") == kApp);
        CHECK(fx.blockers({"test/tap/helper"}, "test/tap/helper") == kApp);
        CHECK(fx.blockers({"test/homebrew-tap/helper"}, "test/homebrew-tap/helper") == kApp);
    }

    TEST_CASE("a tap dependent with a bare dep blocks the qualified target") {
        Fx fx;
        fx.tap("helper");
        fx.tap("app", {"helper"});               // no core helper, so bare means the tap's
        fx.env("/", {"test/tap/app", "helper"}); // a pre-qualification bare entry
        CHECK(fx.blockers({"test/tap/helper"}, "test/tap/helper") == kApp);
        CHECK(fx.blockers({"test/homebrew-tap/helper"}, "test/homebrew-tap/helper") == kApp);
        CHECK(fx.blockers({"helper"}, "helper") == kApp);
    }

    TEST_CASE("a homebrew- form dependency matches the canonical target") {
        Fx fx;
        fx.tap("helper");
        fx.tap("app", {"test/homebrew-tap/helper"});
        fx.env("/", {"test/tap/app", "test/tap/helper"});
        CHECK(fx.blockers({"test/tap/helper"}, "test/tap/helper") == kApp);
        CHECK(fx.blockers({"helper"}, "helper") == kApp);
    }

    TEST_CASE("co-removal matches bare, qualified and homebrew- forms") {
        Fx fx;
        fx.tap("helper");
        fx.tap("app", {"test/tap/helper"});
        fx.env("/", {"test/tap/app", "test/tap/helper"});
        CHECK(fx.check({"helper", "app"}).empty());
        CHECK(fx.check({"test/homebrew-tap/helper", "test/homebrew-tap/app"}).empty());
        CHECK(fx.check({"test/tap/helper", "app"}).empty());
    }

    TEST_CASE("'env still provides it' compares canonical names") {
        Fx fx;
        fx.tap("helper");
        fx.tap("app", {"test/tap/helper"});
        fx.env("/", {"test/tap/helper"});
        fx.env("/work", {"test/tap/app", "helper"}); // its own (bare) helper stays
        CHECK(fx.check({"test/tap/helper"}, "/").empty());
        CHECK(fx.check({"test/homebrew-tap/helper"}, "/").empty());

        fx.env("/work", {"test/tap/app"}); // now it inherits helper from /
        CHECK(fx.blockers({"helper"}, "helper", "/") ==
              std::vector<std::string>{"/work:test/tap/app"});
    }

    TEST_CASE("a core-shadowed tap formula and the core formula don't cross-match") {
        // Distinct installs: core helper 9.9 in /a, the tap's helper 1.0 in
        // /b (kegs Cellar/helper/9.9 and Cellar/helper/1.0).
        Fx fx;
        fx.core("helper");
        fx.core("coreapp", {"helper"});
        fx.tap("helper"); // indexed only as test/tap/helper: core claims "helper"
        fx.tap("app", {"test/tap/helper"});
        REQUIRE(fx.idx.find("helper")->version == "9.9");
        fx.env("/a", {"helper", "coreapp"});
        fx.env("/b", {"test/tap/helper", "test/tap/app"});

        // Uninstalling core helper from /a: only coreapp needs it.
        CHECK(fx.blockers({"helper"}, "helper", "/a") == std::vector<std::string>{"/a:coreapp"});
        CHECK(fx.check({"helper", "coreapp"}, "/a").empty());
        // Uninstalling the tap helper from /b: only the tap app needs it.
        CHECK(fx.blockers({"test/tap/helper"}, "test/tap/helper", "/b") ==
              std::vector<std::string>{"/b:test/tap/app"});
        // No core app, so bare "app" is the tap's: removed together, no block.
        CHECK(fx.check({"test/homebrew-tap/helper", "app"}, "/b").empty());
    }

    TEST_CASE("a tap dependent doesn't block uninstalling the core formula it shadows") {
        // /work's tap app lost its tap helper (e.g. removed earlier with
        // --ignore-dependencies) and inherits from /, which has core helper.
        // Removing core helper must not be blamed on the tap app.
        Fx fx;
        fx.core("helper");
        fx.tap("helper");
        fx.tap("app", {"test/tap/helper"});
        fx.env("/", {"helper"});
        fx.env("/work", {"test/tap/app"});
        CHECK(fx.check({"helper"}, "/").empty());
    }

    TEST_CASE("a same-named core formula doesn't count as still providing the tap formula") {
        Fx fx;
        fx.core("helper");
        fx.tap("helper");
        fx.tap("app", {"test/tap/helper"});
        fx.env("/", {"test/tap/helper"});
        fx.env("/work", {"test/tap/app", "helper"}); // core helper, not the tap's
        CHECK(fx.blockers({"test/tap/helper"}, "test/tap/helper", "/") ==
              std::vector<std::string>{"/work:test/tap/app"});
        CHECK(fx.blockers({"test/homebrew-tap/helper"}, "test/homebrew-tap/helper", "/") ==
              std::vector<std::string>{"/work:test/tap/app"});
    }

    TEST_CASE("receipt deps in the homebrew- form match a bare tap target") {
        Fx fx;
        fx.tap("helper");
        fx.env("/", {"legacy", "test/tap/helper"});
        const auto keg = fx.cfg.store / "legacy" / "1.0";
        fs::create_directories(keg);
        std::ofstream(keg / "INSTALL_RECEIPT.json")
            << R"({"runtime_dependencies":[{"full_name":"test/homebrew-tap/helper"}]})";
        CHECK(fx.blockers({"helper"}, "helper") == std::vector<std::string>{"/:legacy"});
    }

    TEST_CASE("resolve_uninstall_name maps each form onto the manifest key") {
        Fx fx;
        fx.core("helper");
        fx.tap("helper");
        fx.tap("only");
        Manifest m;
        m.packages["homebrew"]["test/tap/helper"] = "1.0";
        m.packages["homebrew"]["only"] = "1.0"; // a pre-qualification bare entry
        CHECK(resolve_uninstall_name(fx.idx, m, "test/tap/helper") == "test/tap/helper");
        CHECK(resolve_uninstall_name(fx.idx, m, "test/homebrew-tap/helper") == "test/tap/helper");
        // Only the tap's helper is installed, so the bare name finds it.
        CHECK(resolve_uninstall_name(fx.idx, m, "helper") == "test/tap/helper");
        CHECK(resolve_uninstall_name(fx.idx, m, "test/tap/only") == "only");
        CHECK(resolve_uninstall_name(fx.idx, m, "test/homebrew-tap/only") == "only");
        CHECK(resolve_uninstall_name(fx.idx, m, "missing") == "missing");

        // With core helper also listed, the bare name means core.
        m.packages["homebrew"]["helper"] = "9.9";
        CHECK(resolve_uninstall_name(fx.idx, m, "helper") == "helper");
        CHECK(resolve_uninstall_name(fx.idx, m, "test/homebrew-tap/helper") == "test/tap/helper");
    }
}

TEST_SUITE("uninstall::dependents::tap::cli") {

    TEST_CASE("den uninstall of a tap helper refuses by any name; override removes the entry") {
        if (!fs::exists("./den")) {
            MESSAGE("den binary not in cwd — skipping");
            return;
        }
        Fx fx;
        fx.core("helper"); // core shadows the tap helper's bare name
        fx.tap("helper");
        fx.tap("app", {"test/tap/helper"});
        save_index(fx.idx, fx.cfg.cache / "index.json");
        fx.env("/", {"test/tap/app", "test/tap/helper"});

        for (const auto* name : {"helper", "test/tap/helper", "test/homebrew-tap/helper"}) {
            auto r = run_den(std::string("uninstall ") + name, fx.tmp.path);
            INFO(name, " -> ", r.out);
            CHECK(r.code != 0);
            CHECK(r.out.find("Refusing to uninstall test/tap/helper") != std::string::npos);
            CHECK(r.out.find("test/tap/app (environment /)") != std::string::npos);
            CHECK(
                read_manifest(fx.cfg.den_home, "/").packages["homebrew"].count("test/tap/helper") ==
                1);
        }

        // Removing the dependent with it (in the homebrew- form) is fine.
        auto r = run_den("uninstall helper test/homebrew-tap/app", fx.tmp.path);
        INFO(r.out);
        CHECK(r.code == 0);
        CHECK(read_manifest(fx.cfg.den_home, "/").packages["homebrew"].empty());
    }

    TEST_CASE("a core install isn't blocked by a dependent of the same-named tap formula") {
        if (!fs::exists("./den")) {
            MESSAGE("den binary not in cwd — skipping");
            return;
        }
        Fx fx;
        fx.core("helper");
        fx.tap("helper");
        fx.tap("app", {"test/tap/helper"});
        save_index(fx.idx, fx.cfg.cache / "index.json");
        fx.env("/", {"helper"});                              // core helper
        fx.env("/work", {"test/tap/app", "test/tap/helper"}); // the tap pair

        auto r = run_den("uninstall helper", fx.tmp.path);
        INFO(r.out);
        CHECK(r.code == 0);
        CHECK(r.out.find("Refusing") == std::string::npos);
        CHECK(read_manifest(fx.cfg.den_home, "/").packages["homebrew"].count("helper") == 0);
        CHECK(read_manifest(fx.cfg.den_home, "/work").packages["homebrew"].size() == 2);
    }

    TEST_CASE("a core install isn't blocked by a tap dependent that inherits it") {
        if (!fs::exists("./den")) {
            MESSAGE("den binary not in cwd — skipping");
            return;
        }
        Fx fx;
        fx.core("helper");
        fx.tap("helper");
        fx.tap("app", {"test/tap/helper"});
        save_index(fx.idx, fx.cfg.cache / "index.json");
        fx.env("/", {"helper"});           // core helper
        fx.env("/work", {"test/tap/app"}); // its tap helper is gone

        auto r = run_den("uninstall helper", fx.tmp.path);
        INFO(r.out);
        CHECK(r.code == 0);
        CHECK(r.out.find("Refusing") == std::string::npos);
        CHECK(read_manifest(fx.cfg.den_home, "/").packages["homebrew"].count("helper") == 0);
        CHECK(read_manifest(fx.cfg.den_home, "/work").packages["homebrew"].size() == 1);
    }
}

} // namespace uninstall_tap_test
} // namespace den

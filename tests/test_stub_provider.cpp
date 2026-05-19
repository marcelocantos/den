// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// End-to-end tests for the stub non-Homebrew provider (🎯T23.6).
//
// These tests exercise the multi-provider seams *outside* the Homebrew
// path — registration, resolution dispatch, manifest read/write under a
// per-provider bucket, env composition via provider->binary_paths, and
// uninstall.  If any of these seams were Homebrew-shaped rather than
// generic, these tests would fail.

#include <doctest.h>

#include "cli/install.h"
#include "core/config.h"
#include "core/error.h"
#include "env/environment.h"
#include "env/manifest.h"
#include "provider/registry.h"
#include "provider/stub_provider.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace den {
namespace test {

namespace fs = std::filesystem;

namespace {

struct TmpDir {
    fs::path path;

    TmpDir() {
        path =
            fs::temp_directory_path() /
            ("den_test_stub_" +
             std::to_string(
                 std::hash<std::string>{}(std::to_string(reinterpret_cast<uintptr_t>(this))) ^
                 static_cast<size_t>(std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::create_directories(path);
    }

    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TmpDir(const TmpDir&) = delete;
    TmpDir& operator=(const TmpDir&) = delete;
};

Config make_config(const fs::path& den_home, const fs::path& store) {
    Config cfg;
    cfg.den_home = den_home;
    cfg.store = store;
    cfg.cache = den_home / "cache";
    return cfg;
}

} // namespace

TEST_SUITE("T23.6::stub_provider") {

    TEST_CASE("provider_name and accepts behaviour") {
        StubProvider p;
        CHECK(p.provider_name() == "stub");
        // Never auto-claim — even names a real provider would route are not
        // automatically routed here.
        CHECK_FALSE(p.accepts("anything"));
        CHECK_FALSE(p.accepts("stub-something"));
        CHECK_FALSE(p.accepts(""));
    }

    TEST_CASE("install creates a runnable script in the stub's storage") {
        TmpDir tmp;
        auto cfg = make_config(tmp.path / "den", tmp.path / "store");
        StubProvider p;

        auto result = p.install(cfg, "demo", "1.2.3");
        CHECK(result.resolved_version == "1.2.3");

        auto script = cfg.den_home / "providers" / "stub" / "demo" / "1.2.3" / "bin" / "demo";
        CHECK(fs::exists(script));
        // Owner-execute bit must be set.
        auto perms = fs::status(script).permissions();
        CHECK((perms & fs::perms::owner_exec) != fs::perms::none);
    }

    TEST_CASE("default version hint resolves to 0.1.0") {
        TmpDir tmp;
        auto cfg = make_config(tmp.path / "den", tmp.path / "store");
        StubProvider p;
        auto result = p.install(cfg, "demo", "");
        CHECK(result.resolved_version == "0.1.0");
    }

    TEST_CASE("list_installed enumerates installed (name, version) pairs") {
        TmpDir tmp;
        auto cfg = make_config(tmp.path / "den", tmp.path / "store");
        StubProvider p;
        p.install(cfg, "alpha", "1.0");
        p.install(cfg, "bravo", "2.0");
        p.install(cfg, "alpha", "1.1");

        auto installed = p.list_installed(cfg);
        REQUIRE(installed.size() == 3);
    }

    TEST_CASE("uninstall removes the package directory") {
        TmpDir tmp;
        auto cfg = make_config(tmp.path / "den", tmp.path / "store");
        StubProvider p;
        p.install(cfg, "demo", "1.0");
        CHECK(p.is_installed(cfg, "demo", "1.0"));
        p.uninstall(cfg, "demo");
        CHECK_FALSE(p.is_installed(cfg, "demo", "1.0"));
    }

    TEST_CASE("package_root layout uses the stub-scoped path") {
        TmpDir tmp;
        auto cfg = make_config(tmp.path / "den", tmp.path / "store");
        StubProvider p;
        auto root = p.package_root(cfg, "demo", "1.0");
        CHECK(root == cfg.den_home / "providers" / "stub" / "demo" / "1.0");
    }

    TEST_CASE("binary_paths returns the bin/ dir when present, empty otherwise") {
        TmpDir tmp;
        auto cfg = make_config(tmp.path / "den", tmp.path / "store");
        StubProvider p;
        p.install(cfg, "demo", "1.0");

        InstalledPackage pkg{"demo", "1.0", p.package_root(cfg, "demo", "1.0")};
        auto paths = p.binary_paths(cfg, pkg);
        REQUIRE(paths.size() == 1);
        CHECK(paths[0].filename() == "bin");

        // After uninstall the bin/ is gone.
        p.uninstall(cfg, "demo");
        InstalledPackage gone{"demo", "1.0", p.package_root(cfg, "demo", "1.0")};
        CHECK(p.binary_paths(cfg, gone).empty());
    }

    TEST_CASE("install with empty name throws UserError") {
        TmpDir tmp;
        auto cfg = make_config(tmp.path / "den", tmp.path / "store");
        StubProvider p;
        CHECK_THROWS_AS(p.install(cfg, "", "1.0"), UserError);
    }
}

TEST_SUITE("T23.6::stub_provider::end_to_end") {

    TEST_CASE("registered in make_default_registry as 'stub'") {
        auto r = make_default_registry(nullptr);
        auto* stub = r.find("stub");
        REQUIRE(stub != nullptr);
        CHECK(stub->provider_name() == "stub");
        // Homebrew remains the default.
        auto* def = r.default_provider();
        REQUIRE(def != nullptr);
        CHECK(def->provider_name() == "homebrew");
    }

    TEST_CASE("explicit --provider stub resolves to the stub provider") {
        auto r = make_default_registry(nullptr);
        Manifest m;
        auto& chosen = resolve_provider(r, "demo", m, "stub");
        CHECK(chosen.provider_name() == "stub");
    }

    TEST_CASE("manifest hint routes to the stub when the entry lives there") {
        auto r = make_default_registry(nullptr);
        Manifest m;
        m.packages["stub"]["demo"] = "1.0";
        auto& chosen = resolve_provider(r, "demo", m, "");
        CHECK(chosen.provider_name() == "stub");
    }

    TEST_CASE("install_packages drives the stub end-to-end") {
        // Exercises the full pipeline outside Homebrew: orchestration
        // (install_packages), provider dispatch (HomebrewProvider would
        // never see this), manifest write under the stub's bucket,
        // env composition via provider->binary_paths + package_root, and
        // the final symlink into env/bin/.
        TmpDir tmp;
        auto cfg = make_config(tmp.path / "den", tmp.path / "store");
        fs::create_directories(cfg.den_home);
        fs::create_directories(cfg.store);

        // The orchestration layer needs a PackageIndex argument but the
        // stub provider doesn't consult it; an empty index is fine.
        PackageIndex empty_idx;
        StubProvider p;
        install_packages(cfg, p, empty_idx, {"demo"});

        // Manifest must have written under the stub's bucket — not homebrew.
        auto manifest = read_manifest(cfg.den_home, "/");
        REQUIRE(manifest.packages.count("stub") == 1);
        CHECK(manifest.packages["stub"]["demo"] == "0.1.0");
        CHECK(manifest.packages.count("homebrew") == 0);

        // Env must have a symlink to the stub's binary.
        auto e_dir = env_dir(cfg.den_home, "/");
        auto link = e_dir / "bin" / "demo";
        REQUIRE(fs::is_symlink(link));
        CHECK(fs::read_symlink(link) ==
              cfg.den_home / "providers" / "stub" / "demo" / "0.1.0" / "bin" / "demo");

        // Uninstall removes the manifest entry and the env symlink.
        uninstall_packages(cfg, p, {"demo"});
        auto after = read_manifest(cfg.den_home, "/");
        CHECK(after.packages["stub"].count("demo") == 0);
        CHECK_FALSE(fs::exists(e_dir / "bin" / "demo"));
    }
}

} // namespace test
} // namespace den

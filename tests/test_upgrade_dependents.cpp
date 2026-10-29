// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// An upgrade moves revision-bumped dependents with their dependency, and a
// batch that rolls back puts back every opt link it moved. The fixtures are
// real binaries: `app` loads libdepa through <prefix>/opt/depa (as a source
// build, and soon a bottle, links it) and prints the version it loaded, so
// each check runs it.

#include <doctest.h>

#include "activity/activity.h"
#include "cli/install.h"
#include "core/error.h"
#include "daemon/daemon.h"
#include "env/environment.h"
#include "env/manifest.h"
#include "env/version_change.h"
#include "index/package.h"
#include "provider/package_provider.h"
#include "store/link.h"
#include "store/pins.h"
#include "store/store.h"

#include <sys/wait.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct DepTmp {
    fs::path path;
    DepTmp() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("den-upgrade-dependents-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~DepTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    DepTmp(const DepTmp&) = delete;
    DepTmp& operator=(const DepTmp&) = delete;
};

Config dep_config(const fs::path& root) {
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

void put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << text;
}

std::string q(const fs::path& p) {
    std::string out = "'";
    for (char c : p.string())
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

std::pair<int, std::string> run(const std::string& cmd) {
    std::string out;
    FILE* p = ::popen((cmd + " 2>&1").c_str(), "r");
    REQUIRE(p != nullptr);
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), buf.size(), p))
        out += buf.data();
    const int status = ::pclose(p);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : 128, out};
}

void cc(const std::string& args) {
    auto [rc, out] = run("cc " + args);
    INFO("cc " << args << "\n" << out);
    REQUIRE(rc == 0);
}

std::string link_text(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_symlink(p, ec))
        return "<not a link>";
    return fs::read_symlink(p, ec).string();
}

#ifdef __APPLE__
std::string lib_file(int soname) {
    return "libdepa." + std::to_string(soname) + ".dylib";
}
#else
std::string lib_file(int soname) {
    return "libdepa.so." + std::to_string(soname);
}
#endif

// Prebuilt kegs, poured by copying into the Cellar.
struct Kegs {
    fs::path root, prefix;

    fs::path keg(const std::string& name, const std::string& version) const {
        return root / name / version;
    }

    // depa <version>: lib/<lib_file(soname)>, whose depa() returns
    // "depa <version>", identified by its opt path.
    void depa(const std::string& version, int soname) const {
        const auto k = keg("depa", version);
        const auto lib = k / "lib" / lib_file(soname);
        put(k / "src.c", "const char* depa(void) { return \"depa " + version + "\"; }\n");
        fs::create_directories(lib.parent_path());
#ifdef __APPLE__
        cc("-dynamiclib -install_name " + q(prefix / "opt" / "depa" / "lib" / lib_file(soname)) +
           " -o " + q(lib) + " " + q(k / "src.c"));
#else
        cc("-shared -fPIC -Wl,-soname," + lib_file(soname) + " -o " + q(lib) + " " +
           q(k / "src.c"));
#endif
        fs::remove(k / "src.c");
    }

    // app <version>: bin/app, linked against depa <dep_version>'s library and
    // loading it through <prefix>/opt/depa.
    void app(const std::string& version, const std::string& dep_version, int soname) const {
        const auto k = keg("app", version);
        const auto lib = keg("depa", dep_version) / "lib" / lib_file(soname);
        put(k / "main.c", "#include <stdio.h>\nconst char* depa(void);\n"
                          "int main(void) { puts(depa()); return 0; }\n");
        fs::create_directories(k / "bin");
#ifdef __APPLE__
        cc("-o " + q(k / "bin" / "app") + " " + q(k / "main.c") + " " + q(lib));
#else
        cc("-o " + q(k / "bin" / "app") + " " + q(k / "main.c") + " " + q(lib) + " -Wl,-rpath," +
           q(prefix / "opt" / "depa" / "lib"));
#endif
        fs::remove(k / "main.c");
    }

    // tool <version>: a plain file, optionally colliding with `stray`.
    void tool(const std::string& version) const {
        put(keg("tool", version) / "bin" / "tool", "tool " + version + "\n");
    }
};

// Pour the way the homebrew provider does: the keg, den's receipt and the
// shared opt link (moved forward from an older den keg).
void pour(const Config& cfg, const Kegs& kegs, const std::string& name,
          const std::string& version) {
    const auto dest = cfg.store / name / version;
    if (fs::exists(dest))
        return;
    fs::create_directories(dest.parent_path());
    fs::copy(kegs.keg(name, version), dest, fs::copy_options::recursive);
    mark_keg_owned(dest, name, version);
    ensure_prefix_opt_link(cfg.store, name, dest);
}

// "homebrew" provider that pours a recipe's dependencies first, as
// HomebrewProvider does, so their opt links move during the batch.
class DepProvider : public PackageProvider {
  public:
    const Config* cfg = nullptr;
    const Kegs* kegs = nullptr;
    // "name@version" -> dependencies poured first, as (name, version).
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> deps;
    std::string fail_after; // "name@version" whose install throws after pouring
    std::vector<std::string> installed_log;

    std::string_view provider_name() const override { return "homebrew"; }
    bool accepts(std::string_view) const override { return true; }
    InstallResult install(const Config& c, std::string_view name,
                          std::string_view version) override {
        const auto key = std::string(name) + "@" + std::string(version);
        installed_log.push_back(key);
        InstallResult r{std::string(version), {}};
        if (auto d = deps.find(key); d != deps.end()) {
            for (const auto& [dn, dv] : d->second) {
                pour(c, *kegs, dn, dv);
                r.auto_deps.push_back(dn);
            }
        }
        pour(c, *kegs, std::string(name), std::string(version));
        if (key == fail_after)
            throw UserError("download failed: curl: (56) LibreSSL SSL_read: bad decrypt");
        return r;
    }
    void uninstall(const Config&, std::string_view) override {}
    std::vector<InstalledPackage> list_installed(const Config& c) const override {
        return ::den::list_installed(c.store);
    }
    fs::path package_root(const Config& c, std::string_view name,
                          std::string_view version) const override {
        return c.store / std::string(name) / std::string(version);
    }
    std::vector<fs::path> binary_paths(const Config&, const InstalledPackage& pkg) const override {
        std::vector<fs::path> out;
        for (const char* d : {"bin", "lib"})
            if (fs::is_directory(pkg.path / d))
                out.push_back(pkg.path / d);
        return out;
    }
};

Package pkg(const std::string& name, const std::string& version, int revision,
            std::vector<std::string> deps = {}) {
    Package p;
    p.name = name;
    p.version = version;
    p.revision = revision;
    p.dependencies = std::move(deps);
    return p;
}

// depa 1.0 (libdepa.1) and app 1.0 on it, installed and linked by den.
struct Fixture {
    DepTmp t;
    Config cfg = dep_config(t.path);
    Kegs kegs{t.path / "kegs", cfg.homebrew_prefix};
    DepProvider prov;
    PackageIndex idx;

    Fixture() {
        kegs.depa("1.0", 1);
        kegs.app("1.0", "1.0", 1);
        prov.cfg = &cfg;
        prov.kegs = &kegs;
        pour(cfg, kegs, "depa", "1.0");
        pour(cfg, kegs, "app", "1.0");
        set_index({pkg("depa", "1.0", 0), pkg("app", "1.0", 0, {"depa"})});
        with_manifest(cfg.den_home, "/", [](Manifest& m) {
            m.packages["homebrew"]["depa"] = "1.0";
            m.packages["homebrew"]["app"] = "1.0";
        });
        materialise(cfg.den_home, cfg.store, "/", &idx);
        REQUIRE(app_says() == "depa 1.0");
        REQUIRE(opt("depa") == "../Cellar/depa/1.0");
    }

    void set_index(const std::vector<Package>& pkgs) {
        idx = PackageIndex{};
        for (const auto& p : pkgs)
            idx.packages[p.name] = p;
    }

    std::string opt(const std::string& name) const {
        return link_text(cfg.homebrew_prefix / "opt" / name);
    }

    std::string manifest(const std::string& name) const {
        auto r = resolve(cfg.den_home, active_env_path(cfg.den_home));
        return r.count(name) ? r.at(name) : "";
    }

    // What the installed app prints (via the prefix's bin link).
    std::string app_says() const {
        auto [rc, out] = run(q(cfg.homebrew_prefix / "bin" / "app"));
        if (rc != 0)
            return "<exit " + std::to_string(rc) + ">";
        while (!out.empty() && out.back() == '\n')
            out.pop_back();
        return out;
    }
};

std::string quiet(const std::function<void()>& fn) {
    std::ostringstream sink;
    auto* old = std::cout.rdbuf(sink.rdbuf());
    std::string err;
    try {
        fn();
    } catch (const UserError& e) {
        err = e.what();
    }
    std::cout.rdbuf(old);
    return sink.str() + (err.empty() ? "" : "\nERROR: " + err);
}

} // namespace

TEST_SUITE("upgrade_dependents") {

    TEST_CASE("den upgrade <dep>: a soname bump upgrades the revision-bumped dependent too") {
        Fixture f;
        f.kegs.depa("2.0", 2); // drops libdepa.1
        f.kegs.app("1.0_1", "2.0", 2);
        f.set_index({pkg("depa", "2.0", 0), pkg("app", "1.0_1", 1, {"depa"})});
        auto out = quiet([&] { upgrade_packages(f.cfg, f.prov, f.idx, {"depa"}); });
        INFO(out);
        CHECK(out.find("ERROR") == std::string::npos);
        CHECK(out.find("Also upgraded 1 dependent(s)") != std::string::npos);
        CHECK(out.find("app 1.0 -> 1.0_1") != std::string::npos);
        CHECK(out.find("2 package(s) upgraded") != std::string::npos);
        CHECK(f.prov.installed_log == std::vector<std::string>{"depa@2.0", "app@1.0_1"});
        CHECK(f.manifest("depa") == "2.0");
        CHECK(f.manifest("app") == "1.0_1");
        CHECK(f.opt("depa") == "../Cellar/depa/2.0");
        CHECK(f.opt("app") == "../Cellar/app/1.0_1");
        CHECK(f.app_says() == "depa 2.0");
        CHECK(read_activity(f.cfg.den_home).size() == 2);
    }

    TEST_CASE("the daemon's upgrade moves the revision-bumped dependent in the same batch") {
        Fixture f;
        f.kegs.depa("2.0", 2);
        f.kegs.app("1.0_1", "2.0", 2);
        f.set_index({pkg("depa", "2.0", 0), pkg("app", "1.0_1", 1, {"depa"})});
        apply_daemon_upgrade(f.cfg, f.prov, f.idx, {"depa", "1.0", "2.0"});
        CHECK(f.manifest("app") == "1.0_1");
        CHECK(f.app_says() == "depa 2.0");
    }

    TEST_CASE("only revision bumps of runtime dependents join the batch") {
        Fixture f;
        f.kegs.depa("1.1", 1);
        SUBCASE("no revision bump") {
            f.set_index({pkg("depa", "1.1", 0), pkg("app", "1.0", 0, {"depa"})});
        }
        SUBCASE("a new upstream version is an upgrade of its own, not a dependent move") {
            f.set_index({pkg("depa", "1.1", 0), pkg("app", "2.0", 0, {"depa"})});
        }
        SUBCASE("a revision bump of something that does not depend on depa") {
            f.set_index({pkg("depa", "1.1", 0), pkg("app", "1.0_1", 1)});
        }
        auto res = apply_version_changes(f.cfg, f.prov, &f.idx, {{"depa", "1.0", "1.1"}}, [] {
            VersionChangeOptions o;
            o.upgrade_dependents = true;
            return o;
        }());
        CHECK(res.dependents.empty());
        CHECK(res.applied.size() == 1);
        CHECK(f.manifest("app") == "1.0");
        CHECK(f.app_says() == "depa 1.1"); // same soname: app follows opt/depa
    }

    TEST_CASE("a pinned dependent is reported and left") {
        Fixture f;
        f.kegs.depa("2.0", 2);
        f.kegs.app("1.0_1", "2.0", 2);
        f.set_index({pkg("depa", "2.0", 0), pkg("app", "1.0_1", 1, {"depa"})});
        write_pins(f.cfg.den_home, {{"app", "1.0"}});
        auto out = quiet([&] { upgrade_packages(f.cfg, f.prov, f.idx, {"depa"}); });
        CHECK(out.find("Not upgrading 1 pinned dependent(s)") != std::string::npos);
        CHECK(out.find("app 1.0 (pinned; 1.0_1 available)") != std::string::npos);
        CHECK(f.manifest("app") == "1.0");
        CHECK(f.prov.installed_log == std::vector<std::string>{"depa@2.0"});
        // The pinned app 1.0 loads libdepa.1, which depa 2.0 drops, so
        // linkage verification holds depa back too rather than break app.
        CHECK(out.find("upgrade of depa 1.0 -> 2.0 was rolled back before switching") !=
              std::string::npos);
        CHECK(f.manifest("depa") == "1.0");
        CHECK(f.app_says() == "depa 1.0");
    }

    // The negative case: a soname bump the index has no rebuilt dependent
    // for. Before A2's linkage verification, the new depa switched in and app
    // failed to load; now the change is rolled back before anything switches.
    TEST_CASE("linkage verification: a soname bump with no rebuilt dependent rolls back") {
        Fixture f;
        f.kegs.depa("2.0", 2); // drops libdepa.1, which app 1.0 loads via opt/depa
        f.set_index({pkg("depa", "2.0", 0), pkg("app", "1.0", 0, {"depa"})});

        SUBCASE("den upgrade") {
            auto out = quiet([&] { upgrade_packages(f.cfg, f.prov, f.idx, {"depa"}); });
            INFO(out);
            CHECK(out.find("upgrade of depa 1.0 -> 2.0 was rolled back before switching: depa "
                           "2.0 no longer has 1 library") != std::string::npos);
            CHECK(out.find("app 1.0: bin/app loads " +
                           (f.cfg.homebrew_prefix / "opt" / "depa" / "lib").string() + "/" +
                           lib_file(1)) != std::string::npos);
            CHECK(
                out.find("depa stays at 1.0 (still linked); the 2.0 keg den poured was removed") !=
                std::string::npos);
            CHECK(f.manifest("depa") == "1.0");
            CHECK(f.opt("depa") == "../Cellar/depa/1.0");
            CHECK_FALSE(fs::exists(f.cfg.store / "depa" / "2.0"));
            CHECK(f.app_says() == "depa 1.0");
            CHECK(read_activity(f.cfg.den_home).empty());
        }
        SUBCASE("the result names the broken loads") {
            VersionChangeOptions opts;
            opts.upgrade_dependents = true;
            auto res = apply_version_changes(f.cfg, f.prov, &f.idx, {{"depa", "1.0", "2.0"}}, opts);
            REQUIRE(res.rolled_back.size() == 1);
            CHECK(res.applied.empty());
            CHECK(res.rolled_back[0].keg_removed);
            REQUIRE(res.rolled_back[0].broken_loads.size() == 1);
            CHECK(res.rolled_back[0].broken_loads[0].starts_with("app 1.0: bin/app loads "));
            CHECK(f.app_says() == "depa 1.0");
        }
        SUBCASE("without verification the switch breaks app") {
            VersionChangeOptions opts;
            opts.verify_linkage = false;
            auto res = apply_version_changes(f.cfg, f.prov, &f.idx, {{"depa", "1.0", "2.0"}}, opts);
            CHECK(res.rolled_back.empty());
            CHECK(f.manifest("depa") == "2.0");
            CHECK(f.app_says() != "depa 1.0");
        }
    }

    TEST_CASE("linkage verification passes a compatible upgrade") {
        Fixture f;
        f.kegs.depa("1.1", 1); // same soname
        f.set_index({pkg("depa", "1.1", 0), pkg("app", "1.0", 0, {"depa"})});
        auto out = quiet([&] { upgrade_packages(f.cfg, f.prov, f.idx, {"depa"}); });
        INFO(out);
        CHECK(out.find("ERROR") == std::string::npos);
        CHECK(f.manifest("depa") == "1.1");
        CHECK(f.opt("depa") == "../Cellar/depa/1.1");
        CHECK(f.app_says() == "depa 1.1");
    }

    TEST_CASE("a dependency that rolls back takes its dependent with it") {
        Fixture f;
        f.kegs.depa("2.0", 2);
        f.kegs.app("1.0_1", "2.0", 2);
        f.set_index({pkg("depa", "2.0", 0), pkg("app", "1.0_1", 1, {"depa"})});
        // Something den does not manage already owns depa 2.0's library path.
        put(f.cfg.homebrew_prefix / "lib" / lib_file(2), "stray\n");
        auto out = quiet([&] { upgrade_packages(f.cfg, f.prov, f.idx, {"depa"}); });
        INFO(out);
        CHECK(out.find("upgrade of depa 1.0 -> 2.0 was rolled back: linking 2.0 would clobber") !=
              std::string::npos);
        CHECK(out.find("upgrade of app 1.0 -> 1.0_1 was rolled back with depa") !=
              std::string::npos);
        CHECK(out.find("app stays at 1.0 (still linked); the 1.0_1 keg den poured was removed") !=
              std::string::npos);
        CHECK(f.manifest("depa") == "1.0");
        CHECK(f.manifest("app") == "1.0");
        CHECK(f.opt("depa") == "../Cellar/depa/1.0");
        CHECK(f.opt("app") == "../Cellar/app/1.0");
        CHECK_FALSE(fs::exists(f.cfg.store / "depa" / "2.0"));
        CHECK_FALSE(fs::exists(f.cfg.store / "app" / "1.0_1"));
        CHECK(f.app_says() == "depa 1.0");
        CHECK(read_activity(f.cfg.den_home).empty());
    }

    TEST_CASE("a rollback points a dependency's opt link back, so dependents load the old one") {
        Fixture f;
        f.kegs.depa("1.1", 1); // same soname: app would load it through opt/depa
        f.kegs.tool("1.0");
        f.kegs.tool("2.0");
        pour(f.cfg, f.kegs, "tool", "1.0");
        with_manifest(f.cfg.den_home, "/",
                      [](Manifest& m) { m.packages["homebrew"]["tool"] = "1.0"; });
        materialise(f.cfg.den_home, f.cfg.store, "/", &f.idx);
        f.set_index({pkg("depa", "1.1", 0), pkg("app", "1.0", 0, {"depa"}),
                     pkg("tool", "2.0", 0, {"depa"})});
        // tool 2.0 brings depa 1.1, which moves opt/depa forward.
        f.prov.deps["tool@2.0"] = {{"depa", "1.1"}};

        SUBCASE("tool 2.0 cannot be linked") {
            fs::remove(f.cfg.homebrew_prefix / "bin" / "tool");
            put(f.cfg.homebrew_prefix / "bin" / "tool", "not den's\n");
            auto res = apply_version_changes(f.cfg, f.prov, &f.idx, {{"tool", "1.0", "2.0"}});
            REQUIRE(res.rolled_back.size() == 1);
            CHECK(f.manifest("tool") == "1.0");
        }
        SUBCASE("tool 2.0's install fails after its dependency was poured") {
            f.prov.fail_after = "tool@2.0";
            CHECK_THROWS_AS(apply_version_changes(f.cfg, f.prov, &f.idx, {{"tool", "1.0", "2.0"}}),
                            UserError);
            CHECK(f.manifest("tool") == "1.0");
        }
        CHECK(f.opt("depa") == "../Cellar/depa/1.0");
        CHECK(f.app_says() == "depa 1.0");
        // The dependency keg itself is left for `den cleanup`, as before.
        CHECK(fs::exists(f.cfg.store / "depa" / "1.1"));
    }

    TEST_CASE("a dependency opt link stays moved while a change that applied needs it") {
        Fixture f;
        f.kegs.depa("1.1", 1);
        f.kegs.tool("1.0");
        f.kegs.tool("2.0");
        put(f.kegs.keg("other", "1.0") / "bin" / "other", "other 1.0\n");
        put(f.kegs.keg("other", "2.0") / "bin" / "other", "other 2.0\n");
        pour(f.cfg, f.kegs, "tool", "1.0");
        pour(f.cfg, f.kegs, "other", "1.0");
        with_manifest(f.cfg.den_home, "/", [](Manifest& m) {
            m.packages["homebrew"]["tool"] = "1.0";
            m.packages["homebrew"]["other"] = "1.0";
        });
        materialise(f.cfg.den_home, f.cfg.store, "/", &f.idx);
        f.set_index({pkg("depa", "1.1", 0), pkg("app", "1.0", 0, {"depa"}),
                     pkg("tool", "2.0", 0, {"depa"}), pkg("other", "2.0", 0, {"depa"})});
        f.prov.deps["tool@2.0"] = {{"depa", "1.1"}};
        f.prov.deps["other@2.0"] = {{"depa", "1.1"}};
        fs::remove(f.cfg.homebrew_prefix / "bin" / "tool");
        put(f.cfg.homebrew_prefix / "bin" / "tool", "not den's\n");
        auto res = apply_version_changes(f.cfg, f.prov, &f.idx,
                                         {{"tool", "1.0", "2.0"}, {"other", "1.0", "2.0"}});
        REQUIRE(res.rolled_back.size() == 1);
        CHECK(res.rolled_back[0].change.name == "tool");
        REQUIRE(res.applied.size() == 1);
        CHECK(f.manifest("other") == "2.0");
        CHECK(f.opt("depa") == "../Cellar/depa/1.1");
        CHECK(f.opt("tool") == "../Cellar/tool/1.0");
        CHECK(f.app_says() == "depa 1.1");
    }
}

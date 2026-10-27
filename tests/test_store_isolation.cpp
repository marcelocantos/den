// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Store isolation: DEN_HOME alone gives den its own prefix and Cellar.
//
// `brew shellenv` exports HOMEBREW_PREFIX / HOMEBREW_CELLAR in every shell,
// so these tests deliberately set them (to a *fake* Homebrew prefix in a
// temp dir, never the real /opt/homebrew) and prove den ignores them when
// choosing where to install. Precedence (see Config::detect):
//   DEN_PREFIX > config.json store.prefix > DEN_HOME/prefix (if populated)
//   > legacy shared Cellar > DEN_HOME/prefix.

#include <doctest.h>

#include "core/config.h"
#include "core/error.h"
#include "download/sha256.h"
#include "migrate/migrate.h"
#include "settings/settings.h"

#include <archive.h>
#include <archive_entry.h>

#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct IsoTmp {
    fs::path path;
    IsoTmp() {
        auto tmpl = (fs::temp_directory_path() / "den-iso-XXXXXX").string();
        char* p = ::mkdtemp(tmpl.data());
        REQUIRE(p != nullptr);
        // Canonical so comparisons survive /tmp -> /private/tmp on macOS.
        path = fs::canonical(p);
    }
    ~IsoTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    IsoTmp(const IsoTmp&) = delete;
    IsoTmp& operator=(const IsoTmp&) = delete;
};

// Set or unset an env var for the scope of a test; restores on exit.
struct EnvVar {
    std::string name;
    std::optional<std::string> old;
    EnvVar(std::string n, std::optional<std::string> value) : name(std::move(n)) {
        if (const char* v = std::getenv(name.c_str())) {
            old = v;
        }
        if (value) {
            ::setenv(name.c_str(), value->c_str(), 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }
    ~EnvVar() {
        if (old) {
            ::setenv(name.c_str(), old->c_str(), 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }
    EnvVar(const EnvVar&) = delete;
    EnvVar& operator=(const EnvVar&) = delete;
};

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << content;
}

// A fake "default Homebrew prefix": Cellar with a keg, Caskroom, opt link.
fs::path make_fake_brew(const fs::path& root) {
    const auto brew = root / "fakebrew";
    write_file(brew / "Cellar" / "brewpkg" / "1.0" / "bin" / "brewpkg", "#!/bin/sh\n");
    write_file(brew / "Caskroom" / "brewcask" / "2.0" / ".keep", "");
    fs::create_directories(brew / "opt");
    fs::create_directory_symlink("../Cellar/brewpkg/1.0", brew / "opt" / "brewpkg");
    return brew;
}

// Recursive (path -> size:mtime) snapshot, symlinks not followed.
std::map<std::string, std::string> snapshot(const fs::path& root) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto& p = it->path();
        auto st = fs::symlink_status(p, ec);
        std::string sig = std::to_string(static_cast<int>(st.type()));
        if (fs::is_regular_file(st)) {
            sig += ":" + std::to_string(static_cast<unsigned long long>(fs::file_size(p, ec)));
        }
        auto t = fs::last_write_time(p, ec);
        sig += ":" + std::to_string(static_cast<long long>(t.time_since_epoch().count()));
        out[fs::relative(p, root).string()] = sig;
    }
    out["."] = std::to_string(
        static_cast<long long>(fs::last_write_time(root, ec).time_since_epoch().count()));
    return out;
}

struct Run {
    int code;
    std::string out;
};

Run run(const std::string& cmd) {
    FILE* pipe = ::popen((cmd + " 2>&1").c_str(), "r");
    REQUIRE(pipe != nullptr);
    std::array<char, 4096> buf{};
    std::string out;
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        out += buf.data();
    }
    int st = ::pclose(pipe);
    return {WIFEXITED(st) ? WEXITSTATUS(st) : -1, out};
}

// Build <name>/<ver>/bin/<name> as a gzipped tar bottle. The script embeds
// @@HOMEBREW_PREFIX@@ so relocation must rewrite it to den's prefix.
fs::path make_bottle(const fs::path& dir, const std::string& name, const std::string& ver) {
    const auto path = dir / (name + "-" + ver + ".tar.gz");
    const std::string body =
        "#!/bin/sh\necho \"" + name + " " + ver + " @@HOMEBREW_PREFIX@@/share/" + name + "\"\n";
    struct archive* a = archive_write_new();
    archive_write_add_filter_gzip(a);
    archive_write_set_format_pax_restricted(a);
    REQUIRE(archive_write_open_filename(a, path.c_str()) == ARCHIVE_OK);
    auto add_dir = [&](const std::string& rel) {
        struct archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, rel.c_str());
        archive_entry_set_filetype(e, AE_IFDIR);
        archive_entry_set_perm(e, 0755);
        archive_write_header(a, e);
        archive_entry_free(e);
    };
    add_dir(name);
    add_dir(name + "/" + ver);
    add_dir(name + "/" + ver + "/bin");
    struct archive_entry* e = archive_entry_new();
    const std::string rel = name + "/" + ver + "/bin/" + name;
    archive_entry_set_pathname(e, rel.c_str());
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, 0755);
    archive_entry_set_size(e, static_cast<la_int64_t>(body.size()));
    archive_write_header(a, e);
    archive_write_data(a, body.data(), body.size());
    archive_entry_free(e);
    archive_write_close(a);
    archive_write_free(a);
    return path;
}

// Seed index.json + the content-addressed archive cache so `den install`
// runs fully offline.
void seed_offline_package(const fs::path& den_home, const std::string& name,
                          const std::string& ver) {
    IsoTmp staging;
    const auto bottle = make_bottle(staging.path, name, ver);
    const auto sha = hash_file(bottle);
    const auto archives = den_home / "cache" / "archives";
    fs::create_directories(archives);
    fs::copy_file(bottle, archives / (sha + ".tar.gz"));
    write_file(den_home / "cache" / "index.json", R"({
  "packages": [{
    "name": ")" + name + R"(",
    "version": ")" + ver + R"(",
    "description": "store isolation fixture",
    "homepage": "",
    "license": "",
    "artifact_type": "binary",
    "deprecated": false,
    "disabled": false,
    "dependencies": [],
    "build_dependencies": [],
    "archives": {
      "all": {
        "url": "file:///nonexistent/)" + name + "-" + ver +
                                                      R"(.tar.gz",
        "sha256": ")" + sha + R"(",
        "relocation": "needs_substitution"
      }
    }
  }]
})");
}

std::string read_all(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

TEST_SUITE("store_isolation") {

    TEST_CASE("DEN_HOME alone selects DEN_HOME/prefix even with brew shellenv vars set") {
        IsoTmp tmp;
        const auto brew = make_fake_brew(tmp.path);
        const auto home = tmp.path / "den";
        EnvVar h("DEN_HOME", home.string());
        EnvVar dp("DEN_PREFIX", std::nullopt);
        EnvVar bp("HOMEBREW_PREFIX", brew.string());
        EnvVar bc("HOMEBREW_CELLAR", (brew / "Cellar").string());

        const auto cfg = Config::detect();
        CHECK(cfg.prefix == home / "prefix");
        CHECK(cfg.store == home / "prefix" / "Cellar");
        CHECK(cfg.prefix_source == PrefixSource::DenHome);
        CHECK_FALSE(cfg.shares_homebrew_cellar());
        // Homebrew's location is still known (for migrate), just not used.
        CHECK(cfg.homebrew_prefix == brew);
        CHECK(cfg.homebrew_cellar == brew / "Cellar");
    }

    TEST_CASE("DEN_PREFIX beats config.json store.prefix") {
        IsoTmp tmp;
        const auto home = tmp.path / "den";
        Settings s;
        s.store.prefix = (tmp.path / "from-config").string();
        write_settings(home, s);
        EnvVar h("DEN_HOME", home.string());
        EnvVar dp("DEN_PREFIX", (tmp.path / "from-env").string());

        const auto cfg = Config::detect();
        CHECK(cfg.prefix == tmp.path / "from-env");
        CHECK(cfg.store == tmp.path / "from-env" / "Cellar");
        CHECK(cfg.prefix_source == PrefixSource::EnvDenPrefix);
    }

    TEST_CASE("config.json store.prefix is used when DEN_PREFIX is unset and round-trips") {
        IsoTmp tmp;
        const auto home = tmp.path / "den";
        Settings s;
        s.store.prefix = (tmp.path / "from-config").string();
        write_settings(home, s);
        // Another settings writer must not drop the key.
        Settings again = read_settings(home);
        again.casks["x"] = "1";
        write_settings(home, again);
        EnvVar h("DEN_HOME", home.string());
        EnvVar dp("DEN_PREFIX", std::nullopt);

        const auto cfg = Config::detect();
        CHECK(cfg.prefix == tmp.path / "from-config");
        CHECK(cfg.prefix_source == PrefixSource::Settings);
        CHECK(get_setting(home, "store.prefix") == (tmp.path / "from-config").string());
    }

    TEST_CASE("pre-isolation den home sharing brew's Cellar keeps using it (read-only)") {
        IsoTmp tmp;
        const auto brew = make_fake_brew(tmp.path);
        const auto home = tmp.path / "den";
        write_file(home / "manifests" / "ROOT" / "manifest.json",
                   R"({"packages":{"homebrew":{"brewpkg":"1.0"}},"auto_deps":{}})");
        EnvVar h("DEN_HOME", home.string());
        EnvVar dp("DEN_PREFIX", std::nullopt);
        EnvVar bp("HOMEBREW_PREFIX", brew.string());
        EnvVar bc("HOMEBREW_CELLAR", (brew / "Cellar").string());

        const auto before = snapshot(home);
        const auto cfg = Config::detect();
        CHECK(cfg.prefix_source == PrefixSource::LegacyShared);
        CHECK(cfg.store == brew / "Cellar");
        CHECK(cfg.shares_homebrew_cellar());
        CHECK(snapshot(home) == before); // detection writes nothing

        SUBCASE("a populated private prefix wins over legacy detection") {
            fs::create_directories(home / "prefix" / "Cellar");
            const auto c2 = Config::detect();
            CHECK(c2.prefix_source == PrefixSource::DenHome);
            CHECK(c2.store == home / "prefix" / "Cellar");
        }
        SUBCASE("brew upgraded the keg underneath den: still legacy") {
            write_file(home / "manifests" / "ROOT" / "manifest.json",
                       R"({"packages":{"homebrew":{"brewpkg":"0.9"}},"auto_deps":{}})");
            const auto c2 = Config::detect();
            CHECK(c2.prefix_source == PrefixSource::LegacyShared);
        }
        SUBCASE("a manifest whose packages brew's Cellar lacks is not legacy") {
            write_file(home / "manifests" / "ROOT" / "manifest.json",
                       R"({"packages":{"homebrew":{"notinbrew":"1.0"}},"auto_deps":{}})");
            const auto c2 = Config::detect();
            CHECK(c2.prefix_source == PrefixSource::DenHome);
        }
    }

    // The headline regression: DEN_HOME alone (with brew shellenv vars
    // pointing at a fake default prefix) installs, lists, cleans up and
    // uninstalls without touching that prefix at all.
    TEST_CASE("DEN_HOME-only install/list/cleanup/uninstall leaves the default prefix unchanged") {
        IsoTmp tmp;
        const auto brew = make_fake_brew(tmp.path);
        const auto home = tmp.path / "den";
        const std::string pkg = "den-iso-hello";
        seed_offline_package(home, pkg, "1.0");

        const auto before = snapshot(brew);
        const std::string env = "env -u DEN_PREFIX -u HOMEBREW_CASKROOM DEN_HOME=" + home.string() +
                                " HOMEBREW_PREFIX=" + brew.string() +
                                " HOMEBREW_CELLAR=" + brew.string() + "/Cellar ./den ";

        const auto cfgout = run(env + "config");
        CHECK(cfgout.out.find("prefix:            " + (home / "prefix").string()) !=
              std::string::npos);
        CHECK(cfgout.out.find("store:             " + (home / "prefix" / "Cellar").string()) !=
              std::string::npos);
        CHECK(cfgout.out.find("homebrew_prefix:   " + brew.string()) != std::string::npos);

        const auto inst = run(env + "install " + pkg);
        INFO(inst.out);
        REQUIRE(inst.code == 0);

        const auto keg = home / "prefix" / "Cellar" / pkg / "1.0";
        REQUIRE(fs::is_regular_file(keg / "bin" / pkg));
        const auto script = read_all(keg / "bin" / pkg);
        CHECK(script.find("@@HOMEBREW_PREFIX@@") == std::string::npos);
        CHECK(script.find((home / "prefix").string() + "/share/" + pkg) != std::string::npos);
        CHECK(script.find(brew.string()) == std::string::npos);

        const auto list = run(env + "list");
        CHECK(list.code == 0);
        CHECK(list.out.find(pkg) != std::string::npos);
        CHECK(list.out.find("brewpkg") == std::string::npos);

        CHECK(run(env + "cleanup").code == 0);
        const auto un = run(env + "uninstall " + pkg);
        INFO(un.out);
        CHECK(un.code == 0);
        CHECK(run(env + "cleanup").code == 0);

        CHECK(snapshot(brew) == before);
        CHECK(fs::is_regular_file(brew / "Cellar" / "brewpkg" / "1.0" / "bin" / "brewpkg"));
    }

    TEST_CASE("migrate pins store.prefix to Homebrew's prefix for a fresh den home") {
        IsoTmp tmp;
        const auto brew = make_fake_brew(tmp.path);
        const auto home = tmp.path / "den";
        EnvVar h("DEN_HOME", home.string());
        EnvVar dp("DEN_PREFIX", std::nullopt);
        EnvVar bp("HOMEBREW_PREFIX", brew.string());
        EnvVar bc("HOMEBREW_CELLAR", (brew / "Cellar").string());
        EnvVar bk("HOMEBREW_CASKROOM", (tmp.path / "no-caskroom").string());

        auto cfg = Config::detect();
        REQUIRE(cfg.prefix_source == PrefixSource::DenHome);

        MigrateOptions dry;
        dry.dry_run = true;
        migrate_from_homebrew(cfg, {}, dry);
        CHECK_FALSE(read_settings(home).store.prefix.has_value());
        CHECK_FALSE(fs::exists(home / "config.json"));

        const auto brew_before = snapshot(brew);
        migrate_from_homebrew(cfg, {});
        CHECK(read_settings(home).store.prefix == brew.string());
        CHECK(snapshot(brew) == brew_before); // metadata-only

        const auto after = Config::detect();
        CHECK(after.prefix_source == PrefixSource::Settings);
        CHECK(after.store == brew / "Cellar");
        CHECK(after.shares_homebrew_cellar());
    }

    TEST_CASE("migrate refuses when den already owns kegs in its private store") {
        IsoTmp tmp;
        const auto brew = make_fake_brew(tmp.path);
        const auto home = tmp.path / "den";
        write_file(home / "prefix" / "Cellar" / "mine" / "1.0" / "bin" / "mine", "x");
        EnvVar h("DEN_HOME", home.string());
        EnvVar dp("DEN_PREFIX", std::nullopt);
        EnvVar bp("HOMEBREW_PREFIX", brew.string());
        EnvVar bc("HOMEBREW_CELLAR", (brew / "Cellar").string());
        EnvVar bk("HOMEBREW_CASKROOM", (tmp.path / "no-caskroom").string());

        auto cfg = Config::detect();
        CHECK_THROWS_AS(migrate_from_homebrew(cfg, {}), UserError);
        CHECK_FALSE(read_settings(home).store.prefix.has_value());
        CHECK_FALSE(fs::exists(home / "manifests" / "ROOT" / "manifest.json"));
    }

    TEST_CASE("migrate refuses when DEN_PREFIX points somewhere other than Homebrew") {
        IsoTmp tmp;
        const auto brew = make_fake_brew(tmp.path);
        const auto home = tmp.path / "den";
        EnvVar h("DEN_HOME", home.string());
        EnvVar dp("DEN_PREFIX", (tmp.path / "elsewhere").string());
        EnvVar bp("HOMEBREW_PREFIX", brew.string());
        EnvVar bc("HOMEBREW_CELLAR", (brew / "Cellar").string());
        EnvVar bk("HOMEBREW_CASKROOM", (tmp.path / "no-caskroom").string());

        auto cfg = Config::detect();
        CHECK_THROWS_AS(migrate_from_homebrew(cfg, {}), UserError);

        SUBCASE("DEN_PREFIX naming Homebrew's prefix is an explicit go-ahead") {
            EnvVar dp2("DEN_PREFIX", brew.string());
            auto c2 = Config::detect();
            CHECK_NOTHROW(migrate_from_homebrew(c2, {}));
        }
    }
}

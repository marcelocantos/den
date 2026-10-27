// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Regression: cask install/list/uninstall must place apps only where den owns
// them, refuse to overwrite foreign apps, never touch Homebrew's Caskroom,
// and accept the synthetic "default" archive tag.

#include <doctest.h>

#include "cask/cask.h"
#include "core/config.h"
#include "core/error.h"
#include "index/index.h"
#include "index/index_cache.h"
#include "index/package.h"
#include "platform/platform.h"
#include "settings/settings.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <archive.h>
#include <archive_entry.h>
#endif

namespace den {
namespace test {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct TmpDir {
    fs::path path;
    TmpDir() {
        path =
            fs::temp_directory_path() /
            ("den_test_cask_" +
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

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    out << body;
}

Config make_cfg(const fs::path& root) {
    Config c;
    c.den_home = root / "den";
    c.cache = c.den_home / "cache";
    c.store = root / "cellar";
    c.homebrew_prefix = root / "opt";
    c.homebrew_cellar = c.store;
    c.homebrew_caskroom = root / "homebrew-caskroom"; // foreign; must stay empty
    c.appdir = root / "Applications";
    c.arch = Arch::Arm64;
    c.macos_version = MacOsVersion{15, 0};
    fs::create_directories(c.den_home);
    fs::create_directories(c.cache);
    fs::create_directories(c.store);
    fs::create_directories(c.homebrew_caskroom);
    fs::create_directories(c.appdir);
    // Active env so binary links have a home.
    fs::create_directories(c.den_home / "envs" / "ROOT" / "bin");
    write_file(c.den_home / "active_env", "/\n");
    return c;
}

void write_receipt_file(const fs::path& version_dir, const std::string& token,
                        const std::string& version, const std::vector<fs::path>& apps) {
    fs::create_directories(version_dir);
    json j;
    j["poured_by"] = "den";
    j["token"] = token;
    j["version"] = version;
    j["quarantine_cleared"] = true;
    j["apps"] = json::array();
    for (const auto& a : apps) {
        j["apps"].push_back(a.string());
    }
    j["binaries"] = json::array();
    j["pkgs"] = json::array();
    write_file(version_dir / kDenCaskReceiptFilename, j.dump(2) + "\n");
}

#ifdef __APPLE__
/// Build a minimal zip containing a fake .app bundle and optional binary.
fs::path make_app_zip(const fs::path& dir, const std::string& app_name,
                      const std::string& bin_name = {}) {
    const fs::path staging = dir / "content";
    const fs::path app = staging / app_name;
    fs::create_directories(app / "Contents" / "MacOS");
    write_file(app / "Contents" / "Info.plist", "<?xml version=\"1.0\"?><plist></plist>\n");
    write_file(app / "Contents" / "MacOS" / "AppBin", "#!/bin/sh\necho ok\n");
    fs::permissions(app / "Contents" / "MacOS" / "AppBin", fs::perms::owner_all);

    if (!bin_name.empty()) {
        write_file(staging / bin_name, "#!/bin/sh\necho bin\n");
        fs::permissions(staging / bin_name, fs::perms::owner_all);
    }

    const fs::path zip_path = dir / "payload.zip";
    struct archive* a = archive_write_new();
    archive_write_set_format_zip(a);
    archive_write_open_filename(a, zip_path.c_str());

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(staging, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        const auto rel = fs::relative(it->path(), staging).string();
        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, rel.c_str());
        if (it->is_directory()) {
            archive_entry_set_filetype(entry, AE_IFDIR);
            archive_entry_set_perm(entry, 0755);
            archive_write_header(a, entry);
        } else {
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0755);
            auto sz = fs::file_size(it->path());
            archive_entry_set_size(entry, static_cast<la_int64_t>(sz));
            archive_write_header(a, entry);
            std::ifstream in(it->path(), std::ios::binary);
            std::string data((std::istreambuf_iterator<char>(in)), {});
            archive_write_data(a, data.data(), data.size());
        }
        archive_entry_free(entry);
    }
    archive_write_close(a);
    archive_write_free(a);
    return zip_path;
}

Package make_zip_cask(const std::string& token, const std::string& version,
                      const fs::path& zip_path, const std::string& app_name,
                      const std::string& bin_name = {}) {
    Package pkg;
    pkg.name = token;
    pkg.version = version;
    pkg.artifact_type = ArtifactType::App;
    Package::Archive ar;
    ar.url = "file://" + zip_path.string();
    ar.sha256.clear(); // no_check — local fixture
    ar.relocation = RelocationType::Portable;
    pkg.archives["default"] = std::move(ar);

    CaskArtifact app;
    app.kind = CaskArtifactKind::App;
    app.source = app_name;
    pkg.cask_artifacts.push_back(app);

    if (!bin_name.empty()) {
        CaskArtifact bin;
        bin.kind = CaskArtifactKind::Binary;
        bin.source = bin_name;
        bin.target = bin_name;
        pkg.cask_artifacts.push_back(bin);
    }
    return pkg;
}
#endif

} // namespace

TEST_SUITE("cask::platform") {

    TEST_CASE("best_archive_tag accepts the cask default tag") {
        const auto tag = best_archive_tag(Arch::Arm64, MacOsVersion{15, 0}, {"default"});
        REQUIRE(tag);
        CHECK(*tag == "default");
    }

    TEST_CASE("best_archive_tag prefers a native tag over default") {
        const auto tag =
            best_archive_tag(Arch::Arm64, MacOsVersion{15, 0}, {"default", "arm64_sequoia"});
        REQUIRE(tag);
        CHECK(*tag == "arm64_sequoia");
    }
}

TEST_SUITE("cask::ownership") {

    TEST_CASE("read_cask_receipt accepts a well-formed den receipt") {
        TmpDir tmp;
        auto cfg = make_cfg(tmp.path);
        const auto ver = cask_version_path(cfg, "demo", "1.0.0");
        const auto app = cfg.appdir / "Demo.app";
        write_receipt_file(ver, "demo", "1.0.0", {app});

        auto r = read_cask_receipt(ver);
        REQUIRE(r);
        CHECK(r->token == "demo");
        CHECK(r->version == "1.0.0");
        REQUIRE(r->apps.size() == 1);
        CHECK(r->apps[0] == app);
        CHECK(app_owned_by_den(cfg, app));
    }

    TEST_CASE("foreign receipt or mismatched token is not ownership") {
        TmpDir tmp;
        auto cfg = make_cfg(tmp.path);
        const auto ver = cask_version_path(cfg, "demo", "1.0.0");
        fs::create_directories(ver);
        // poured_by is not den.
        write_file(ver / kDenCaskReceiptFilename,
                   R"({"poured_by":"brew","token":"demo","version":"1.0.0","apps":[]})");
        CHECK_FALSE(read_cask_receipt(ver));

        // Token does not match directory.
        write_receipt_file(ver, "other", "1.0.0", {cfg.appdir / "X.app"});
        CHECK_FALSE(read_cask_receipt(ver));
        CHECK_FALSE(app_owned_by_den(cfg, cfg.appdir / "X.app"));
    }

    TEST_CASE("Homebrew Caskroom is never used as den staging") {
        TmpDir tmp;
        auto cfg = make_cfg(tmp.path);
        CHECK(den_caskroom(cfg) == cfg.den_home / "caskroom");
        CHECK(den_caskroom(cfg) != cfg.homebrew_caskroom);
    }
}

#ifdef __APPLE__

TEST_SUITE("cask::install_zip") {

    TEST_CASE("install zip app + binary, list, uninstall; foreign app refused") {
        TmpDir tmp;
        auto cfg = make_cfg(tmp.path);
        setenv("DEN_APPDIR", cfg.appdir.c_str(), 1);
        setenv("DEN_HOME", cfg.den_home.c_str(), 1);

        // Foreign app already present — den must refuse to overwrite it.
        const fs::path foreign = cfg.appdir / "Foreign.app";
        fs::create_directories(foreign / "Contents");
        write_file(foreign / "Contents" / "marker", "not-den\n");

        {
            auto zip = make_app_zip(tmp.path / "zip1", "Foreign.app");
            auto pkg = make_zip_cask("foreign-cask", "1.0", zip, "Foreign.app");
            CHECK_THROWS_AS(install_cask(cfg, pkg), UserError);
            CHECK(fs::exists(foreign / "Contents" / "marker"));
            // Homebrew Caskroom must remain untouched.
            CHECK(fs::is_empty(cfg.homebrew_caskroom));
        }

        // Happy path: install a free app + binary.
        {
            auto zip = make_app_zip(tmp.path / "zip2", "Demo.app", "democtl");
            auto pkg = make_zip_cask("demo", "2.1.0", zip, "Demo.app", "democtl");
            auto result = install_cask(cfg, pkg);
            CHECK(result.resolved_version == "2.1.0");

            CHECK(fs::exists(cfg.appdir / "Demo.app" / "Contents" / "MacOS" / "AppBin"));
            CHECK(is_cask_installed(cfg, "demo"));
            CHECK(installed_cask_version(cfg, "demo") == "2.1.0");

            auto listed = list_den_casks(cfg);
            REQUIRE(listed.size() == 1);
            CHECK(listed[0].name == "demo");
            CHECK(listed[0].version == "2.1.0");

            // Settings recorded.
            auto settings = read_settings(cfg.den_home);
            CHECK(settings.casks.at("demo") == "2.1.0");

            // Binary linked into active env bin.
            const auto link = cfg.den_home / "envs" / "ROOT" / "bin" / "democtl";
            CHECK(fs::is_symlink(link));

            // Receipt marks ownership.
            CHECK(app_owned_by_den(cfg, cfg.appdir / "Demo.app"));

            // Homebrew Caskroom still empty.
            CHECK(fs::is_empty(cfg.homebrew_caskroom));

            uninstall_cask(cfg, "demo");
            CHECK_FALSE(fs::exists(cfg.appdir / "Demo.app"));
            CHECK_FALSE(is_cask_installed(cfg, "demo"));
            CHECK_FALSE(fs::exists(link));
            // Foreign app still intact.
            CHECK(fs::exists(foreign / "Contents" / "marker"));
            CHECK(fs::is_empty(cfg.homebrew_caskroom));
        }

        unsetenv("DEN_APPDIR");
        unsetenv("DEN_HOME");
    }
}

namespace {

/// Build a zip whose Demo.app (plus a democtl binary) carries a version marker.
fs::path make_versioned_zip(const fs::path& dir, const std::string& marker) {
    const fs::path pre = dir / "content" / "Demo.app" / "Contents";
    write_file(pre / "VERSION", marker + "\n");
    return make_app_zip(dir, "Demo.app", "democtl");
}

std::string read_marker(const fs::path& app) {
    std::ifstream in(app / "Contents" / "VERSION");
    std::string s;
    std::getline(in, s);
    return s;
}

struct UpgradeEnv {
    TmpDir tmp;
    Config cfg;
    UpgradeEnv() : cfg(make_cfg(tmp.path)) {
        setenv("DEN_APPDIR", cfg.appdir.c_str(), 1);
        setenv("DEN_HOME", cfg.den_home.c_str(), 1);
        unsetenv("DEN_CASK_FAULT");
        auto z1 = make_versioned_zip(tmp.path / "v1", "v1");
        install_cask(cfg, make_zip_cask("demo", "1.0", z1, "Demo.app", "democtl"));
    }
    ~UpgradeEnv() {
        unsetenv("DEN_APPDIR");
        unsetenv("DEN_HOME");
        unsetenv("DEN_CASK_FAULT");
    }
    UpgradeEnv(const UpgradeEnv&) = delete;
    UpgradeEnv& operator=(const UpgradeEnv&) = delete;

    fs::path app() const { return cfg.appdir / "Demo.app"; }
    fs::path link() const { return cfg.den_home / "envs" / "ROOT" / "bin" / "democtl"; }

    /// v1 must be fully intact: app content, receipt, settings, binary link,
    /// and no leftover v2 state or staging debris in the appdir.
    void check_v1_intact() const {
        CHECK(fs::exists(app() / "Contents" / "MacOS" / "AppBin"));
        CHECK(read_marker(app()) == "v1");
        CHECK(installed_cask_version(cfg, "demo") == "1.0");
        CHECK(read_cask_receipt(cask_version_path(cfg, "demo", "1.0")));
        CHECK(app_owned_by_den(cfg, app()));
        CHECK_FALSE(fs::exists(cask_version_path(cfg, "demo", "2.0")));
        auto settings = read_settings(cfg.den_home);
        CHECK(settings.casks.count("demo") == 1);
        if (settings.casks.count("demo") == 1) {
            CHECK(settings.casks.at("demo") == "1.0");
        }
        CHECK(fs::is_symlink(link()));
        CHECK(fs::exists(link())); // still resolves into the v1 stage
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(cfg.appdir, ec)) {
            CHECK_MESSAGE(e.path().filename() == "Demo.app",
                          "unexpected appdir entry: " << e.path().string());
        }
        CHECK(fs::is_empty(cfg.homebrew_caskroom));
    }
};

} // namespace

TEST_SUITE("cask::appdir_binary") {

    // Regression: casks such as libreoffice, macdown and openscad declare
    // `binary "$APPDIR/Foo.app/Contents/..."`. The app is still in staging
    // when binaries are resolved, so den used to look in the appdir, find
    // nothing and fail with "binary not found".
    Package make_appdir_binary_cask(const fs::path& zip, const std::string& version,
                                    bool binary_first) {
        Package pkg = make_zip_cask("demo", version, zip, "Demo.app");
        CaskArtifact bin;
        bin.kind = CaskArtifactKind::Binary;
        bin.source = "$APPDIR/Demo.app/Contents/MacOS/AppBin";
        bin.target = "$HOMEBREW_PREFIX/bin/demo";
        if (binary_first) {
            pkg.cask_artifacts.insert(pkg.cask_artifacts.begin(), bin);
        } else {
            pkg.cask_artifacts.push_back(bin);
        }
        return pkg;
    }

    TEST_CASE("binary inside a staged app links into the installed app") {
        for (bool binary_first : {false, true}) {
            CAPTURE(binary_first);
            TmpDir tmp;
            auto cfg = make_cfg(tmp.path);
            setenv("DEN_APPDIR", cfg.appdir.c_str(), 1);
            setenv("DEN_HOME", cfg.den_home.c_str(), 1);

            auto zip = make_app_zip(tmp.path / "zip", "Demo.app");
            install_cask(cfg, make_appdir_binary_cask(zip, "1.0", binary_first));

            const auto link = cfg.den_home / "envs" / "ROOT" / "bin" / "demo";
            const auto want = cfg.appdir / "Demo.app" / "Contents" / "MacOS" / "AppBin";
            REQUIRE(fs::is_symlink(link));
            CHECK(fs::read_symlink(link) == want);
            CHECK(fs::exists(link)); // resolves now that the app is swapped in
            CHECK(fs::equivalent(link, want));
            // Nothing left in staging; the binary was not copied out of the bundle.
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(cfg.appdir, ec)) {
                CHECK(e.path().filename() == "Demo.app");
            }
            CHECK_FALSE(fs::exists(cask_version_path(cfg, "demo", "1.0") / "stage" / "demo"));

            auto receipt = read_cask_receipt(cask_version_path(cfg, "demo", "1.0"));
            REQUIRE(receipt);
            REQUIRE(receipt->binaries.size() == 1);
            CHECK(receipt->binaries[0].target == want);

            // Upgrade keeps the link pointing into the (new) app.
            auto zip2 = make_versioned_zip(tmp.path / "zip2", "v2");
            install_cask(cfg, make_appdir_binary_cask(zip2, "2.0", binary_first));
            CHECK(read_marker(cfg.appdir / "Demo.app") == "v2");
            CHECK(fs::is_symlink(link));
            CHECK(fs::equivalent(link, want));

            uninstall_cask(cfg, "demo");
            CHECK_FALSE(fs::exists(cfg.appdir / "Demo.app"));
            CHECK_FALSE(fs::is_symlink(link));

            unsetenv("DEN_APPDIR");
            unsetenv("DEN_HOME");
        }
    }

    TEST_CASE("missing binary inside a staged app is refused and leaves nothing behind") {
        TmpDir tmp;
        auto cfg = make_cfg(tmp.path);
        setenv("DEN_APPDIR", cfg.appdir.c_str(), 1);
        setenv("DEN_HOME", cfg.den_home.c_str(), 1);
        auto zip = make_app_zip(tmp.path / "zip", "Demo.app");
        auto pkg = make_appdir_binary_cask(zip, "1.0", false);
        pkg.cask_artifacts.back().source = "$APPDIR/Demo.app/Contents/MacOS/Nope";
        CHECK_THROWS_AS(install_cask(cfg, pkg), UserError);
        CHECK_FALSE(fs::exists(cfg.appdir / "Demo.app"));
        CHECK_FALSE(is_cask_installed(cfg, "demo"));
        CHECK_FALSE(fs::is_symlink(cfg.den_home / "envs" / "ROOT" / "bin" / "demo"));
        std::error_code ec;
        CHECK(fs::is_empty(cfg.appdir, ec));
        unsetenv("DEN_APPDIR");
        unsetenv("DEN_HOME");
    }
}

TEST_SUITE("cask::upgrade_safety") {

    TEST_CASE("upgrade succeeds and replaces the den-owned app") {
        UpgradeEnv env;
        auto z2 = make_versioned_zip(env.tmp.path / "v2", "v2");
        install_cask(env.cfg, make_zip_cask("demo", "2.0", z2, "Demo.app", "democtl"));
        CHECK(read_marker(env.app()) == "v2");
        CHECK(installed_cask_version(env.cfg, "demo") == "2.0");
        CHECK_FALSE(fs::exists(cask_version_path(env.cfg, "demo", "1.0")));
        CHECK(app_owned_by_den(env.cfg, env.app()));
        CHECK(fs::exists(env.link()));
        CHECK(fs::read_symlink(env.link()).string().find("/2.0/") != std::string::npos);
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(env.cfg.appdir, ec)) {
            CHECK(e.path().filename() == "Demo.app");
        }
    }

    TEST_CASE("upgrade with a missing download leaves the old app intact") {
        UpgradeEnv env;
        auto pkg = make_zip_cask("demo", "2.0", env.tmp.path / "nope.zip", "Demo.app", "democtl");
        CHECK_THROWS(install_cask(env.cfg, pkg));
        env.check_v1_intact();
    }

    TEST_CASE("upgrade with a corrupt archive leaves the old app intact") {
        UpgradeEnv env;
        const auto bad = env.tmp.path / "bad.zip";
        write_file(bad, "this is not a zip archive");
        CHECK_THROWS(
            install_cask(env.cfg, make_zip_cask("demo", "2.0", bad, "Demo.app", "democtl")));
        env.check_v1_intact();
    }

    TEST_CASE("upgrade fault at each stage leaves the old app intact") {
        for (std::string stage : {"download", "extract", "copy", "quarantine", "swap", "receipt"}) {
            CAPTURE(stage);
            UpgradeEnv env;
            auto z2 = make_versioned_zip(env.tmp.path / "v2", "v2");
            setenv("DEN_CASK_FAULT", stage.c_str(), 1);
            CHECK_THROWS(
                install_cask(env.cfg, make_zip_cask("demo", "2.0", z2, "Demo.app", "democtl")));
            unsetenv("DEN_CASK_FAULT");
            env.check_v1_intact();
            // A clean retry after the failure still upgrades.
            install_cask(env.cfg, make_zip_cask("demo", "2.0", z2, "Demo.app", "democtl"));
            CHECK(read_marker(env.app()) == "v2");
            CHECK(installed_cask_version(env.cfg, "demo") == "2.0");
        }
    }

    TEST_CASE("upgrade refused by a foreign app leaves everything intact") {
        UpgradeEnv env;
        // v2 adds a second app whose name collides with a foreign app.
        const fs::path foreign = env.cfg.appdir / "Other.app";
        fs::create_directories(foreign / "Contents");
        write_file(foreign / "Contents" / "marker", "not-den\n");
        auto z2 = make_versioned_zip(env.tmp.path / "v2", "v2");
        auto pkg = make_zip_cask("demo", "2.0", z2, "Demo.app", "democtl");
        CaskArtifact other;
        other.kind = CaskArtifactKind::App;
        other.source = "Demo.app";
        other.target = "Other.app";
        pkg.cask_artifacts.push_back(other);
        CHECK_THROWS_AS(install_cask(env.cfg, pkg), UserError);
        CHECK(fs::exists(foreign / "Contents" / "marker"));
        fs::remove_all(foreign);
        env.check_v1_intact();
    }
}

#else

TEST_SUITE("cask::linux") {
    TEST_CASE("install_cask errors cleanly on Linux") {
        TmpDir tmp;
        auto cfg = make_cfg(tmp.path);
        Package pkg;
        pkg.name = "demo";
        pkg.version = "1.0";
        pkg.artifact_type = ArtifactType::App;
        Package::Archive ar;
        ar.url = "https://example.com/demo.zip";
        pkg.archives["default"] = ar;
        CHECK_THROWS_AS(install_cask(cfg, pkg), UserError);
    }
}

#endif

// Font casks (font-hack, the nerd fonts, …) declare `font` artifacts, which
// den used to drop, failing with "has no supported artifacts".
TEST_SUITE("cask::fonts") {

    TEST_CASE("index keeps font artifacts through json and the lookup cache") {
        json casks = json::array({{
            {"token", "font-hack"},
            {"name", json::array({"Hack"})},
            {"version", "3.003"},
            {"url", "https://example.com/Hack-v3.003-ttf.zip"},
            {"sha256", std::string(64, 'a')},
            {"artifacts", json::array({
                              {{"font", json::array({"ttf/Hack-Bold.ttf"})},
                               {"target", "/$HOME/Library/Fonts/Hack-Bold.ttf"}},
                              {{"font", json::array({"ttf/Hack-Regular.ttf"})},
                               {"target", "/$HOME/Library/Fonts/Hack-Regular.ttf"}},
                              {{"zap", json::array()}},
                          })},
        }});
        auto idx = transform_homebrew("[]", casks.dump());
        const auto* p = idx.find("font-hack");
        REQUIRE(p != nullptr);
        REQUIRE(p->cask_artifacts.size() == 2);
        CHECK(p->cask_artifacts[0].kind == CaskArtifactKind::Font);
        CHECK(p->cask_artifacts[0].source == "ttf/Hack-Bold.ttf");
        CHECK(p->cask_artifacts[0].target == "/$HOME/Library/Fonts/Hack-Bold.ttf");

        TmpDir tmp;
        const auto path = tmp.path / "index.json";
        save_index(idx, path);
        for (auto loaded : {load_index(path), load_index_json(path)}) {
            const auto* q = loaded.find("font-hack");
            REQUIRE(q != nullptr);
            REQUIRE(q->cask_artifacts.size() == 2);
            CHECK(q->cask_artifacts[1].kind == CaskArtifactKind::Font);
            CHECK(q->cask_artifacts[1].source == "ttf/Hack-Regular.ttf");
        }
        auto enc = index_cache::encode_package(*p);
        auto dec = index_cache::decode_package(enc.data(), enc.size());
        REQUIRE(dec.cask_artifacts.size() == 2);
        CHECK(dec.cask_artifacts[0].kind == CaskArtifactKind::Font);
    }
}

#ifdef __APPLE__

namespace {

/// A font-only cask whose zip holds ttf/<name> for each font.
Package make_font_cask(const fs::path& dir, const std::string& version,
                       const std::vector<std::pair<std::string, std::string>>& fonts) {
    const fs::path content = dir / "content";
    fs::create_directories(content);
    for (const auto& [name, body] : fonts) {
        write_file(content / "ttf" / name, body);
    }
    auto zip = make_app_zip(dir, "Unused.app");
    auto pkg = make_zip_cask("font-test", version, zip, "Unused.app");
    pkg.cask_artifacts.clear();
    for (const auto& [name, body] : fonts) {
        (void)body;
        CaskArtifact a;
        a.kind = CaskArtifactKind::Font;
        a.source = "ttf/" + name;
        a.target = "/$HOME/Library/Fonts/" + name;
        pkg.cask_artifacts.push_back(a);
    }
    return pkg;
}

std::string slurp_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

/// True if p is tmp itself or lies beneath it (lexically, after normalising).
bool is_under(const fs::path& p, const fs::path& tmp) {
    const auto rel =
        fs::absolute(p).lexically_normal().lexically_relative(fs::absolute(tmp).lexically_normal());
    return !rel.empty() && *rel.begin() != "..";
}

/// Font tests must never touch the real ~/Library/Fonts: DEN_FONTDIR points
/// into the scratch dir and HOME is scratch too, so even the fallback
/// ($HOME/Library/Fonts) stays inside it. The constructor asserts both.
struct FontEnv {
    TmpDir tmp;
    Config cfg = make_cfg(tmp.path);
    fs::path fontdir = tmp.path / "Fonts";
    fs::path home = tmp.path / "home";
    std::optional<std::string> saved_home;
    FontEnv() {
        if (const char* h = std::getenv("HOME")) {
            saved_home = h;
        }
        fs::create_directories(home);
        setenv("HOME", home.c_str(), 1);
        setenv("DEN_APPDIR", cfg.appdir.c_str(), 1);
        setenv("DEN_HOME", cfg.den_home.c_str(), 1);
        setenv("DEN_FONTDIR", fontdir.c_str(), 1);
        REQUIRE(cask_fontdir(cfg) == fontdir);
        REQUIRE(is_under(cask_fontdir(cfg), tmp.path));
    }
    ~FontEnv() {
        unsetenv("DEN_APPDIR");
        unsetenv("DEN_HOME");
        unsetenv("DEN_FONTDIR");
        if (saved_home) {
            setenv("HOME", saved_home->c_str(), 1);
        } else {
            unsetenv("HOME");
        }
    }
    /// Every font a receipt records must be inside the scratch dir.
    void check_receipt_fonts_scratch(const std::string& version) const {
        auto receipt = read_cask_receipt(cask_version_path(cfg, "font-test", version));
        REQUIRE(receipt);
        for (const auto& f : receipt->fonts) {
            CHECK_MESSAGE(is_under(f, tmp.path), f.string());
        }
    }
};

} // namespace

TEST_SUITE("cask::fonts") {

    TEST_CASE("without DEN_FONTDIR fonts go to $HOME/Library/Fonts (scratch HOME here)") {
        FontEnv env;
        unsetenv("DEN_FONTDIR");
        const fs::path expected = env.home / "Library" / "Fonts";
        REQUIRE(cask_fontdir(env.cfg) == expected);
        REQUIRE(is_under(cask_fontdir(env.cfg), env.tmp.path));
        install_cask(env.cfg, make_font_cask(env.tmp.path / "v1", "1.0", {{"A-Regular.ttf", "r"}}));
        CHECK(slurp_file(expected / "A-Regular.ttf") == "r");
        env.check_receipt_fonts_scratch("1.0");
        uninstall_cask(env.cfg, "font-test");
        CHECK_FALSE(fs::exists(expected / "A-Regular.ttf"));
    }

    TEST_CASE("a font-only cask installs its fonts and uninstall removes them") {
        FontEnv env;
        auto pkg = make_font_cask(env.tmp.path / "v1", "1.0",
                                  {{"A-Regular.ttf", "regular"}, {"A-Bold.ttf", "bold"}});
        install_cask(env.cfg, pkg);

        CHECK(cask_fontdir(env.cfg) == env.fontdir);
        CHECK(slurp_file(env.fontdir / "A-Regular.ttf") == "regular");
        CHECK(slurp_file(env.fontdir / "A-Bold.ttf") == "bold");
        CHECK(font_owned_by_den(env.cfg, env.fontdir / "A-Bold.ttf"));
        auto receipt = read_cask_receipt(cask_version_path(env.cfg, "font-test", "1.0"));
        REQUIRE(receipt);
        CHECK(receipt->fonts.size() == 2);
        env.check_receipt_fonts_scratch("1.0");
        CHECK(receipt->apps.empty());
        CHECK(fs::is_empty(env.cfg.appdir));
        // No staging/backup leftovers beside the fonts.
        size_t entries = 0;
        for ([[maybe_unused]] const auto& e : fs::directory_iterator(env.fontdir))
            ++entries;
        CHECK(entries == 2);

        uninstall_cask(env.cfg, "font-test");
        CHECK_FALSE(fs::exists(env.fontdir / "A-Regular.ttf"));
        CHECK_FALSE(fs::exists(env.fontdir / "A-Bold.ttf"));
        CHECK_FALSE(is_cask_installed(env.cfg, "font-test"));
    }

    TEST_CASE("refuses to overwrite a font den did not install") {
        FontEnv env;
        write_file(env.fontdir / "A-Bold.ttf", "user's own");
        auto pkg = make_font_cask(env.tmp.path / "v1", "1.0",
                                  {{"A-Regular.ttf", "regular"}, {"A-Bold.ttf", "bold"}});
        CHECK_THROWS_AS(install_cask(env.cfg, pkg), UserError);
        CHECK(slurp_file(env.fontdir / "A-Bold.ttf") == "user's own");
        CHECK_FALSE(fs::exists(env.fontdir / "A-Regular.ttf"));
        CHECK_FALSE(is_cask_installed(env.cfg, "font-test"));
    }

    TEST_CASE("upgrade replaces den's fonts and drops ones the new version lacks") {
        FontEnv env;
        install_cask(env.cfg, make_font_cask(env.tmp.path / "v1", "1.0",
                                             {{"A-Regular.ttf", "r1"}, {"A-Old.ttf", "old"}}));
        install_cask(env.cfg, make_font_cask(env.tmp.path / "v2", "2.0",
                                             {{"A-Regular.ttf", "r2"}, {"A-New.ttf", "new"}}));
        CHECK(slurp_file(env.fontdir / "A-Regular.ttf") == "r2");
        CHECK(slurp_file(env.fontdir / "A-New.ttf") == "new");
        CHECK_FALSE(fs::exists(env.fontdir / "A-Old.ttf"));
        CHECK(installed_cask_version(env.cfg, "font-test") == "2.0");
        env.check_receipt_fonts_scratch("2.0");
        size_t entries = 0;
        for ([[maybe_unused]] const auto& e : fs::directory_iterator(env.fontdir))
            ++entries;
        CHECK(entries == 2);
        uninstall_cask(env.cfg, "font-test");
        CHECK(fs::is_empty(env.fontdir));
    }
}

#endif

} // namespace test
} // namespace den

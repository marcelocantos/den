// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>

#include "settings/settings.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace den {
namespace test {

namespace fs = std::filesystem;

struct TmpDir {
    fs::path path;

    TmpDir() {
        path =
            fs::temp_directory_path() /
            ("den_test_settings_" +
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

TEST_SUITE("settings::read_settings") {

    TEST_CASE("returns defaults for nonexistent directory") {
        auto s = read_settings("/nonexistent/den_home_xyz");
        CHECK(s.daemon.auto_download == true);
        CHECK(s.daemon.auto_upgrade == false);
        CHECK_FALSE(s.daemon.upgrade_window.has_value());
        CHECK_FALSE(s.daemon.interval_secs.has_value());
        CHECK_FALSE(s.search.provider.has_value());
    }
}

TEST_SUITE("settings::write_read_round_trip") {

    TEST_CASE("write and read back") {
        TmpDir tmp;

        Settings s;
        s.daemon.auto_download = false;
        s.daemon.auto_upgrade = true;
        s.daemon.upgrade_window = "02:00-04:00";
        s.daemon.interval_secs = 3600;
        s.search.provider = "formulae.brew.sh";

        write_settings(tmp.path, s);

        auto s2 = read_settings(tmp.path);
        CHECK(s2.daemon.auto_download == false);
        CHECK(s2.daemon.auto_upgrade == true);
        REQUIRE(s2.daemon.upgrade_window.has_value());
        CHECK(*s2.daemon.upgrade_window == "02:00-04:00");
        REQUIRE(s2.daemon.interval_secs.has_value());
        CHECK(*s2.daemon.interval_secs == 3600);
        REQUIRE(s2.search.provider.has_value());
        CHECK(*s2.search.provider == "formulae.brew.sh");
    }
}

TEST_SUITE("settings::get_setting") {

    TEST_CASE("reads a boolean setting") {
        TmpDir tmp;
        write_settings(tmp.path, Settings{}); // defaults

        CHECK(get_setting(tmp.path, "daemon.auto_upgrade") == "false");
        CHECK(get_setting(tmp.path, "daemon.auto_download") == "true");
    }

    TEST_CASE("throws for nonexistent key") {
        TmpDir tmp;
        write_settings(tmp.path, Settings{});

        CHECK_THROWS_AS(get_setting(tmp.path, "daemon.nonexistent"), std::invalid_argument);
    }
}

TEST_SUITE("settings::set_setting") {

    TEST_CASE("set boolean to true") {
        TmpDir tmp;
        write_settings(tmp.path, Settings{});

        set_setting(tmp.path, "daemon.auto_upgrade", "true");

        auto s = read_settings(tmp.path);
        CHECK(s.daemon.auto_upgrade == true);
    }

    TEST_CASE("set boolean to false") {
        TmpDir tmp;
        Settings s;
        s.daemon.auto_download = true;
        write_settings(tmp.path, s);

        set_setting(tmp.path, "daemon.auto_download", "false");

        auto s2 = read_settings(tmp.path);
        CHECK(s2.daemon.auto_download == false);
    }

    TEST_CASE("type inference for booleans") {
        TmpDir tmp;
        write_settings(tmp.path, Settings{});

        // auto_upgrade is a bool field, default false.
        set_setting(tmp.path, "daemon.auto_upgrade", "true");
        CHECK(get_setting(tmp.path, "daemon.auto_upgrade") == "true");

        set_setting(tmp.path, "daemon.auto_upgrade", "false");
        CHECK(get_setting(tmp.path, "daemon.auto_upgrade") == "false");
    }

    TEST_CASE("set a new string field") {
        TmpDir tmp;
        write_settings(tmp.path, Settings{});

        // Setting a key that doesn't exist yet stores as string.
        set_setting(tmp.path, "search.provider", "custom.example.com");
        CHECK(get_setting(tmp.path, "search.provider") == "custom.example.com");
    }

    TEST_CASE("rejects non-boolean value for boolean field") {
        TmpDir tmp;
        write_settings(tmp.path, Settings{});

        CHECK_THROWS_AS(set_setting(tmp.path, "daemon.auto_upgrade", "maybe"),
                        std::invalid_argument);
    }
}

// store.prefix is what install.sh persists via `den set` / `den get`: other
// keys must survive, and a damaged config.json must never be replaced.
TEST_SUITE("settings::store_prefix_single_key") {

    static void write_file(const fs::path& p, const std::string& body) {
        std::ofstream f(p);
        f << body;
    }
    static std::string read_file(const fs::path& p) {
        std::ifstream f(p);
        return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }

    TEST_CASE("get on a missing config.json throws SettingNotFound") {
        TmpDir tmp;
        CHECK_THROWS_AS(get_setting(tmp.path, "store.prefix"), SettingNotFound);
    }

    TEST_CASE("set store.prefix with no config.json creates it") {
        TmpDir tmp;
        set_setting(tmp.path, "store.prefix", "/x/prefix");
        CHECK(get_setting(tmp.path, "store.prefix") == "/x/prefix");
        auto st = fs::status(tmp.path / "config.json");
        CHECK((st.permissions() & fs::perms::all) == fs::perms(kConfigFileMode));
    }

    TEST_CASE("set store.prefix preserves unrelated and unknown keys") {
        TmpDir tmp;
        write_file(tmp.path / "config.json",
                   R"({"daemon":{"auto_upgrade":true,"interval_secs":60},)"
                   R"("taps":{"a/b":"https://example.invalid/b"},"future_key":{"x":[1,2]}})");
        set_setting(tmp.path, "store.prefix", "/x/prefix");
        auto j = nlohmann::json::parse(read_file(tmp.path / "config.json"));
        CHECK(j["store"]["prefix"] == "/x/prefix");
        CHECK(j["daemon"]["auto_upgrade"] == true);
        CHECK(j["daemon"]["interval_secs"] == 60);
        CHECK(j["taps"]["a/b"] == "https://example.invalid/b");
        CHECK(j["future_key"]["x"] == nlohmann::json::array({1, 2}));
    }

    TEST_CASE("an invalid config.json is reported and left untouched") {
        TmpDir tmp;
        const std::string bad = "{\"daemon\": {\"auto_upgrade\": true,\n";
        write_file(tmp.path / "config.json", bad);
        CHECK_THROWS_AS(get_setting(tmp.path, "store.prefix"), SettingsFileError);
        CHECK_THROWS_AS(set_setting(tmp.path, "store.prefix", "/x"), SettingsFileError);
        CHECK(read_file(tmp.path / "config.json") == bad);
        CHECK_FALSE(fs::exists(tmp.path / "config.json.tmp"));
    }

    TEST_CASE("a non-object config.json is reported and left untouched") {
        TmpDir tmp;
        write_file(tmp.path / "config.json", "[1,2]\n");
        CHECK_THROWS_AS(set_setting(tmp.path, "store.prefix", "/x"), SettingsFileError);
        CHECK(read_file(tmp.path / "config.json") == "[1,2]\n");
    }
}

TEST_SUITE("settings::display_all_settings") {

    TEST_CASE("returns valid JSON string") {
        TmpDir tmp;
        write_settings(tmp.path, Settings{});

        auto json_str = display_all_settings(tmp.path);
        CHECK_FALSE(json_str.empty());
        // Should contain the daemon section.
        CHECK(json_str.find("daemon") != std::string::npos);
        CHECK(json_str.find("auto_download") != std::string::npos);
    }
}

} // namespace test
} // namespace den

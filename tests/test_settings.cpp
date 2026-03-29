// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>

#include "settings/settings.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace den {
namespace test {

namespace fs = std::filesystem;

struct TmpDir {
    fs::path path;

    TmpDir() {
        path = fs::temp_directory_path() / ("den_test_settings_" + std::to_string(
            std::hash<std::string>{}(std::to_string(reinterpret_cast<uintptr_t>(this)))
            ^ static_cast<size_t>(std::chrono::steady_clock::now().time_since_epoch().count())));
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

        CHECK_THROWS_AS(
            set_setting(tmp.path, "daemon.auto_upgrade", "maybe"),
            std::invalid_argument);
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

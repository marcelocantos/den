// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Tests for environment path <-> slug encoding (production env_slug).
//
//   "/"            -> "ROOT"
//   "/ml"          -> "ml"
//   "/work/legacy" -> "work%2Flegacy"
//   "/50%off"      -> "50%25off"
//   '.'            -> "%2E"

#include <doctest.h>

#include "env/manifest.h"

#include <string>

namespace den {
namespace test {

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_SUITE("manifest::env_slug") {

    TEST_CASE("root path encodes to ROOT") {
        CHECK(env_slug("/") == "ROOT");
    }

    TEST_CASE("single-component path") {
        CHECK(env_slug("/ml") == "ml");
    }

    TEST_CASE("two-component path encodes slash as %2F") {
        CHECK(env_slug("/work/legacy") == "work%2Flegacy");
    }

    TEST_CASE("dash in component is encoded as %2D") {
        CHECK(env_slug("/my-project") == "my%2Dproject");
    }

    TEST_CASE("percent in component is encoded as %25") {
        CHECK(env_slug("/50%off") == "50%25off");
    }

    TEST_CASE("deeply nested path") {
        CHECK(env_slug("/a/b/c") == "a%2Fb%2Fc");
    }

} // TEST_SUITE env_slug

TEST_SUITE("manifest::slug_to_path") {

    TEST_CASE("ROOT decodes to /") {
        CHECK(slug_to_path("ROOT") == "/");
    }

    TEST_CASE("simple slug decodes to single-component path") {
        CHECK(slug_to_path("ml") == "/ml");
    }

    TEST_CASE("%2F decodes to /") {
        CHECK(slug_to_path("work%2Fml") == "/work/ml");
    }

    TEST_CASE("%2D in component decodes to -") {
        CHECK(slug_to_path("my%2Dproject") == "/my-project");
    }

} // TEST_SUITE slug_to_path

TEST_SUITE("manifest::round_trip") {

    static void check_round_trip(const std::string& path) {
        INFO("path = ", path);
        CHECK(slug_to_path(env_slug(path)) == path);
    }

    TEST_CASE("root") {
        check_round_trip("/");
    }
    TEST_CASE("/ml") {
        check_round_trip("/ml");
    }
    TEST_CASE("/work/ml") {
        check_round_trip("/work/ml");
    }

    TEST_CASE("path with dash") {
        check_round_trip("/work/my-project");
    }

    TEST_CASE("path with percent") {
        check_round_trip("/work/50%off");
    }

    TEST_CASE("path with literal %2D component") {
        // A component that IS the text "%2D" must survive round-trip.
        check_round_trip("/work/%2D");
    }

    TEST_CASE("path with double-dash component") {
        check_round_trip("/work/my--project");
    }

    TEST_CASE("/work/legacy slug matches production encoding") {
        CHECK(env_slug("/work/legacy") == "work%2Flegacy");
        check_round_trip("/work/legacy");
    }

} // TEST_SUITE round_trip

} // namespace test
} // namespace den

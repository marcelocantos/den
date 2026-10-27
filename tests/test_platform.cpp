// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>

#include "platform/platform.h"

namespace den {

TEST_SUITE("platform") {

    TEST_CASE("macos_codename maps known versions") {
        CHECK(macos_codename(26) == std::optional<std::string>{"tahoe"});
        CHECK(macos_codename(15) == std::optional<std::string>{"sequoia"});
        CHECK(macos_codename(14) == std::optional<std::string>{"sonoma"});
        CHECK(macos_codename(13) == std::optional<std::string>{"ventura"});
        CHECK(macos_codename(12) == std::optional<std::string>{"monterey"});
        CHECK(macos_codename(11) == std::nullopt);
        CHECK(macos_codename(0) == std::nullopt);
        CHECK(macos_codename(99) == std::nullopt);
    }

    TEST_CASE("bottle_tag_candidates: arm64 macOS 15 (Sequoia)") {
        const MacOsVersion sequoia{15, 0};
        const auto candidates = bottle_tag_candidates(Arch::Arm64, MacOsVersion{15, 0});
        REQUIRE(!candidates.empty());
        CHECK(candidates[0] == "arm64_sequoia");
        // Should also include older versions as fallbacks.
        bool found_sonoma = false;
        for (const auto& t : candidates) {
            if (t == "arm64_sonoma") {
                found_sonoma = true;
            }
        }
        CHECK(found_sonoma);
        // Should not include arm64_tahoe (newer than sequoia).
        for (const auto& t : candidates) {
            CHECK(t != "arm64_tahoe");
        }
    }

    TEST_CASE("bottle_tag_candidates: arm64 macOS 26 (Tahoe) includes all") {
        const auto candidates = bottle_tag_candidates(Arch::Arm64, MacOsVersion{26, 0});
        CHECK(candidates[0] == "arm64_tahoe");
        bool found_sequoia = false, found_sonoma = false;
        for (const auto& t : candidates) {
            if (t == "arm64_sequoia") {
                found_sequoia = true;
            }
            if (t == "arm64_sonoma") {
                found_sonoma = true;
            }
        }
        CHECK(found_sequoia);
        CHECK(found_sonoma);
    }

    TEST_CASE("bottle_tag_candidates: x86_64 Linux") {
        const auto candidates = bottle_tag_candidates(Arch::X86_64, std::nullopt);
        REQUIRE(candidates.size() == 1);
        CHECK(candidates[0] == "x86_64_linux");
    }

    TEST_CASE("best_archive_tag: arm64 macOS picks arm64_sequoia") {
        const std::vector<std::string> available = {
            "arm64_sequoia",
            "arm64_sonoma",
            "x86_64_linux",
        };
        const auto tag = best_archive_tag(Arch::Arm64, MacOsVersion{15, 0}, available);
        REQUIRE(tag.has_value());
        CHECK(*tag == "arm64_sequoia");
    }

    TEST_CASE("best_archive_tag: arm64 macOS falls back to arm64_sonoma") {
        // Only sonoma is available — sequoia not present.
        const std::vector<std::string> available = {
            "arm64_sonoma",
            "x86_64_linux",
        };
        const auto tag = best_archive_tag(Arch::Arm64, MacOsVersion{15, 0}, available);
        REQUIRE(tag.has_value());
        CHECK(*tag == "arm64_sonoma");
    }

    TEST_CASE("best_archive_tag: x86_64 Linux picks x86_64_linux") {
        const std::vector<std::string> available = {
            "arm64_sequoia",
            "x86_64_linux",
        };
        const auto tag = best_archive_tag(Arch::X86_64, std::nullopt, available);
        REQUIRE(tag.has_value());
        CHECK(*tag == "x86_64_linux");
    }

    TEST_CASE("best_archive_tag: returns nullopt when no tag matches") {
        const std::vector<std::string> available = {
            "some_other_platform",
        };
        const auto tag = best_archive_tag(Arch::Arm64, MacOsVersion{15, 0}, available);
        CHECK(!tag.has_value());
    }

    TEST_CASE("best_archive_tag: empty available list returns nullopt") {
        const auto tag = best_archive_tag(Arch::Arm64, MacOsVersion{15, 0}, {});
        CHECK(!tag.has_value());
    }

    TEST_CASE("macos_version_from_darwin: Big Sur..Sequoia use offset 9") {
        CHECK(macos_version_from_darwin("20.6.0")->major == 11);
        CHECK(macos_version_from_darwin("23.4.0")->major == 14);
        CHECK(macos_version_from_darwin("24.1.0")->major == 15);
        CHECK(macos_version_from_darwin("24.1.0")->minor == 1);
    }

    TEST_CASE("macos_version_from_darwin: Darwin 25 is macOS 26 (Tahoe), not 16") {
        const auto v = macos_version_from_darwin("25.6.0");
        REQUIRE(v.has_value());
        CHECK(v->major == 26);
        CHECK(v->minor == 6);
        CHECK(macos_version_from_darwin("26.0.0")->major == 27);
        CHECK(!macos_version_from_darwin("19.6.0").has_value());
        CHECK(!macos_version_from_darwin("garbage").has_value());
    }

    TEST_CASE("parse_macos_product_version") {
        auto v = parse_macos_product_version("26.6.2");
        REQUIRE(v.has_value());
        CHECK(v->to_string() == "26.6");
        CHECK(parse_macos_product_version("15")->to_string() == "15.0");
        CHECK(!parse_macos_product_version("").has_value());
        CHECK(!parse_macos_product_version("x").has_value());
    }

    TEST_CASE("resolve_macos_version: real product version wins") {
        auto v = resolve_macos_version(std::string("26.6.2"), std::string("25.6.0"));
        REQUIRE(v.has_value());
        CHECK(v->to_string() == "26.6");
        CHECK(resolve_macos_version(std::string("15.7.1"), std::string("24.6.0"))->to_string() ==
              "15.7");
    }

    TEST_CASE("resolve_macos_version: SYSTEM_VERSION_COMPAT shims fall back to Darwin") {
        // Regression: den config reported macos_version 16.0 on 26.6.2.
        auto v = resolve_macos_version(std::string("16.0"), std::string("25.6.0"));
        REQUIRE(v.has_value());
        CHECK(v->major == 26);
        v = resolve_macos_version(std::string("10.16"), std::string("24.6.0"));
        REQUIRE(v.has_value());
        CHECK(v->major == 15);
    }

    TEST_CASE("resolve_macos_version: missing inputs") {
        CHECK(resolve_macos_version(std::nullopt, std::string("25.6.0"))->major == 26);
        CHECK(resolve_macos_version(std::string("26.1"), std::nullopt)->to_string() == "26.1");
        CHECK(!resolve_macos_version(std::nullopt, std::nullopt).has_value());
    }

    TEST_CASE("Tahoe hosts get arm64_tahoe bottles first") {
        const auto v = resolve_macos_version(std::string("26.6.2"), std::string("25.6.0"));
        const auto cands = bottle_tag_candidates(Arch::Arm64, v);
        REQUIRE(!cands.empty());
        CHECK(cands.front() == "arm64_tahoe");
    }

#ifdef __APPLE__
    TEST_CASE("detect_macos_version agrees with the kernel on this host") {
        const auto v = detect_macos_version();
        REQUIRE(v.has_value());
        CHECK(v->major >= 11);
        CHECK((v->major <= 15 || v->major >= 26));
    }
#endif

    TEST_CASE("detect_arch returns a valid arch") {
        const Arch arch = detect_arch();
        CHECK((arch == Arch::Arm64 || arch == Arch::X86_64));
    }

} // TEST_SUITE

} // namespace den

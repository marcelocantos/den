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
        CHECK(macos_codename(0)  == std::nullopt);
        CHECK(macos_codename(99) == std::nullopt);
    }

    TEST_CASE("bottle_tag_candidates: arm64 macOS 15 (Sequoia)") {
        const MacOsVersion sequoia{15, 0};
        const auto candidates = bottle_tag_candidates(Arch::Arm64,
                                                      MacOsVersion{15, 0});
        REQUIRE(!candidates.empty());
        CHECK(candidates[0] == "arm64_sequoia");
        // Should also include older versions as fallbacks.
        bool found_sonoma = false;
        for (const auto& t : candidates) {
            if (t == "arm64_sonoma") { found_sonoma = true; }
        }
        CHECK(found_sonoma);
        // Should not include arm64_tahoe (newer than sequoia).
        for (const auto& t : candidates) {
            CHECK(t != "arm64_tahoe");
        }
    }

    TEST_CASE("bottle_tag_candidates: arm64 macOS 26 (Tahoe) includes all") {
        const auto candidates = bottle_tag_candidates(Arch::Arm64,
                                                      MacOsVersion{26, 0});
        CHECK(candidates[0] == "arm64_tahoe");
        bool found_sequoia = false, found_sonoma = false;
        for (const auto& t : candidates) {
            if (t == "arm64_sequoia") { found_sequoia = true; }
            if (t == "arm64_sonoma")  { found_sonoma = true; }
        }
        CHECK(found_sequoia);
        CHECK(found_sonoma);
    }

    TEST_CASE("bottle_tag_candidates: x86_64 Linux") {
        const auto candidates = bottle_tag_candidates(Arch::X86_64,
                                                      std::nullopt);
        REQUIRE(candidates.size() == 1);
        CHECK(candidates[0] == "x86_64_linux");
    }

    TEST_CASE("best_archive_tag: arm64 macOS picks arm64_sequoia") {
        const std::vector<std::string> available = {
            "arm64_sequoia", "arm64_sonoma", "x86_64_linux",
        };
        const auto tag = best_archive_tag(Arch::Arm64,
                                          MacOsVersion{15, 0},
                                          available);
        REQUIRE(tag.has_value());
        CHECK(*tag == "arm64_sequoia");
    }

    TEST_CASE("best_archive_tag: arm64 macOS falls back to arm64_sonoma") {
        // Only sonoma is available — sequoia not present.
        const std::vector<std::string> available = {
            "arm64_sonoma", "x86_64_linux",
        };
        const auto tag = best_archive_tag(Arch::Arm64,
                                          MacOsVersion{15, 0},
                                          available);
        REQUIRE(tag.has_value());
        CHECK(*tag == "arm64_sonoma");
    }

    TEST_CASE("best_archive_tag: x86_64 Linux picks x86_64_linux") {
        const std::vector<std::string> available = {
            "arm64_sequoia", "x86_64_linux",
        };
        const auto tag = best_archive_tag(Arch::X86_64,
                                          std::nullopt,
                                          available);
        REQUIRE(tag.has_value());
        CHECK(*tag == "x86_64_linux");
    }

    TEST_CASE("best_archive_tag: returns nullopt when no tag matches") {
        const std::vector<std::string> available = {
            "some_other_platform",
        };
        const auto tag = best_archive_tag(Arch::Arm64,
                                          MacOsVersion{15, 0},
                                          available);
        CHECK(!tag.has_value());
    }

    TEST_CASE("best_archive_tag: empty available list returns nullopt") {
        const auto tag = best_archive_tag(Arch::Arm64,
                                          MacOsVersion{15, 0},
                                          {});
        CHECK(!tag.has_value());
    }

    TEST_CASE("detect_arch returns a valid arch") {
        const Arch arch = detect_arch();
        CHECK((arch == Arch::Arm64 || arch == Arch::X86_64));
    }

} // TEST_SUITE

} // namespace den

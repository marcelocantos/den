// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// `den doctor` Mach-O header-space check: warn when den's prefix is longer
// than the prefix bottles were built for, because relocated load commands
// must fit in each binary's spare header padding.

#include "../src/doctor/doctor.h"

#include <doctest.h>

#include <string>

namespace den {

TEST_SUITE("doctor::prefix_header_space") {

    TEST_CASE("standard bottle prefix matches Homebrew's default for the host") {
#ifdef __APPLE__
        CHECK(standard_bottle_prefix(Arch::Arm64) == "/opt/homebrew");
        CHECK(standard_bottle_prefix(Arch::X86_64) == "/usr/local");
#else
        CHECK(standard_bottle_prefix(Arch::X86_64) == "/home/linuxbrew/.linuxbrew");
#endif
    }

    TEST_CASE("prefix no longer than the standard prefix does not warn") {
        CHECK(prefix_header_space_warning("/opt/homebrew", "/opt/homebrew", true).empty());
        CHECK(prefix_header_space_warning("/opt/den", "/opt/homebrew", true).empty());
        CHECK(prefix_header_space_warning("/tmp/abcdefgh", "/opt/homebrew", true)
                  .empty()); // 13 == 13
    }

    TEST_CASE("longer prefix warns on Mach-O platforms") {
        const std::string p = "/Users/someone/.den/prefix";
        auto msg = prefix_header_space_warning(p, "/opt/homebrew", true);
        REQUIRE_FALSE(msg.empty());
        CHECK(msg.find(p) != std::string::npos);
        CHECK(msg.find("/opt/homebrew") != std::string::npos);
        CHECK(msg.find(std::to_string(p.size())) != std::string::npos);
        CHECK(msg.find("13") != std::string::npos);
        CHECK(msg.find("header") != std::string::npos);
        // One character over is enough to warn.
        CHECK_FALSE(prefix_header_space_warning("/opt/homebrewX", "/opt/homebrew", true).empty());
    }

    TEST_CASE("ELF platforms never warn (patchelf can grow the dynamic section)") {
        CHECK(prefix_header_space_warning("/home/someone/.den/prefix/very/long/path",
                                          "/home/linuxbrew/.linuxbrew", false)
                  .empty());
    }

} // TEST_SUITE

} // namespace den

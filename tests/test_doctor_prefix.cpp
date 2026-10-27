// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// `den doctor` Mach-O header-space check: an informational note when den's
// prefix is longer than the prefix bottles were built for (relocated load
// commands must fit in each binary's spare header padding). It is only a
// warning when kegs from mismatched fixed-cellar bottles are present (see
// test_fixed_cellar.cpp).

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

    TEST_CASE("prefix no longer than the standard prefix needs no note") {
        CHECK(prefix_header_space_note("/opt/homebrew", "/opt/homebrew", true).empty());
        CHECK(prefix_header_space_note("/opt/den", "/opt/homebrew", true).empty());
        CHECK(prefix_header_space_note("/tmp/abcdefgh", "/opt/homebrew", true).empty()); // 13 == 13
    }

    TEST_CASE("longer prefix gets an informational note on Mach-O platforms") {
        const std::string p = "/Users/someone/.den/prefix";
        auto msg = prefix_header_space_note(p, "/opt/homebrew", true);
        REQUIRE_FALSE(msg.empty());
        CHECK(msg.find(p) != std::string::npos);
        CHECK(msg.find("/opt/homebrew") != std::string::npos);
        CHECK(msg.find(std::to_string(p.size())) != std::string::npos);
        CHECK(msg.find("13") != std::string::npos);
        CHECK(msg.find("header") != std::string::npos);
        CHECK(msg.find("fixed-cellar") != std::string::npos);
        // One character over is enough for the note.
        CHECK_FALSE(prefix_header_space_note("/opt/homebrewX", "/opt/homebrew", true).empty());
    }

    TEST_CASE("ELF platforms get no note (patchelf can grow the dynamic section)") {
        CHECK(prefix_header_space_note("/home/someone/.den/prefix/very/long/path",
                                       "/home/linuxbrew/.linuxbrew", false)
                  .empty());
    }

} // TEST_SUITE

} // namespace den

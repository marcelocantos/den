// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>

#include "ruby/portable_ruby_manifest.h"

#include <cctype>
#include <string>
#include <string_view>

namespace den {

namespace {

bool looks_like_sha256(std::string_view s) {
    if (s.size() != 64) {
        return false;
    }
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_SUITE("portable_ruby_manifest") {

    TEST_CASE("version is non-empty") {
        CHECK(std::string_view(kPortableRubyVersion).size() > 0);
    }

    TEST_CASE("arm64_linux URL is well-formed and contains the pinned version") {
        std::string_view url = kPortableRubyArm64LinuxUrl;
        CHECK(url.starts_with("https://github.com/Homebrew/homebrew-portable-ruby/"));
        CHECK(url.find(kPortableRubyVersion) != std::string_view::npos);
        CHECK(url.find("arm64_linux") != std::string_view::npos);
        CHECK(url.ends_with(".tar.gz"));
    }

    TEST_CASE("x86_64_linux URL is well-formed and contains the pinned version") {
        std::string_view url = kPortableRubyX8664LinuxUrl;
        CHECK(url.starts_with("https://github.com/Homebrew/homebrew-portable-ruby/"));
        CHECK(url.find(kPortableRubyVersion) != std::string_view::npos);
        CHECK(url.find("x86_64_linux") != std::string_view::npos);
        CHECK(url.ends_with(".tar.gz"));
    }

    TEST_CASE("SHA-256 constants are 64 hex characters") {
        CHECK(looks_like_sha256(kPortableRubyArm64LinuxSha256));
        CHECK(looks_like_sha256(kPortableRubyX8664LinuxSha256));
    }

    TEST_CASE("arm64 and x86_64 SHA-256s differ") {
        CHECK(std::string(kPortableRubyArm64LinuxSha256) !=
              std::string(kPortableRubyX8664LinuxSha256));
    }
}

} // namespace den

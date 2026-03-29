// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>

#include "download/sha256.h"

#include <filesystem>
#include <fstream>

namespace den {

TEST_SUITE("sha256") {

    TEST_CASE("hash_string produces correct digest for known input") {
        // SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
        CHECK(hash_string("") == "e3b0c44298fc1c149afbf4c8996fb924"
                                 "27ae41e4649b934ca495991b7852b855");

        // SHA-256("abc")
        CHECK(hash_string("abc") == "ba7816bf8f01cfea414140de5dae2223"
                                    "b00361a396177a9cb410ff61f20015ad");

        // SHA-256("hello") = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
        CHECK(hash_string("hello") == "2cf24dba5fb0a30e26e83b2ac5b9e29e"
                                      "1b161e5c1fa7425e73043362938b9824");
    }

    TEST_CASE("hash_file produces correct digest for a temp file") {
        // Create a temporary file with known content.
        const auto tmp_path = std::filesystem::temp_directory_path() / "den_sha256_test.txt";

        {
            std::ofstream f(tmp_path, std::ios::binary);
            REQUIRE(f.is_open());
            f << "hello";
        }

        const std::string digest = hash_file(tmp_path);
        std::filesystem::remove(tmp_path);

        CHECK(digest == "2cf24dba5fb0a30e26e83b2ac5b9e29e"
                        "1b161e5c1fa7425e73043362938b9824");
    }

    TEST_CASE("hash_file throws on missing file") {
        CHECK_THROWS_AS(hash_file("/nonexistent/path/that/should/not/exist.bin"),
                        std::runtime_error);
    }

} // TEST_SUITE

} // namespace den

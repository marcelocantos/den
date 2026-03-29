// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "sha256.h"

#include "../core/error.h"

#include <picosha2.h>

#include <array>
#include <fstream>

namespace den {

std::string hash_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw DownloadError("cannot open file for hashing: " + path.string());
    }

    picosha2::hash256_one_by_one hasher;

    constexpr std::size_t kBufSize = 8192;
    std::array<char, kBufSize> buf{};

    while (file.read(buf.data(), kBufSize) || file.gcount() > 0) {
        auto count = static_cast<std::size_t>(file.gcount());
        hasher.process(buf.begin(), buf.begin() + count);
    }

    hasher.finish();

    std::string hex;
    picosha2::get_hash_hex_string(hasher, hex);
    return hex;
}

std::string hash_string(const std::string& data) {
    return picosha2::hash256_hex_string(data);
}

bool verify_file(const fs::path& path, const std::string& expected) {
    return hash_file(path) == expected;
}

} // namespace den

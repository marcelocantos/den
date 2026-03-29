// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <filesystem>
#include <string>

namespace den {

namespace fs = std::filesystem;

/// Compute the SHA256 hex digest of a file using streaming reads.
std::string hash_file(const fs::path& path);

/// Compute the SHA256 hex digest of an in-memory string.
std::string hash_string(const std::string& data);

/// Verify a file's SHA256 matches the expected hex digest.
bool verify_file(const fs::path& path, const std::string& expected);

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <filesystem>

namespace den {

namespace fs = std::filesystem;

/// Embedded Ruby bundle: Portable Ruby stdlib + Homebrew library + Sorbet runtime.
/// Compressed with zstd --ultra -22 (~1.6MB, expands to ~14MB).
extern const unsigned char ruby_bundle_data[];
extern const size_t ruby_bundle_size;

/// Unpack the embedded Ruby bundle to den_home/ruby/ if not already unpacked.
/// Returns the path to the unpacked ruby directory.
/// Thread-safe (uses a lock file).
fs::path ensure_ruby_bundle(const fs::path& den_home);

} // namespace den

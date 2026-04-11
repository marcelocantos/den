// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <filesystem>

namespace den {

namespace fs = std::filesystem;

/// Lazy-download Homebrew's Portable Ruby for Linux and extract it into
/// `den_home/ruby/`, producing the same internal layout as the macOS
/// embedded bundle (`den_home/ruby/ruby/bin/ruby`).
///
/// Thread-safe (uses the same lock file as `ensure_ruby_bundle`).
/// Returns the path to the unpacked ruby directory (`den_home/ruby`).
///
/// Throws:
///   - `DownloadError` if the network fetch fails or SHA-256 verification fails
///   - `ArchiveError` if extraction fails
///   - `InternalError` on lock-file, filesystem errors, or unsupported arch
fs::path ensure_portable_ruby_linux(const fs::path& den_home);

} // namespace den

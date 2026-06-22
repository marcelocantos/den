// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <filesystem>
#include <vector>

namespace den {

namespace fs = std::filesystem;

// In-use file detection for the upgrade-deferral path (🎯T51 / 🎯T72).
//
// The daemon must not replace a keg whose files are currently held open by a
// live process. These helpers probe the OS for open file handles:
//   - macOS: parses `lsof` output.
//   - Linux: scans /proc/<pid>/maps (and the per-pid fd table).
//
// Both helpers are defensive: a missing tool, a permission error, or a
// process that exits mid-scan is treated as "not detectably in use" rather
// than throwing — the caller decides what to do with the (possibly empty)
// answer.

/// Return true if any live process currently holds the given file open.
/// Returns false for a nonexistent path or when detection is unavailable.
bool is_file_in_use(const fs::path& file);

/// Scan a Cellar keg root (e.g. <cellar>/openssl/3.2.0) and return the set of
/// regular files under it that are currently held open by a live process.
/// Returns an empty vector if the keg root does not exist or nothing is held.
std::vector<fs::path> files_in_use_under(const fs::path& keg_root);

} // namespace den

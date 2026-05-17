// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T61 — Quick smoke check: supervisor without launchctl
//
// Provides run_supervisor_smoke(), called from the smoke runner to verify that
// den's built-in supervisor does not delegate to launchctl.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

/// Result of a single supervisor smoke check step.
struct SupervisorCheckResult {
    std::string description;
    bool passed = false;
    std::string detail; // non-empty on failure
};

/// Result of the full supervisor smoke run.
struct SupervisorSmokeResult {
    std::vector<SupervisorCheckResult> checks;
    bool all_passed = false;
};

/// Run the supervisor smoke check.
///
/// den_binary  — path to the den binary under test.
/// scratch_dir — writable temporary directory for spy bins, logs, etc.
///               The caller owns lifecycle (create before call, remove after).
SupervisorSmokeResult run_supervisor_smoke(const fs::path& den_binary, const fs::path& scratch_dir);

} // namespace den

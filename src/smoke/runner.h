// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"

#include <filesystem>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

/// Result of a single smoke check.
struct CheckResult {
    std::string description;
    bool passed = false;
    std::string output; // stdout+stderr if failed
};

/// Result of a full smoke test for one package.
struct SmokeResult {
    std::string package_name;
    bool installed = false;
    std::vector<CheckResult> checks;

    bool all_passed() const {
        if (!installed) return false;
        for (const auto& c : checks)
            if (!c.passed) return false;
        return true;
    }

    int pass_count() const {
        int n = 0;
        for (const auto& c : checks)
            if (c.passed) ++n;
        return n;
    }
};

/// Run tier 1 smoke tests from a JSON definition file.
/// Each package is installed into an isolated DEN_HOME,
/// then checks are executed against the installed package.
/// If real_den_home is provided, uses its cached index;
/// otherwise fetches a fresh index.
std::vector<SmokeResult> run_smoke_tests(const fs::path& test_defs_json,
                                         const Config& config,
                                         int max_packages = 0);

} // namespace den

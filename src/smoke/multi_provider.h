// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T60 — Multi-provider smoke check API.
//
// run_multi_provider_smoke() exercises install → list → run → uninstall for
// one representative package per provider.  All probes return Skip until the
// relevant upstream target (🎯T23/T38/T39/T40/T41) is implemented.
//
// Representative packages:
//   homebrew : jq           — plain binary, `jq --version` (baseline, always implemented)
//   python   : black        — `black --version` (🎯T38)
//   node     : prettier     — `prettier --version` (🎯T39)
//   go       : golangci-lint — `golangci-lint --version` (🎯T40)
//   cargo    : ripgrep      — crate ripgrep, binary rg, `rg --version` (🎯T41)

#pragma once

#include <string>
#include <vector>

namespace den {
namespace smoke {

enum class SmokeStatus {
    Skip = 0, // Provider not yet implemented; blocked on upstream target.
    Pass = 1, // Install/list/run/uninstall all succeeded.
    Fail = 2, // One or more steps failed.
};

struct ProviderSmokeResult {
    std::string provider;     // e.g. "homebrew", "python", "node", "go", "cargo"
    std::string package_name; // e.g. "jq", "black", "prettier", "golangci-lint", "ripgrep"
    SmokeStatus status = SmokeStatus::Skip;
    std::string note; // failure message or skip reason
};

struct MultiProviderSmokeResult {
    std::vector<ProviderSmokeResult> providers;

    bool all_pass() const {
        for (const auto& r : providers)
            if (r.status != SmokeStatus::Pass)
                return false;
        return !providers.empty();
    }

    bool any_fail() const {
        for (const auto& r : providers)
            if (r.status == SmokeStatus::Fail)
                return true;
        return false;
    }
};

/// Run the multi-provider smoke suite. Returns results for each provider.
/// Currently all probes skip; call this to confirm the harness compiles and
/// runs as CI evidence for 🎯T60.
MultiProviderSmokeResult run_multi_provider_smoke();

/// Print a human-readable summary of the smoke results to stdout.
void print_multi_provider_smoke_result(const MultiProviderSmokeResult& result);

} // namespace smoke
} // namespace den

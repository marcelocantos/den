// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T60 — Multi-provider smoke check.
//
// Invocable via `den doctor --smoke multi-provider` once that flag is wired.
// Each provider probe is self-contained: it attempts a minimal install/run/
// uninstall cycle against an isolated DEN_HOME and reports SKIP / PASS / FAIL.
//
// Representative packages per provider:
//   Homebrew  : jq          — small, standalone, `jq --version`
//   Python    : black       — `black --version` (🎯T38)
//   Node/npm  : prettier    — `prettier --version` (🎯T39)
//   Go        : golangci-lint — `golangci-lint --version` (🎯T40)
//   Cargo     : ripgrep     — crate name ripgrep, binary rg (🎯T41)
//
// Status:
//   All five probes return SmokeStatus::Skip until the respective upstream
//   target is implemented. The harness is intentionally structured so that
//   returning PASS from any probe constitutes progress evidence for T60.

#include "multi_provider.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace den {
namespace smoke {

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// run_cmd — capture stdout+stderr; return {exit_code, output}.
// ---------------------------------------------------------------------------
std::pair<int, std::string> run_cmd(const std::string& cmd) {
    std::string output;
    std::array<char, 4096> buf{};
    FILE* pipe = ::popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe)
        return {-1, "popen failed"};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        output += buf.data();
    int raw = ::pclose(pipe);
    int code = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
    return {code, output};
}

// ---------------------------------------------------------------------------
// Probe — one provider's smoke check.
// ---------------------------------------------------------------------------
struct Probe {
    std::string provider;     // human label
    std::string target;       // upstream 🎯T identifier blocking this probe
    std::string package_name; // as the user would type to `den install`
    std::string run_cmd_str;  // command to verify the installed binary
    std::string run_expect;   // substring expected in run output
};

// run_probe wraps a single install/run/uninstall cycle, returning a
// ProviderSmokeResult with appropriate status.
ProviderSmokeResult run_probe(const Probe& p, const fs::path& den_home_root) {
    ProviderSmokeResult result;
    result.provider = p.provider;
    result.package_name = p.package_name;
    result.status = SmokeStatus::Skip;
    result.note = p.target + " not yet implemented";

    // Until the provider is wired (T23 + individual Txx), every probe skips.
    //
    // TODO (🎯T23): replace this early-return with an actual install/run/uninstall cycle.
    // The expected flow once providers are wired:
    //   1. Create isolated den_home = den_home_root / ("probe-" + p.provider)
    //   2. run `den install <package>` with DEN_HOME=den_home
    //   3. verify `den list` contains p.package_name
    //   4. run p.run_cmd_str, verify output contains p.run_expect
    //   5. run `den uninstall <package>`
    //   6. verify `den list` no longer contains p.package_name
    //   7. set result.status = SmokeStatus::Pass
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

MultiProviderSmokeResult run_multi_provider_smoke() {
    // Isolated DEN_HOME root for all probes in this run.
    auto den_home_root = fs::temp_directory_path() / "den-smoke-mp";
    std::error_code ec;
    fs::create_directories(den_home_root, ec);

    // Probe definitions — one per active provider.
    // See file-header comment for package rationale.
    static const std::vector<Probe> kProbes = {
        {
            "homebrew",
            "🎯T23",
            "jq",
            "jq --version",
            "jq-",
        },
        {
            "python",
            "🎯T38",
            "black",
            "black --version",
            "black",
        },
        {
            "node",
            "🎯T39",
            "prettier",
            "prettier --version",
            ".", // any semver digit string
        },
        {
            "go",
            "🎯T40",
            "golangci-lint",
            "golangci-lint --version",
            "golangci-lint",
        },
        {
            "cargo",
            "🎯T41",
            "ripgrep",
            "rg --version",
            "ripgrep",
        },
    };

    MultiProviderSmokeResult overall;
    for (const auto& probe : kProbes) {
        overall.providers.push_back(run_probe(probe, den_home_root));
    }

    // Clean up root.
    fs::remove_all(den_home_root, ec);
    return overall;
}

void print_multi_provider_smoke_result(const MultiProviderSmokeResult& result) {
    static const char* kLabel[] = {"SKIP", "PASS", "FAIL"};
    std::cout << "\n=== Multi-provider smoke (🎯T60) ===\n";
    int passed = 0, failed = 0, skipped = 0;
    for (const auto& r : result.providers) {
        auto status_idx = static_cast<int>(r.status);
        std::cout << "  [" << kLabel[status_idx] << "] " << r.provider << " :: " << r.package_name;
        if (!r.note.empty())
            std::cout << "  (" << r.note << ")";
        std::cout << "\n";
        switch (r.status) {
        case SmokeStatus::Pass:
            ++passed;
            break;
        case SmokeStatus::Fail:
            ++failed;
            break;
        case SmokeStatus::Skip:
            ++skipped;
            break;
        }
    }
    std::cout << "\n  " << passed << " passed, " << failed << " failed, " << skipped
              << " skipped\n";
    if (passed == static_cast<int>(result.providers.size()))
        std::cout << "  All providers PASS.\n";
    else if (failed > 0)
        std::cout << "  Some providers FAILED.\n";
    else
        std::cout << "  Providers not yet implemented (all SKIP).\n";
}

} // namespace smoke
} // namespace den

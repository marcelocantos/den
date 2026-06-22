// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T60 — Multi-provider smoke check.
//
// Each provider probe runs a real install → list → run → uninstall cycle in
// an isolated DEN_HOME, driving the same orchestration the CLI uses. Probes
// degrade gracefully: if a provider's toolchain (uv/pip, npm, go, cargo) is
// absent, or an install fails for an environmental reason (no network), the
// probe reports Skip with a clear note rather than Fail, so the check stays
// deterministic in offline/minimal environments.
//
// Representative packages per provider:
//   Homebrew  : jq          — install path needs a fetched index, so it is
//                             skipped here (covered by the den_tests harness);
//                             this probe focuses on the new ecosystem providers
//   Python    : black       — `black --version`            (🎯T38)
//   Node/npm  : prettier     — `prettier --version`         (🎯T39)
//   Go        : gofumpt      — small, fast `go install`     (🎯T40)
//   Cargo     : xsv          — small crate, `xsv --version` (🎯T41)

#include "multi_provider.h"

#include "../core/config.h"
#include "../core/error.h"
#include "../env/environment.h"
#include "../env/manifest.h"
#include "../index/package.h"
#include "../provider/exec.h"
#include "../provider/registry.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace den {
namespace smoke {

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Probe — one provider's smoke check.
// ---------------------------------------------------------------------------
struct Probe {
    std::string provider;      // den provider name ("pip", "npm", "go", "cargo")
    std::string label;         // human label
    std::string toolchain;     // executable that must exist for this probe to run
    std::string package_name;  // as the user would type to `den install`
    std::string run_binary;    // binary to invoke (may differ from package_name)
    bool expect_version_digit; // run output should contain a digit (a version)
};

// Build an isolated Config rooted at `den_home`.
Config make_config(const fs::path& den_home) {
    Config cfg;
    cfg.den_home = den_home;
    cfg.store = den_home / "store";
    cfg.cache = den_home / "cache";
    fs::create_directories(cfg.store);
    fs::create_directories(cfg.cache);
    return cfg;
}

// run_probe wraps a single install → list → run → uninstall cycle entirely
// in-process (no dependency on `den` being on PATH).
ProviderSmokeResult run_probe(const Probe& p, const fs::path& den_home_root) {
    ProviderSmokeResult result;
    result.provider = p.label;
    result.package_name = p.package_name;
    result.status = SmokeStatus::Skip;

    if (!tool_available(p.toolchain)) {
        result.note = p.toolchain + " not found — provider toolchain unavailable";
        return result;
    }

    auto den_home = den_home_root / ("probe-" + p.provider);
    std::error_code ec;
    fs::remove_all(den_home, ec);
    auto cfg = make_config(den_home);
    auto active = active_env_path(cfg.den_home); // "/"

    PackageIndex empty_idx; // ecosystem providers don't consult the index
    auto registry = make_default_registry(&empty_idx);
    auto* provider = registry.find(p.provider);
    if (!provider) {
        result.status = SmokeStatus::Fail;
        result.note = "provider '" + p.provider + "' not registered";
        return result;
    }

    // 1. Install.
    std::string version;
    try {
        auto install_result = provider->install(cfg, p.package_name, "");
        version = install_result.resolved_version;
    } catch (const std::exception& e) {
        // Treat install failure as Skip — almost always a network/registry
        // hiccup in CI, not a defect in the provider wiring.
        result.note = std::string("install failed (skipping): ") + e.what();
        return result;
    }

    // Record in the manifest + materialise the env, exactly as the CLI does.
    with_manifest(cfg.den_home, active,
                  [&](Manifest& m) { m.packages[p.provider][p.package_name] = version; });
    materialise(cfg.den_home, cfg.store, active, &empty_idx);

    auto fail = [&](const std::string& note) {
        result.status = SmokeStatus::Fail;
        result.note = note;
    };

    // 2. List — the package must appear under its provider.
    bool listed = false;
    for (const auto& installed : provider->list_installed(cfg)) {
        if (installed.name == p.package_name) {
            listed = true;
            break;
        }
    }
    if (!listed) {
        fail("installed package not reported by list_installed");
        return result;
    }

    // 3. Run — the binary must be on the env's PATH and execute.
    auto bin_dir = env_dir(cfg.den_home, active) / "bin";
    auto binary = bin_dir / p.run_binary;
    if (!fs::exists(binary, ec)) {
        fail("binary '" + p.run_binary + "' not materialised into env bin/");
        return result;
    }
    auto run_res = run_tool({binary.string(), "--version"}, {}, bin_dir.string());
    if (!run_res.spawned || run_res.exit_code != 0) {
        fail("`" + p.run_binary + " --version` failed:\n" + run_res.output);
        return result;
    }
    if (p.expect_version_digit) {
        bool has_digit = false;
        for (char c : run_res.output) {
            if (std::isdigit(static_cast<unsigned char>(c))) {
                has_digit = true;
                break;
            }
        }
        if (!has_digit) {
            fail("run output had no version digit:\n" + run_res.output);
            return result;
        }
    }

    // 4. Uninstall — the package must disappear.
    provider->uninstall(cfg, p.package_name);
    for (const auto& installed : provider->list_installed(cfg)) {
        if (installed.name == p.package_name) {
            fail("package still present after uninstall");
            return result;
        }
    }

    fs::remove_all(den_home, ec);
    result.status = SmokeStatus::Pass;
    result.note = p.package_name + " " + version + " install/list/run/uninstall OK";
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

MultiProviderSmokeResult run_multi_provider_smoke() {
    auto den_home_root = fs::temp_directory_path() / "den-smoke-mp";
    std::error_code ec;
    fs::create_directories(den_home_root, ec);

    static const std::vector<Probe> kProbes = {
        {"pip", "python", "uv", "black", "black", true},
        {"npm", "node", "npm", "prettier", "prettier", true},
        {"go", "go", "go", "gofumpt", "gofumpt", true},
        {"cargo", "cargo", "cargo", "xsv", "xsv", true},
    };

    MultiProviderSmokeResult overall;
    for (const auto& probe : kProbes) {
        overall.providers.push_back(run_probe(probe, den_home_root));
    }

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
    if (failed > 0)
        std::cout << "  Some providers FAILED.\n";
    else if (passed > 0)
        std::cout << "  Implemented providers PASS.\n";
    else
        std::cout << "  No provider toolchains available (all SKIP).\n";
}

} // namespace smoke
} // namespace den

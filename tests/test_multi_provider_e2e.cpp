// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T60 — Multi-provider end-to-end harness.
//
// Verifies the acceptance criteria for T60 (wires upstream T23/T38/T39/T40/T41).
// Each provider section covers install → list → run → uninstall for one
// representative package. Tests are marked skip(true) (doctest decorator)
// until the relevant upstream target is implemented.
//
// Representative packages per provider:
//   Homebrew  : jq          — small, standalone, no deps, excellent smoke signal
//   Python    : black       — popular formatter; exercises pyvenv integration (🎯T38)
//   Node/npm  : prettier    — popular formatter; exercises node_modules/bin wiring (🎯T39)
//   Go        : golangci-lint — standard Go linter; go install path, static binary (🎯T40)
//   Cargo     : ripgrep     — well-known Rust tool; fast, no runtime deps (🎯T41)
//
// Isolation: every test case uses a fresh DEN_HOME under /tmp.
// The den binary is expected at ./den relative to the build directory.

#define DOCTEST_CONFIG_SUPER_FAST_ASSERTS
#include <doctest.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace den {
namespace multi_provider_test {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// TempDir — RAII unique temp directory removed on destruction.
// ---------------------------------------------------------------------------
struct TempDir {
    fs::path path;

    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_mp_XXXXXX").string();
        char* result = ::mkdtemp(tmpl.data());
        if (!result) {
            throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
        }
        path = result;
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// ---------------------------------------------------------------------------
// run_den — run the den binary with args and a given DEN_HOME, capturing
// combined stdout+stderr. Returns {exit_code, output}.
//
// Isolates the shared Cellar so host packages do not bleed into assertions.
// ---------------------------------------------------------------------------
static std::pair<int, std::string> run_den(const std::string& args, const fs::path& den_home) {
    const std::string prefix = den_home.string() + "/brew";
    const std::string cellar = prefix + "/Cellar";
    std::string cmd = "DEN_HOME=" + den_home.string() + " HOMEBREW_PREFIX=" + prefix +
                      " HOMEBREW_CELLAR=" + cellar + " ./den " + args + " 2>&1";

    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("popen failed: " + cmd);
    }

    std::string output;
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        output += buf.data();
    }
    int raw = ::pclose(pipe);
    int code = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
    return {code, output};
}

static std::string den_out(const std::string& args, const fs::path& den_home) {
    return run_den(args, den_home).second;
}

// ---------------------------------------------------------------------------
// Homebrew provider tests
//
// Package: jq
// Rationale: minimal binary (no runtime deps), available as a bottle on
// every Homebrew-supported platform, binary name == formula name,
// `jq --version` is deterministic.
//
// Skipped until: T23 (PackageProvider trait) is wired into uniform dispatch.
// The Homebrew install path exists but the provider-transparent `den install`
// surface is not yet hooked through a registry.
// ---------------------------------------------------------------------------
TEST_SUITE("multi_provider::homebrew") {

    // T23 not yet implemented — skip all homebrew provider uniformity tests.
    TEST_CASE("jq: install / list / run / uninstall via uniform den command" * doctest::skip(true) *
              doctest::description("Blocked on 🎯T23 (PackageProvider trait)")) {
        TempDir tmp;

        // install — plain package name, no "homebrew:" prefix.
        auto [install_rc, install_out] = run_den("install jq", tmp.path);
        CHECK(install_rc == 0);
        REQUIRE_MESSAGE(install_out.find("jq") != std::string::npos,
                        "install output should mention jq; got: " << install_out);

        // list — jq should appear.
        auto list_out = den_out("list", tmp.path);
        CHECK(list_out.find("jq") != std::string::npos);

        // run — jq --version prints "jq-<version>".
        auto [run_rc, run_out] = run_den("run jq --version", tmp.path);
        CHECK(run_rc == 0);
        CHECK(run_out.find("jq-") != std::string::npos);

        // uninstall.
        auto [uninst_rc, uninst_out] = run_den("uninstall jq", tmp.path);
        CHECK(uninst_rc == 0);
        (void)uninst_out;

        // list — jq must be gone.
        auto list_after = den_out("list", tmp.path);
        CHECK(list_after.find("jq") == std::string::npos);
    }

    TEST_CASE("info and uninstall accept plain package name with no provider prefix" *
              doctest::skip(true) *
              doctest::description("Blocked on 🎯T23 (PackageProvider trait)")) {
        // Acceptance: T60 requires that provider-specific syntax does NOT leak
        // into the user-facing command.  This test verifies that `den info jq`
        // produces output without requiring the user to type "homebrew:jq".
    }
}

// ---------------------------------------------------------------------------
// Python provider tests (🎯T38)
//
// Package: black
// Rationale: widely used; binary is `black`, no C-extension deps, and
// `black --version` is reliable.  Alternative: ruff if black is flakey in CI.
// ---------------------------------------------------------------------------
TEST_SUITE("multi_provider::python") {

    TEST_CASE("black: install / list / run / uninstall" * doctest::skip(true) *
              doctest::description("Blocked on 🎯T38 (Python provider), requires 🎯T23 first")) {
        TempDir tmp;

        // install — plain package name.
        auto [install_rc, install_out] = run_den("install black", tmp.path);
        CHECK(install_rc == 0);
        REQUIRE(install_out.find("black") != std::string::npos);

        // list — uniform listing shows black.
        auto list_out = den_out("list", tmp.path);
        CHECK(list_out.find("black") != std::string::npos);

        // run — binary is on PATH via env symlinks.
        auto [run_rc, run_out] = run_den("run black --version", tmp.path);
        CHECK(run_rc == 0);
        CHECK(run_out.find("black") != std::string::npos);

        // uninstall.
        auto [uninst_rc, _uninst_out] = run_den("uninstall black", tmp.path);
        CHECK(uninst_rc == 0);

        // list — gone.
        auto list_after = den_out("list", tmp.path);
        CHECK(list_after.find("black") == std::string::npos);
    }

    TEST_CASE("Python packages land in per-env lib/python3.X/site-packages" * doctest::skip(true) *
              doctest::description("Blocked on 🎯T38 (Python provider)")) {
        // When T38 lands, Python packages must be in the env-local site-packages,
        // not in the Homebrew Cellar. Verifies per-provider area invariant.
    }

    TEST_CASE("Python packages PATH-compose with Homebrew kegs" * doctest::skip(true) *
              doctest::description("Blocked on 🎯T38 (Python provider)")) {
        // `den init` must export a PATH where both Homebrew binaries and
        // Python-provider binaries are accessible from the same shell.
    }
}

// ---------------------------------------------------------------------------
// Node/npm provider tests (🎯T39)
//
// Package: prettier
// Rationale: CLI-first, no native addons, `prettier --version` is deterministic.
// ---------------------------------------------------------------------------
TEST_SUITE("multi_provider::node") {

    TEST_CASE("prettier: install / list / run / uninstall" * doctest::skip(true) *
              doctest::description("Blocked on 🎯T39 (Node/npm provider), requires 🎯T23 first")) {
        TempDir tmp;

        auto [install_rc, install_out] = run_den("install prettier", tmp.path);
        CHECK(install_rc == 0);
        REQUIRE(install_out.find("prettier") != std::string::npos);

        auto list_out = den_out("list", tmp.path);
        CHECK(list_out.find("prettier") != std::string::npos);

        // prettier --version prints a semver string — verify at least one digit.
        auto [run_rc, run_out] = run_den("run prettier --version", tmp.path);
        CHECK(run_rc == 0);
        bool has_digit = false;
        for (char c : run_out) {
            if (std::isdigit(static_cast<unsigned char>(c))) {
                has_digit = true;
                break;
            }
        }
        CHECK(has_digit);

        auto [uninst_rc, _uninst_out] = run_den("uninstall prettier", tmp.path);
        CHECK(uninst_rc == 0);

        auto list_after = den_out("list", tmp.path);
        CHECK(list_after.find("prettier") == std::string::npos);
    }

    TEST_CASE("Node packages land in per-env lib/node_modules" * doctest::skip(true) *
              doctest::description("Blocked on 🎯T39 (Node/npm provider)")) {
        // Packages installed via the npm provider must be isolated per
        // environment, not in a global node_modules directory.
    }
}

// ---------------------------------------------------------------------------
// Go provider tests (🎯T40)
//
// Package: golangci-lint
// Rationale: canonical Go tooling, statically linked, `golangci-lint --version`
// is deterministic.  Alternative: `gopls` or `staticcheck` if golangci-lint
// is too large for CI.
// ---------------------------------------------------------------------------
TEST_SUITE("multi_provider::go") {

    TEST_CASE("golangci-lint: install / list / run / uninstall" * doctest::skip(true) *
              doctest::description("Blocked on 🎯T40 (Go provider), requires 🎯T23 first")) {
        TempDir tmp;

        auto [install_rc, install_out] = run_den("install golangci-lint", tmp.path);
        CHECK(install_rc == 0);
        REQUIRE(install_out.find("golangci-lint") != std::string::npos);

        auto list_out = den_out("list", tmp.path);
        CHECK(list_out.find("golangci-lint") != std::string::npos);

        // golangci-lint --version prints "golangci-lint has version X.Y.Z".
        auto [run_rc, run_out] = run_den("run golangci-lint --version", tmp.path);
        CHECK(run_rc == 0);
        CHECK(run_out.find("golangci-lint") != std::string::npos);

        auto [uninst_rc, _uninst_out] = run_den("uninstall golangci-lint", tmp.path);
        CHECK(uninst_rc == 0);

        auto list_after = den_out("list", tmp.path);
        CHECK(list_after.find("golangci-lint") == std::string::npos);
    }

    TEST_CASE("Go binaries land in env-local GOBIN" * doctest::skip(true) *
              doctest::description("Blocked on 🎯T40 (Go provider)")) {
        // `go install` must write to an environment-local GOBIN, not ~/go/bin.
        // Verifies per-provider isolation.
    }
}

// ---------------------------------------------------------------------------
// Cargo provider tests (🎯T41)
//
// Package: ripgrep (crate name: ripgrep, binary: rg)
// Rationale: popular, statically linked Rust tool. Crate name ≠ binary name
// exercises provider metadata handling.
// ---------------------------------------------------------------------------
TEST_SUITE("multi_provider::cargo") {

    TEST_CASE("ripgrep: install / list / run / uninstall" * doctest::skip(true) *
              doctest::description("Blocked on 🎯T41 (Cargo provider), requires 🎯T23 first")) {
        TempDir tmp;

        // Crate name is "ripgrep"; the installed binary is "rg".
        auto [install_rc, install_out] = run_den("install ripgrep", tmp.path);
        CHECK(install_rc == 0);
        REQUIRE(install_out.find("ripgrep") != std::string::npos);

        // list — shows the crate name.
        auto list_out = den_out("list", tmp.path);
        CHECK(list_out.find("ripgrep") != std::string::npos);

        // run — binary is `rg`, not `ripgrep`; output contains "ripgrep".
        auto [run_rc, run_out] = run_den("run rg --version", tmp.path);
        CHECK(run_rc == 0);
        CHECK(run_out.find("ripgrep") != std::string::npos);

        auto [uninst_rc, _uninst_out] = run_den("uninstall ripgrep", tmp.path);
        CHECK(uninst_rc == 0);

        auto list_after = den_out("list", tmp.path);
        CHECK(list_after.find("ripgrep") == std::string::npos);
    }

    TEST_CASE("Cargo binaries land in env-local CARGO_INSTALL_ROOT" * doctest::skip(true) *
              doctest::description("Blocked on 🎯T41 (Cargo provider)")) {
        // Rust binaries must be installed into an environment-local root,
        // not ~/.cargo/bin. Verifies per-provider isolation.
    }
}

// ---------------------------------------------------------------------------
// Cross-provider invariants (T60 acceptance criteria)
// ---------------------------------------------------------------------------
TEST_SUITE("multi_provider::invariants") {

    TEST_CASE("den list shows packages from all providers uniformly" * doctest::skip(true) *
              doctest::description("Blocked on 🎯T23 (PackageProvider trait)")) {
        // After installing jq (Homebrew), black (pip), prettier (npm),
        // golangci-lint (go), and ripgrep (cargo), `den list` must show all
        // five without any provider-specific prefix in the package name column.
    }

    TEST_CASE("den info works for packages from each provider" * doctest::skip(true) *
              doctest::description("Blocked on 🎯T23 (PackageProvider trait)")) {
        // `den info <name>` must return metadata regardless of which provider
        // owns the package. No provider-specific syntax in the command.
    }

    TEST_CASE("provider-specific syntax does not appear in user-facing commands" *
              doctest::skip(true) *
              doctest::description("Blocked on 🎯T23 (PackageProvider trait)")) {
        // Acceptance: `den install jq` installs jq; the user need not write
        // `homebrew:jq` or `pip:black`. Provider detection is automatic.
    }

    TEST_CASE("packages from each provider PATH-compose via env symlinks" * doctest::skip(true) *
              doctest::description("Blocked on 🎯T23/T38/T39/T40/T41")) {
        // All providers produce binaries in the active env's bin/ directory
        // (or via symlinks), so a single PATH entry covers all providers.
    }
}

} // namespace multi_provider_test
} // namespace den

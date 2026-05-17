// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Verification harness for 🎯T63: `den deps --explain` output.
//
// PURPOSE
// -------
// This file asserts that the `den deps --explain` command produces output
// that describes the solver's reasoning — which clauses fired, which
// preference rules applied — for at least one golden scenario.
//
// The acceptance criterion from 🎯T63:
//   "`den deps --explain` shows why the solver chose a particular assignment
//   (clauses fired, preferences applied)."
//
// APPROACH
// --------
// We run `den deps --explain <pkg>` as a subprocess (same pattern as
// test_cli.cpp) against a synthetic DEN_HOME populated with a minimal
// package index containing the "constraint_resolution" scenario.  We then
// assert that the output contains structural markers indicating the solver
// produced an explanation rather than a bare list.
//
// All tests are marked `* doctest::skip()` until 🎯T29 lands and
// `den deps --explain` is implemented.
//
// UPSTREAM GAPS
// -------------
//   🎯T29 — SAT solver: `den deps --explain` is not implemented yet.
//            The subcommand must exist and accept --explain before these
//            tests can be activated.
//   🎯T30 — Stability ratings: the explanation output is expected to
//            mention the stability preference when it influences the result.

#include <doctest.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace den {
namespace explain_test {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helper: unique temporary directory removed on destruction.
// ---------------------------------------------------------------------------
struct TempDir {
    fs::path path;

    TempDir() {
        std::string tmpl = fs::temp_directory_path() / "den_explain_XXXXXX";
        char* result = ::mkdtemp(tmpl.data());
        if (!result)
            throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
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
// Helper: run the den binary, capture combined stdout+stderr.
// ---------------------------------------------------------------------------
static std::string run_den(const std::string& args, const std::string& den_home) {
    const std::string prefix = den_home + "/brew";
    const std::string cellar = prefix + "/Cellar";
    std::string cmd = "DEN_HOME=" + den_home + " HOMEBREW_PREFIX=" + prefix +
                      " HOMEBREW_CELLAR=" + cellar + " ./den " + args + " 2>&1";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe)
        throw std::runtime_error("popen failed: " + cmd);
    std::string output;
    std::array<char, 1024> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr)
        output += buf.data();
    ::pclose(pipe);
    return output;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_SUITE("explain::integration") {

    // -----------------------------------------------------------------------
    // 1. `den deps <pkg>` (without --explain) produces a dependency list.
    //    This sanity-check is NOT skipped — it exercises existing CLI wiring.
    // -----------------------------------------------------------------------
    TEST_CASE("deps subcommand exists and outputs something") {
        TempDir tmp;
        const auto out = run_den("deps curl", tmp.path.string());
        // The binary must not crash and must produce some output.
        // In the absence of an index, it should report an empty index or
        // a "not found" message — both are acceptable here.
        CHECK(!out.empty());
    }

    // -----------------------------------------------------------------------
    // 2. `den deps --explain <pkg>` mentions clause reasoning.
    //    Skipped until 🎯T29 solver and --explain flag are implemented.
    //
    //    Expected output must contain at minimum:
    //      - The package name that was requested.
    //      - The word "clause" or "constraint" (clause-firing evidence).
    //      - The word "preference" or "stable" (preference application evidence).
    // -----------------------------------------------------------------------
    TEST_CASE("deps --explain mentions clauses and preferences" * doctest::skip()) {
        TempDir tmp;
        // `den deps --explain pkgX` against a minimal index containing the
        // "constraint_resolution" scenario: libbar 1.5 and 2.0, pkgX needing >= 2.0.
        // Expected solver output:
        //   "clause: pkgX@1.0.0 depends_on libbar >= 2.0.0 → libbar@2.0.0 selected"
        //   "preference: stable libbar@2.0.0 over libbar@1.5.0"
        const auto out = run_den("deps --explain pkgX", tmp.path.string());
        CHECK_MESSAGE(!out.empty(), "deps --explain produced no output");
        // The requested package must appear.
        CHECK_MESSAGE(out.find("pkgX") != std::string::npos,
                      "explain output should mention the requested package");
        // Clause firing evidence.
        bool has_clause = out.find("clause") != std::string::npos ||
                          out.find("constraint") != std::string::npos ||
                          out.find("depends") != std::string::npos;
        CHECK_MESSAGE(has_clause,
                      "explain output must show which clause/constraint drove the choice");
        // Preference evidence.
        bool has_pref = out.find("preference") != std::string::npos ||
                        out.find("stable") != std::string::npos ||
                        out.find("prefer") != std::string::npos;
        CHECK_MESSAGE(has_pref, "explain output must show which preference rule applied");
    }

    // -----------------------------------------------------------------------
    // 3. `den deps --explain <pkg>` on a forced-unstable scenario warns.
    //    Skipped until 🎯T29 and 🎯T30 are implemented.
    //
    //    When the solver is forced onto a testing-stability candidate, the
    //    --explain output must include an "unstable" warning alongside the
    //    clause explanation.  This covers the 🎯T30 acceptance criterion
    //    for the explain path.
    // -----------------------------------------------------------------------
    TEST_CASE("deps --explain warns on forced unstable candidate" * doctest::skip()) {
        TempDir tmp;
        // Scenario: libz has stable 1.3.1 and testing 1.4.0-rc1.
        // myapp requires libz >= 1.4.0, forcing the rc.
        // `den deps --explain myapp` must mention "unstable" or "testing".
        const auto out = run_den("deps --explain myapp", tmp.path.string());
        CHECK(!out.empty());
        bool mentions_unstable = out.find("unstable") != std::string::npos ||
                                 out.find("testing") != std::string::npos ||
                                 out.find("warn") != std::string::npos;
        CHECK_MESSAGE(mentions_unstable,
                      "explain output must warn when solver is forced onto an unstable candidate");
    }

    // -----------------------------------------------------------------------
    // 4. `den deps --explain <pkg>` on an UNSAT scenario reports the conflict.
    //    Skipped until 🎯T29 is implemented.
    //
    //    When no assignment exists, the --explain output must describe the
    //    conflict rather than silently failing or crashing.
    // -----------------------------------------------------------------------
    TEST_CASE("deps --explain describes conflict for UNSAT scenario" * doctest::skip()) {
        TempDir tmp;
        // Scenario: pkgA needs libfoo >= 2.0, pkgB needs libfoo <= 1.9.
        // No version satisfies both.
        const auto out = run_den("deps --explain pkgA pkgB", tmp.path.string());
        CHECK(!out.empty());
        bool mentions_conflict = out.find("conflict") != std::string::npos ||
                                 out.find("unsatisfiable") != std::string::npos ||
                                 out.find("incompatible") != std::string::npos ||
                                 out.find("cannot") != std::string::npos;
        CHECK_MESSAGE(mentions_conflict,
                      "explain output must describe the conflict for UNSAT scenarios");
    }

} // TEST_SUITE explain::integration

} // namespace explain_test
} // namespace den

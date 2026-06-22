// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Infrastructure smoke test for the den-vs-brew benchmark suite (🎯T68).
//
// These checks assert the suite's *contract* exists — driver script, results
// directory, regression checker, a committed baseline snapshot, and the CI
// workflow — without running the benchmark itself (which needs hyperfine, brew
// and a network).  Absence of any piece is caught by CI.

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace {

// True iff the owner-execute bit is set on `p`.
bool is_executable(const fs::path& p) {
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0)
        return false;
    return (st.st_mode & S_IXUSR) != 0;
}

} // namespace

// DEN_CORPUS_DIR is defined by CMakeLists.txt as <repo_root>/tests/corpus/...
// We derive the repo root from it.
#ifndef DEN_CORPUS_DIR
#error "DEN_CORPUS_DIR must be defined by the build system"
#endif

static fs::path repo_root() {
    // DEN_CORPUS_DIR == <repo_root>/tests/corpus/homebrew-core
    return fs::path(DEN_CORPUS_DIR).parent_path().parent_path().parent_path();
}

TEST_SUITE("bench_suite_present") {

    TEST_CASE("compare-brew.sh exists") {
        auto script = repo_root() / "scripts" / "bench" / "compare-brew.sh";
        CHECK_MESSAGE(fs::exists(script), "bench script missing: ", script.string());
    }

    TEST_CASE("compare-brew.sh is executable") {
        auto script = repo_root() / "scripts" / "bench" / "compare-brew.sh";
        REQUIRE(fs::exists(script));
        CHECK_MESSAGE(is_executable(script), "bench script is not executable: ", script.string());
    }

    TEST_CASE("compare-brew.sh measures all five ops") {
        // The driver must reference every op in the T68 acceptance criteria —
        // guards against a regression that drops install/upgrade again.
        auto script = repo_root() / "scripts" / "bench" / "compare-brew.sh";
        REQUIRE(fs::exists(script));
        std::ifstream in(script);
        std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (const std::string op : {"list", "info", "search", "install", "upgrade"}) {
            // The op must appear either as a quoted driver argument
            // (e.g. run_op "search") or as a display label (op: install).
            bool present = body.find("\"" + op + "\"") != std::string::npos ||
                           body.find("op: " + op) != std::string::npos;
            CHECK_MESSAGE(present, "bench script does not measure op: ", op);
        }
    }

    TEST_CASE("compare-results.sh exists and is executable") {
        auto script = repo_root() / "scripts" / "bench" / "compare-results.sh";
        REQUIRE_MESSAGE(fs::exists(script), "regression checker missing: ", script.string());
        CHECK_MESSAGE(is_executable(script),
                      "regression checker is not executable: ", script.string());
    }

    TEST_CASE("bench results directory exists") {
        auto results_dir = repo_root() / "bench" / "results";
        CHECK_MESSAGE(fs::is_directory(results_dir),
                      "bench/results/ directory missing: ", results_dir.string());
    }

    TEST_CASE("a committed baseline snapshot exists") {
        // The "recorded over time" criterion requires at least one committed
        // snapshot so a trend has a starting point.
        auto results_dir = repo_root() / "bench" / "results";
        REQUIRE(fs::is_directory(results_dir));
        bool found = false;
        for (const auto& e : fs::directory_iterator(results_dir)) {
            const auto name = e.path().filename().string();
            if (name.rfind("bench-", 0) == 0 && e.path().extension() == ".json") {
                found = true;
                break;
            }
        }
        CHECK_MESSAGE(found, "no committed bench-*.json baseline in: ", results_dir.string());
    }

    TEST_CASE("bench workflow file exists") {
        auto wf = repo_root() / ".github" / "workflows" / "bench.yml";
        CHECK_MESSAGE(fs::exists(wf), "bench workflow missing: ", wf.string());
    }

} // TEST_SUITE

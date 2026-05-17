// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Trivial smoke test: the benchmark driver script must exist at the expected
// path relative to the repo root and be marked executable.  Absence is caught
// by CI without needing to actually run the benchmark.

#include <doctest.h>

#include <filesystem>
#include <sys/stat.h>

namespace fs = std::filesystem;

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

        struct stat st {};
        REQUIRE(::stat(script.c_str(), &st) == 0);
        // Owner-execute bit must be set.
        CHECK_MESSAGE((st.st_mode & S_IXUSR) != 0,
                      "bench script is not executable: ", script.string());
    }

    TEST_CASE("bench results directory exists") {
        auto results_dir = repo_root() / "bench" / "results";
        CHECK_MESSAGE(fs::is_directory(results_dir),
                      "bench/results/ directory missing: ", results_dir.string());
    }

    TEST_CASE("bench workflow file exists") {
        auto wf = repo_root() / ".github" / "workflows" / "bench.yml";
        CHECK_MESSAGE(fs::exists(wf),
                      "bench workflow missing: ", wf.string());
    }

} // TEST_SUITE

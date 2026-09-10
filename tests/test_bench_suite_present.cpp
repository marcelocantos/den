// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Infrastructure smoke test for the den-vs-brew benchmark suite (🎯T68).
//
// These checks assert the suite's *contract* exists — driver script, results
// directory, regression checker, a committed baseline snapshot, and the CI
// workflow — and that compare-results.sh is host-aware (cross-host
// regression must not fail T68). The live hyperfine run itself is not
// executed here (needs brew + a network).

#include <doctest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// True iff the owner-execute bit is set on `p`.
bool is_executable(const fs::path& p) {
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0)
        return false;
    return (st.st_mode & S_IXUSR) != 0;
}

struct CmdResult {
    std::string output;
    int exit_code = -1;
};

CmdResult run_cmd(const std::string& cmd) {
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("popen failed: " + cmd);
    }
    std::string output;
    std::array<char, 1024> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        output += buf.data();
    }
    const int status = ::pclose(pipe);
    CmdResult r;
    r.output = output;
    if (WIFEXITED(status)) {
        r.exit_code = WEXITSTATUS(status);
    }
    return r;
}

// Minimal paired snapshot: one den + one brew row for `list`.
// Empty `host` omits the field (legacy snapshots from before host tagging).
std::string bench_pair_json(const std::string& host, double den_s, double brew_s) {
    const std::string host_field = host.empty() ? "" : "\"host\":\"" + host + "\",";
    auto row = [&](const char* tool, double mean) {
        return std::string("  {\"timestamp\":\"t\",") + host_field + "\"op\":\"list\",\"tool\":\"" +
               tool + "\",\"mean_s\":" + std::to_string(mean) + ",\"stddev_s\":0,\"min_s\":" +
               std::to_string(mean) + ",\"max_s\":" + std::to_string(mean) +
               ",\"status\":\"ok\"}";
    };
    return "[\n" + row("den-list", den_s) + ",\n" + row("brew-list", brew_s) + "\n]\n";
}

void write_file(const fs::path& p, const std::string& body) {
    std::ofstream out(p);
    out << body;
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

    TEST_CASE("compare-brew.sh records a host class on snapshots") {
        auto script = repo_root() / "scripts" / "bench" / "compare-brew.sh";
        REQUIRE(fs::exists(script));
        std::ifstream in(script);
        std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK_MESSAGE(body.find("HOST_ID") != std::string::npos,
                      "compare-brew.sh must record a HOST_ID on each snapshot row");
        CHECK_MESSAGE(body.find("GITHUB_ACTIONS") != std::string::npos,
                      "compare-brew.sh must distinguish GitHub Actions from local hosts");
    }

    TEST_CASE("compare-results.sh skips cross-host regression") {
        if (::system("jq --version >/dev/null 2>&1") != 0) {
            WARN("jq not available — skipping compare-results.sh host-class tests");
            return;
        }

        auto script = repo_root() / "scripts" / "bench" / "compare-results.sh";
        REQUIRE(fs::exists(script));

        std::string tmpl = (fs::temp_directory_path() / "den_bench_XXXXXX").string();
        char* dir = ::mkdtemp(tmpl.data());
        REQUIRE(dir != nullptr);
        const fs::path tmp = dir;
        const auto cleanup = [&]() {
            std::error_code ec;
            fs::remove_all(tmp, ec);
        };

        // den still beats brew (0.10s vs 0.40s) but is 100% slower than the
        // 0.05s baseline — enough to trip the default 25% regression gate
        // if the hosts were treated as comparable.
        const fs::path snap = tmp / "snap.json";
        const fs::path base = tmp / "base.json";
        write_file(snap, bench_pair_json("gha-macos14-ARM64", 0.10, 0.40));
        write_file(base, bench_pair_json("Darwin-arm64-local", 0.05, 0.40));

        const std::string cmd = script.string() + " \"" + snap.string() +
                                "\" --baseline \"" + base.string() + "\" 2>&1";
        const auto r = run_cmd(cmd);
        cleanup();

        CHECK_MESSAGE(r.exit_code == 0,
                      "cross-host regression must be skipped, not fail T68:\n", r.output);
        const bool skipped = r.output.find("skipping regression") != std::string::npos ||
                             r.output.find("regression check skipped") != std::string::npos;
        CHECK_MESSAGE(skipped, "expected a skip message, got:\n", r.output);
        CHECK_MESSAGE(r.output.find("REGRESSED") == std::string::npos,
                      "cross-host delta must not be reported as REGRESSED:\n", r.output);
    }

    TEST_CASE("compare-results.sh skips untagged local baseline vs tagged CI snapshot") {
        // Recreates run 34113774338: CI snapshot (new, host-tagged) vs the
        // committed M4 Max baseline (no host field). Absolute ms differ by
        // ~50–80%; that must not fail T68.
        if (::system("jq --version >/dev/null 2>&1") != 0) {
            WARN("jq not available — skipping compare-results.sh host-class tests");
            return;
        }

        auto script = repo_root() / "scripts" / "bench" / "compare-results.sh";
        REQUIRE(fs::exists(script));

        std::string tmpl = (fs::temp_directory_path() / "den_bench_XXXXXX").string();
        char* dir = ::mkdtemp(tmpl.data());
        REQUIRE(dir != nullptr);
        const fs::path tmp = dir;
        const auto cleanup = [&]() {
            std::error_code ec;
            fs::remove_all(tmp, ec);
        };

        const fs::path snap = tmp / "snap.json";
        const fs::path base = tmp / "base.json";
        write_file(snap, bench_pair_json("gha-macos14-ARM64", 0.235, 0.403));
        write_file(base, bench_pair_json("", 0.131, 0.779));

        const std::string cmd = script.string() + " \"" + snap.string() +
                                "\" --baseline \"" + base.string() + "\" 2>&1";
        const auto r = run_cmd(cmd);
        cleanup();

        CHECK_MESSAGE(r.exit_code == 0,
                      "untagged local baseline vs CI snapshot must skip regression:\n",
                      r.output);
        CHECK_MESSAGE(r.output.find("REGRESSED") == std::string::npos,
                      "legacy untagged baseline must not REGRESS a CI run:\n", r.output);
    }

    TEST_CASE("compare-results.sh still fails same-host regressions") {
        if (::system("jq --version >/dev/null 2>&1") != 0) {
            WARN("jq not available — skipping compare-results.sh host-class tests");
            return;
        }

        auto script = repo_root() / "scripts" / "bench" / "compare-results.sh";
        REQUIRE(fs::exists(script));

        std::string tmpl = (fs::temp_directory_path() / "den_bench_XXXXXX").string();
        char* dir = ::mkdtemp(tmpl.data());
        REQUIRE(dir != nullptr);
        const fs::path tmp = dir;
        const auto cleanup = [&]() {
            std::error_code ec;
            fs::remove_all(tmp, ec);
        };

        const fs::path snap = tmp / "snap.json";
        const fs::path base = tmp / "base.json";
        write_file(snap, bench_pair_json("gha-macos14-ARM64", 0.10, 0.40));
        write_file(base, bench_pair_json("gha-macos14-ARM64", 0.05, 0.40));

        const std::string cmd = script.string() + " \"" + snap.string() +
                                "\" --baseline \"" + base.string() + "\" 2>&1";
        const auto r = run_cmd(cmd);
        cleanup();

        CHECK_MESSAGE(r.exit_code != 0,
                      "same-host +25% regression must still fail T68:\n", r.output);
        CHECK_MESSAGE(r.output.find("REGRESSED") != std::string::npos,
                      "expected REGRESSED verdict, got:\n", r.output);
    }

} // TEST_SUITE

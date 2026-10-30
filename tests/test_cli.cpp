// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Integration tests that invoke the den binary and inspect its output.
// Each test creates a fresh temporary DEN_HOME so runs are isolated.

#include <doctest.h>

#include "store/store.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace den {
namespace cli_test {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helper: unique temporary directory that is removed on destruction.
// ---------------------------------------------------------------------------
struct TempDir {
    fs::path path;

    TempDir() {
        // mkdtemp requires a writable template.
        std::string tmpl = fs::temp_directory_path() / "den_cli_XXXXXX";
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

    // Non-copyable.
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// ---------------------------------------------------------------------------
// Helper: run the den binary with the given args and den_home, capturing
// combined stdout+stderr.  The binary is expected at ./den relative to the
// working directory (the build directory when run via ctest).
//
// Tests pin DEN_PREFIX (den's store) and HOMEBREW_PREFIX/CELLAR (where den
// believes Homebrew lives, used by migrate/diagnostics) into the per-test
// DEN_HOME, so neither the store nor brew detection sees host state.
// ---------------------------------------------------------------------------
static std::string run_den(const std::string& args, const std::string& den_home) {
    // Build command: redirect stderr to stdout so we capture everything.
    // Point den's prefix and the Homebrew location into the per-test
    // DEN_HOME. The directories don't need to pre-exist — den
    // tolerates a missing store for read-only queries.
    const std::string prefix = den_home + "/brew";
    const std::string cellar = prefix + "/Cellar";
    std::string cmd = "DEN_HOME=" + den_home + " DEN_PREFIX=" + prefix +
                      " HOMEBREW_PREFIX=" + prefix + " HOMEBREW_CELLAR=" + cellar + " ./den " +
                      args + " 2>&1";

    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("popen failed for: " + cmd);
    }

    std::string output;
    std::array<char, 1024> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        output += buf.data();
    }
    ::pclose(pipe);
    return output;
}

// Convenience overload that creates a fresh TempDir automatically.
static std::string run_den_fresh(const std::string& args) {
    TempDir tmp;
    return run_den(args, tmp.path.string());
}

struct DenResult {
    std::string output; // combined stdout + stderr
    int exit_code;      // raw process exit code (from WEXITSTATUS)
};

// Run den with an explicit store. den's Cellar is always <DEN_PREFIX>/Cellar,
// so `cellar` must end in "/Cellar"; its parent becomes DEN_PREFIX.
static DenResult run_den_with_cellar(const std::string& args, const std::string& den_home,
                                     const std::string& cellar) {
    REQUIRE(fs::path(cellar).filename() == "Cellar");
    const std::string prefix = fs::path(cellar).parent_path().string();
    std::string cmd = "DEN_HOME=" + den_home + " DEN_PREFIX=" + prefix +
                      " HOMEBREW_PREFIX=" + prefix + " HOMEBREW_CELLAR=" + cellar + " ./den " +
                      args + " 2>&1";

    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("popen failed for: " + cmd);
    }

    std::string output;
    std::array<char, 1024> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        output += buf.data();
    }
    int raw = ::pclose(pipe);
    int exit_code = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
    return {output, exit_code};
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_SUITE("cli::integration") {

    // -----------------------------------------------------------------------
    // 1. --version
    // -----------------------------------------------------------------------
    TEST_CASE("--version contains the expected version string") {
        const auto out = run_den_fresh("--version");
        CHECK(out.find(DEN_VERSION) != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 2. --help
    // -----------------------------------------------------------------------
    TEST_CASE("--help contains description and subcommand list") {
        const auto out = run_den_fresh("--help");
        // CLI11 description string from Cli::M::app.
        CHECK(out.find("universal development environment manager") != std::string::npos);
        // A representative selection of subcommands that should be listed.
        CHECK(out.find("install") != std::string::npos);
        CHECK(out.find("list") != std::string::npos);
        CHECK(out.find("env") != std::string::npos);
        CHECK(out.find("settings") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 3. --help-agent
    // -----------------------------------------------------------------------
    TEST_CASE("--help-agent includes agent guide text") {
        const auto out = run_den_fresh("--help-agent");
        // The agent guide marker written in cli.cpp.
        CHECK(out.find("agents-guide.md") != std::string::npos);
        // Should also include the regular help content.
        CHECK(out.find("universal development environment manager") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 4. config
    // -----------------------------------------------------------------------
    TEST_CASE("config shows den_home and arch") {
        TempDir tmp;
        const auto out = run_den("config", tmp.path.string());
        CHECK(out.find("den_home:") != std::string::npos);
        CHECK(out.find("arch:") != std::string::npos);
        // den_home line should include the temp path.
        CHECK(out.find(tmp.path.string()) != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 5. settings — fresh home returns valid JSON with daemon.auto_download
    // -----------------------------------------------------------------------
    TEST_CASE("settings on fresh home returns JSON with daemon.auto_download") {
        const auto out = run_den_fresh("settings");
        // Must be parseable-looking JSON (starts with '{').
        bool starts_with_brace = false;
        for (char c : out) {
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
                continue;
            starts_with_brace = (c == '{');
            break;
        }
        CHECK(starts_with_brace);
        // Default settings include these keys.
        CHECK(out.find("auto_download") != std::string::npos);
        CHECK(out.find("daemon") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 6. set + settings
    // Note: setting a new boolean key via `set` stores it as a JSON string
    // because the key does not yet exist in config.json at write time, so type
    // inference falls back to string.  The stored value "true" is a string and
    // is NOT picked up by the boolean deserialiser in read_settings.  This test
    // documents the current behaviour (set echoes the assignment; the key is
    // present in config.json even if the settings display still shows the
    // default).
    // -----------------------------------------------------------------------
    TEST_CASE("set echoes the key=value assignment") {
        TempDir tmp;
        const auto set_out = run_den("set daemon.auto_upgrade true", tmp.path.string());
        // `set` should confirm the write.
        CHECK(set_out.find("daemon.auto_upgrade") != std::string::npos);
        CHECK(set_out.find("true") != std::string::npos);
    }

    TEST_CASE("settings after set still returns valid JSON") {
        TempDir tmp;
        run_den("set daemon.auto_upgrade true", tmp.path.string());
        const auto out = run_den("settings", tmp.path.string());
        CHECK(out.find("daemon") != std::string::npos);
        CHECK(out.find("auto_download") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 6b. get — exit 0 with the value, 1 when unset, 2 for a damaged
    // config.json (install.sh relies on this split to persist DEN_PREFIX).
    // -----------------------------------------------------------------------
    TEST_CASE("get store.prefix: unset, set, and damaged config") {
        TempDir tmp;
        const std::string home = tmp.path.string();
        auto r = run_den_with_cellar("get store.prefix", home, home + "/p/Cellar");
        CHECK(r.exit_code == 1);
        CHECK(r.output.find("store.prefix is not set") != std::string::npos);

        run_den("set store.prefix /x/prefix", home);
        r = run_den_with_cellar("get store.prefix", home, home + "/p/Cellar");
        CHECK(r.exit_code == 0);
        CHECK(r.output == "/x/prefix\n");

        {
            std::ofstream f(tmp.path / "config.json");
            f << "{not json";
        }
        r = run_den_with_cellar("get store.prefix", home, home + "/p/Cellar");
        CHECK(r.exit_code == 2);
        CHECK(r.output.find("not valid JSON") != std::string::npos);
        r = run_den_with_cellar("set store.prefix /y", home, home + "/p/Cellar");
        CHECK(r.exit_code != 0);
        std::ifstream f(tmp.path / "config.json");
        std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(body == "{not json");
    }

    // -----------------------------------------------------------------------
    // 7. env create + list + remove lifecycle
    // -----------------------------------------------------------------------
    TEST_CASE("env create / list / remove lifecycle") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        // Create.
        const auto create_out = run_den("env create /test", home);
        CHECK(create_out.find("Created") != std::string::npos);
        CHECK(create_out.find("/test") != std::string::npos);

        // List shows the new environment.
        const auto list_out = run_den("env list", home);
        CHECK(list_out.find("/test") != std::string::npos);

        // Remove.
        const auto remove_out = run_den("env remove /test", home);
        CHECK(remove_out.find("Removed") != std::string::npos);

        // List no longer shows it.
        const auto list_after = run_den("env list", home);
        CHECK(list_after.find("/test") == std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 8. env list on empty home returns "No environments."
    // -----------------------------------------------------------------------
    TEST_CASE("env list on fresh home says no environments") {
        const auto out = run_den_fresh("env list");
        CHECK(out.find("No environments") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 9. env show — active environment with no packages
    // -----------------------------------------------------------------------
    TEST_CASE("env show on empty environment says no packages") {
        const auto out = run_den_fresh("env show");
        CHECK(out.find("no packages") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 10. env freeze — stub exits cleanly (not yet implemented)
    // -----------------------------------------------------------------------
    TEST_CASE("env freeze outputs JSON lockfile") {
        const auto out = run_den_fresh("env freeze");
        CHECK(out.find("environment") != std::string::npos);
        CHECK(out.find("packages") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 11. status — stub exits cleanly
    // -----------------------------------------------------------------------
    TEST_CASE("status shows environment and daemon info") {
        const auto out = run_den_fresh("status");
        CHECK(out.find("Active environment:") != std::string::npos);
        CHECK(out.find("Daemon:") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 12. daemon status — shows running state and auto-download setting
    // -----------------------------------------------------------------------
    TEST_CASE("daemon status shows daemon state and settings") {
        const auto out = run_den_fresh("daemon status");
        CHECK(out.find("Daemon:") != std::string::npos);
        // Daemon should not be running in a fresh temp home.
        CHECK(out.find("not running") != std::string::npos);
        CHECK(out.find("Auto-download:") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 13. list — no packages installed
    // -----------------------------------------------------------------------
    TEST_CASE("list on empty store says no packages") {
        const auto out = run_den_fresh("list");
        CHECK(out.find("No packages installed") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 14. search — no index cached
    // -----------------------------------------------------------------------
    TEST_CASE("search without index reports empty index") {
        const auto out = run_den_fresh("search python");
        CHECK(out.find("Index is empty") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 15. self-update --check — version probe: no download, no binary replacement
    // -----------------------------------------------------------------------
    TEST_CASE("self-update --check prints a version line without downloading") {
        // This test contacts the GitHub Releases API.  If the network is
        // unavailable the command may print "Unable to determine the latest
        // version." — that is still a valid, non-crashing result.
        const auto out = run_den_fresh("self-update --check");

        // Must contain "den" on the one-line output.
        CHECK(out.find("den") != std::string::npos);

        // Must match one of:
        //   "den X.Y.Z is the latest version."
        //   "den X.Y.Z is available (current: ...)"
        //   "Unable to determine the latest version."  (network absent)
        const bool up_to_date = out.find("is the latest version") != std::string::npos;
        const bool update_avail = out.find("is available") != std::string::npos;
        const bool no_network = out.find("Unable to determine") != std::string::npos;
        CHECK((up_to_date || update_avail || no_network));

        // Must NOT have started a download.
        CHECK(out.find("Downloading") == std::string::npos);
    }

    TEST_CASE("self-update --dry-run is accepted as an alias for --check") {
        // --dry-run is an alias for --check.  Verify it is parsed without error
        // (CLI11 would exit non-zero and emit "unknown option" otherwise).
        const auto out = run_den_fresh("self-update --dry-run");
        CHECK(out.find("unknown option") == std::string::npos);
        CHECK(out.find("Downloading") == std::string::npos);
    }

    TEST_CASE("self-update --version rejects a malformed version before any network access") {
        const auto out = run_den_fresh("self-update --version not-a-version");
        CHECK(out.find("invalid release version") != std::string::npos);
        CHECK(out.find("Downloading") == std::string::npos);
        CHECK(out.find("unknown option") == std::string::npos);
    }

    TEST_CASE("self-update --check --version probes a tag without downloading") {
        // Contacts the GitHub Releases API for v0.14.0 (a published GA). With
        // no network the command fails with "could not fetch release".
        const auto out = run_den_fresh("self-update --check --version 0.14.0");
        const bool found = out.find("den v0.14.0 is available") != std::string::npos;
        const bool no_network = out.find("could not fetch release") != std::string::npos ||
                                out.find("has no den binary") != std::string::npos;
        CHECK((found || no_network));
        CHECK(out.find("Downloading") == std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 16. doctor — runs without crashing, reports findings or passes
    // -----------------------------------------------------------------------
    TEST_CASE("doctor runs and produces a summary") {
        const auto out = run_den_fresh("doctor");
        // Either "finding(s)" (issues found) or "All checks passed."
        const bool has_findings = out.find("finding(s)") != std::string::npos;
        const bool all_passed = out.find("All checks passed") != std::string::npos;
        CHECK((has_findings || all_passed));
    }

    TEST_CASE("doctor on fresh home reports missing directory warnings") {
        // A brand-new temp home will trigger warnings for the missing den_home
        // directory structure.
        const auto out = run_den_fresh("doctor");
        CHECK(out.find("finding(s)") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 16. install with blocked Cellar — no SIGABRT (regression for crash fix)
    //
    // Before the fix, den aborted with SIGABRT (exit 134) when
    // fs::create_directories threw std::filesystem_error because the Cellar
    // parent didn't exist or wasn't writable.  After the fix:
    //   - The exception is caught and converted to a UserError with a clear
    //     message, or the install proceeds if the store is auto-creatable.
    //   - Exit code is 1 (clean failure), never 134 (abort).
    //   - Output contains "error:" or "fatal:", not "terminate called".
    //
    // We trigger the original scenario by placing a regular file at the Cellar
    // path, making fs::create_directories(<cellar>/<pkg_name>) fail with
    // ENOTDIR.  A minimal index.json with an "all"-tagged archive ensures the
    // code reaches the install path (past the "index is empty" guard) before
    // the network fetch fails (fake URL) or the directory creation fails.
    // Either way the process must exit cleanly.
    // -----------------------------------------------------------------------
    TEST_CASE("install with blocked Cellar exits cleanly, not with SIGABRT") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        // Seed a minimal index.json so the "index is empty" guard passes.
        // The package has an "all" archive tag that matches every platform;
        // the URL is intentionally fake — the test only needs the code to
        // reach the Cellar creation step (or fail at download with a clean
        // error), not to complete a real install.
        const std::string cache_dir = home + "/cache";
        fs::create_directories(cache_dir);
        std::ofstream idx(cache_dir + "/index.json");
        idx << R"({
  "packages": [{
    "name": "den-test-pkg",
    "version": "1.0.0",
    "description": "test",
    "homepage": "",
    "license": "",
    "artifact_type": "binary",
    "deprecated": false,
    "disabled": false,
    "dependencies": [],
    "build_dependencies": [],
    "archives": {
      "all": {
        "url": "file:///nonexistent/den-test-pkg-1.0.0.tar.gz",
        "sha256": "0000000000000000000000000000000000000000000000000000000000000000",
        "relocation": "any_cellar"
      }
    }
  }]
})";
        idx.close();

        // Block the Cellar path by placing a regular file there.
        // fs::create_directories(<cellar>/<pkg_name>) will fail with ENOTDIR.
        fs::create_directories(home + "/blocked");
        const std::string blocked_cellar = home + "/blocked/Cellar";
        std::ofstream(blocked_cellar) << "not a directory\n";

        auto result = run_den_with_cellar("install den-test-pkg", home, blocked_cellar);

        // Must NOT be SIGABRT (exit 134).
        CHECK(result.exit_code != 134);
        // Must be a clean non-zero exit.
        CHECK(result.exit_code != 0);
        // Must NOT contain the raw C++ exception message that indicates abort.
        CHECK(result.output.find("terminate called") == std::string::npos);
        CHECK(result.output.find("std::filesystem") == std::string::npos);
        // Should contain a human-readable error prefix.
        const bool has_error = result.output.find("error:") != std::string::npos ||
                               result.output.find("fatal:") != std::string::npos ||
                               result.output.find("cannot") != std::string::npos;
        CHECK(has_error);
    }

    // -----------------------------------------------------------------------
    // 16b. Unresolvable packages exit non-zero. `den install` with no index
    //      and `den info` on a missing package used to log a message and exit
    //      0, so scripts could not tell failure from success.
    // -----------------------------------------------------------------------
    TEST_CASE("install with an empty index exits non-zero with a clear message") {
        TempDir home;
        const std::string cellar = (home.path / "brew" / "Cellar").string();

        auto r = run_den_with_cellar("install nosuchpkg", home.path.string(), cellar);
        CHECK(r.exit_code == 1);
        CHECK(r.output.find("error: the package index is empty; run `den update` first") !=
              std::string::npos);
        CHECK_FALSE(fs::exists(home.path / "envs" / "default" / "bin" / "nosuchpkg"));

        auto s = run_den_with_cellar("install -s nosuchpkg", home.path.string(), cellar);
        CHECK(s.exit_code == 1);
        CHECK(s.output.find("error: the package index is empty; run `den update` first") !=
              std::string::npos);
    }

    TEST_CASE("info on a missing package exits non-zero with a clear message") {
        TempDir home;
        const std::string cellar = (home.path / "brew" / "Cellar").string();

        // Empty index: say so, rather than implying the name is wrong.
        auto empty = run_den_with_cellar("info nosuchpkg", home.path.string(), cellar);
        CHECK(empty.exit_code == 1);
        CHECK(empty.output.find("error: package not found: nosuchpkg (the package index is "
                                "empty; run `den update` first)") != std::string::npos);

        // Populated index without the package.
        fs::create_directories(home.path / "cache");
        std::ofstream(home.path / "cache" / "index.json") << R"({
  "packages": [{
    "name": "realpkg", "version": "1.0.0", "description": "present",
    "homepage": "", "license": "", "artifact_type": "binary",
    "deprecated": false, "disabled": false,
    "dependencies": [], "build_dependencies": [], "archives": {}
  }]
})";
        auto missing = run_den_with_cellar("info nosuchpkg", home.path.string(), cellar);
        CHECK(missing.exit_code == 1);
        CHECK(missing.output.find("error: package not found: nosuchpkg (run `den update` to "
                                  "refresh the index)") != std::string::npos);

        // A package that is present still succeeds.
        auto found = run_den_with_cellar("info realpkg", home.path.string(), cellar);
        CHECK(found.exit_code == 0);
        CHECK(found.output.find("Version:      1.0.0") != std::string::npos);

        // And install of a name missing from a populated index fails too.
        auto inst = run_den_with_cellar("install nosuchpkg", home.path.string(), cellar);
        CHECK(inst.exit_code == 1);
        CHECK(inst.output.find("nosuchpkg") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 16c. `den upgrade` with a missing or empty index exits non-zero. Every
    //      form (all, named, --dry-run) used to log "package index is empty"
    //      and exit 0, which reads as "nothing to upgrade".
    // -----------------------------------------------------------------------
    TEST_CASE("upgrade with a missing or empty index exits non-zero with a clear message") {
        TempDir home;
        const std::string h = home.path.string();
        const std::string cellar = (home.path / "brew" / "Cellar").string();
        const std::string msg = "error: the package index is empty; run `den update` first";
        const std::string forms[] = {"upgrade", "upgrade --dry-run", "upgrade -n",
                                     "upgrade nosuchpkg", "upgrade --dry-run nosuchpkg"};

        // Missing index (fresh home, no cache/index.json).
        REQUIRE_FALSE(fs::exists(home.path / "cache" / "index.json"));
        for (const auto& form : forms) {
            CAPTURE(form);
            auto r = run_den_with_cellar(form, h, cellar);
            CHECK(r.exit_code == 1);
            CHECK(r.output.find(msg) != std::string::npos);
        }

        // Present but empty index.
        fs::create_directories(home.path / "cache");
        std::ofstream(home.path / "cache" / "index.json") << R"({"packages": []})";
        for (const auto& form : forms) {
            CAPTURE(form);
            auto r = run_den_with_cellar(form, h, cellar);
            CHECK(r.exit_code == 1);
            CHECK(r.output.find(msg) != std::string::npos);
        }

        // A populated index gets past the guard: dry-run with nothing
        // installed succeeds.
        std::ofstream(home.path / "cache" / "index.json") << R"({
  "packages": [{
    "name": "realpkg", "version": "1.0.0", "description": "present",
    "homepage": "", "license": "", "artifact_type": "binary",
    "deprecated": false, "disabled": false,
    "dependencies": [], "build_dependencies": [], "archives": {}
  }]
})";
        auto ok = run_den_with_cellar("upgrade --dry-run", h, cellar);
        CAPTURE(ok.output);
        CHECK(ok.exit_code == 0);
        CHECK(ok.output.find(msg) == std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 17. cleanup — shared Cellar. A Homebrew keg must survive; a den-owned
    //     version nothing references is removed. --dry-run deletes nothing.
    // -----------------------------------------------------------------------
    TEST_CASE("cleanup keeps an untracked brew keg and removes a den-owned stale version") {
        TempDir home;
        const fs::path cellar = home.path / "pfx" / "Cellar";

        const auto brew = cellar / "brew-only-pkg" / "1.0";
        fs::create_directories(brew);
        std::ofstream(brew / "KEEP") << "keep\n";
        std::ofstream(brew / "INSTALL_RECEIPT.json") << "{}\n";

        const auto stale = cellar / "den-pkg" / "1.0";
        fs::create_directories(stale);
        std::ofstream(stale / "STALE") << "old\n";
        mark_keg_owned(stale, "den-pkg", "1.0");

        const auto current = cellar / "den-pkg" / "2.0";
        fs::create_directories(current);
        std::ofstream(current / "KEEP") << "new\n";
        mark_keg_owned(current, "den-pkg", "2.0");

        // Installed by den before ownership receipts existed.
        const auto legacy = cellar / "den-legacy" / "0.9";
        fs::create_directories(legacy);
        std::ofstream(legacy / "KEEP") << "legacy\n";

        fs::create_directories(home.path / "manifests" / "ROOT");
        std::ofstream(home.path / "manifests" / "ROOT" / "manifest.json") << R"({
  "packages": { "homebrew": { "den-pkg": "2.0" } },
  "auto_deps": {}
}
)";

        const auto cache_blob = home.path / "cache" / "archives" / "blob.tar.gz";
        fs::create_directories(cache_blob.parent_path());
        std::ofstream(cache_blob) << "cached\n";

        const auto preview =
            run_den_with_cellar("cleanup --dry-run", home.path.string(), cellar.string());
        CHECK(preview.exit_code == 0);
        CHECK(preview.output.find("Would remove den-pkg 1.0") != std::string::npos);
        CHECK(preview.output.find("Would remove 1 old package version(s).") != std::string::npos);
        CHECK(preview.output.find("brew-only-pkg") == std::string::npos);
        CHECK(fs::exists(brew / "KEEP"));
        CHECK(fs::exists(stale / "STALE"));
        CHECK(fs::exists(current / "KEEP"));
        CHECK(fs::exists(legacy / "KEEP"));
        CHECK(fs::exists(cache_blob));

        const auto result = run_den_with_cellar("cleanup", home.path.string(), cellar.string());
        CHECK(result.exit_code == 0);
        CHECK(result.output.find("Removing den-pkg 1.0") != std::string::npos);
        CHECK(result.output.find("Cleaned up 1 old package version(s).") != std::string::npos);
        CHECK(result.output.find("untouched") != std::string::npos);
        CHECK(result.output.find("Cleared archive cache.") != std::string::npos);
        CHECK(result.output.find("brew-only-pkg") == std::string::npos);
        CHECK(fs::exists(brew / "KEEP"));
        CHECK(fs::exists(current / "KEEP"));
        CHECK(fs::exists(legacy / "KEEP"));
        CHECK_FALSE(fs::exists(stale));
        CHECK_FALSE(fs::exists(cache_blob));
    }

    TEST_CASE("cleanup --help describes den-owned removal and dry-run") {
        const auto out = run_den_fresh("cleanup --help");
        CHECK(out.find("den installed") != std::string::npos);
        CHECK(out.find("Homebrew") != std::string::npos);
        CHECK(out.find("dry-run") != std::string::npos);
        CHECK(out.find("unknown option") == std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 18. A version_scheme bump to a lower version (openssl@3 3.6.3 installed,
    //     index pins 3.5.9 with version_scheme 1) is held end to end:
    //     `outdated`, `upgrade --dry-run` and `upgrade` all report the hold
    //     and change nothing; `den use openssl@3 3.5.9` still switches down.
    // -----------------------------------------------------------------------
    TEST_CASE("openssl@3 scheme bump to 3.5.9 is held by upgrade; den use still downgrades") {
        TempDir home;
        const fs::path cellar = home.path / "pfx" / "Cellar";
        const auto k363 = cellar / "openssl@3" / "3.6.3";
        const auto k359 = cellar / "openssl@3" / "3.5.9";
        for (const auto& k : {k363, k359}) {
            fs::create_directories(k / "bin");
            std::ofstream(k / "bin" / "openssl") << "#!/bin/sh\n";
        }
        std::ofstream(k363 / "INSTALL_RECEIPT.json")
            << R"({"source":{"versions":{"stable":"3.6.3","version_scheme":0}}})";
        std::ofstream(k359 / "INSTALL_RECEIPT.json")
            << R"({"source":{"versions":{"stable":"3.5.9","version_scheme":1}}})";
        fs::create_directories(home.path / "manifests" / "ROOT");
        std::ofstream(home.path / "manifests" / "ROOT" / "manifest.json")
            << R"({"packages":{"homebrew":{"openssl@3":"3.6.3"}}})";
        fs::create_directories(home.path / "cache");
        std::ofstream(home.path / "cache" / "index.json") << R"({
  "packages": [{
    "name": "openssl@3", "version": "3.5.9", "version_scheme": 1,
    "description": "TLS", "homepage": "", "license": "", "artifact_type": "binary",
    "deprecated": false, "disabled": false,
    "dependencies": [], "build_dependencies": [], "archives": {}
  }]
})";
        const std::string reason =
            "held: index pins 3.5.9 (version_scheme 1), below installed 3.6.3";
        auto manifest = [&] {
            std::ifstream f(home.path / "manifests" / "ROOT" / "manifest.json");
            return std::string(std::istreambuf_iterator<char>(f), {});
        };
        const auto before = manifest();

        auto outdated = run_den_with_cellar("outdated", home.path.string(), cellar.string());
        CHECK(outdated.output.find("[" + reason) != std::string::npos);

        auto dry = run_den_with_cellar("upgrade --dry-run", home.path.string(), cellar.string());
        CHECK(dry.exit_code == 0);
        CHECK(dry.output.find("openssl@3 3.6.3 (" + reason +
                              "; to downgrade: den use openssl@3 3.5.9)") != std::string::npos);
        CHECK(dry.output.find("Nothing to upgrade.") != std::string::npos);
        CHECK(dry.output.find("Dry run: nothing changed.") != std::string::npos);
        CHECK(manifest() == before);

        auto up = run_den_with_cellar("upgrade", home.path.string(), cellar.string());
        CHECK(up.exit_code == 0);
        CHECK(up.output.find(reason) != std::string::npos);
        CHECK(up.output.find("to upgrade:") == std::string::npos);
        CHECK(manifest() == before);

        auto use = run_den_with_cellar("use openssl@3 3.5.9", home.path.string(), cellar.string());
        CHECK(use.exit_code == 0);
        CHECK(use.output.find("Switched openssl@3 to 3.5.9") != std::string::npos);
        const auto after = manifest();
        CHECK(after.find("\"3.5.9\"") != std::string::npos);
        CHECK(after.find("\"3.6.3\"") == std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 19. `den upgrade` with no names makes one pass, however many provider
    //     buckets the manifest has (it used to make one per bucket).
    // -----------------------------------------------------------------------
    TEST_CASE("upgrade-all makes one pass with two provider buckets") {
        TempDir home;
        const fs::path cellar = home.path / "pfx" / "Cellar";
        const auto k363 = cellar / "openssl@3" / "3.6.3";
        fs::create_directories(k363 / "bin");
        std::ofstream(k363 / "bin" / "openssl") << "#!/bin/sh\n";
        std::ofstream(k363 / "INSTALL_RECEIPT.json")
            << R"({"source":{"versions":{"stable":"3.6.3","version_scheme":0}}})";
        fs::create_directories(home.path / "manifests" / "ROOT");
        std::ofstream(home.path / "manifests" / "ROOT" / "manifest.json")
            << R"({"packages":{"homebrew":{"openssl@3":"3.6.3"},"stub":{"tool":"0.1.0"}}})";
        fs::create_directories(home.path / "cache");
        std::ofstream(home.path / "cache" / "index.json") << R"({
  "packages": [{
    "name": "openssl@3", "version": "3.5.9", "version_scheme": 1,
    "description": "TLS", "homepage": "", "license": "", "artifact_type": "binary",
    "deprecated": false, "disabled": false,
    "dependencies": [], "build_dependencies": [], "archives": {}
  }]
})";
        auto count = [](const std::string& s, const std::string& needle) {
            size_t n = 0;
            for (auto p = s.find(needle); p != std::string::npos; p = s.find(needle, p + 1))
                ++n;
            return n;
        };

        auto dry = run_den_with_cellar("upgrade --dry-run", home.path.string(), cellar.string());
        CHECK(dry.exit_code == 0);
        CHECK(count(dry.output, "Holding 1 package(s)") == 1);
        CHECK(count(dry.output, "Dry run: nothing changed.") == 1);

        auto up = run_den_with_cellar("upgrade", home.path.string(), cellar.string());
        CHECK(up.exit_code == 0);
        CHECK(count(up.output, "Holding 1 package(s)") == 1);
        CHECK(count(up.output, "Nothing to upgrade.") == 1);
    }

    // -----------------------------------------------------------------------
    // 20. `den doctor` warns (not errors) about a held package, listing
    //     installed vs available, the reason and `den use` to downgrade; the
    //     warning is absent once the index offers nothing below installed.
    // -----------------------------------------------------------------------
    TEST_CASE("doctor warns about a held package, and not otherwise") {
        TempDir home;
        const fs::path cellar = home.path / "pfx" / "Cellar";
        const auto k363 = cellar / "openssl@3" / "3.6.3";
        fs::create_directories(k363 / "bin");
        std::ofstream(k363 / "bin" / "openssl") << "#!/bin/sh\n";
        std::ofstream(k363 / "INSTALL_RECEIPT.json")
            << R"({"source":{"versions":{"stable":"3.6.3","version_scheme":0}}})";
        fs::create_directories(home.path / "manifests" / "ROOT");
        std::ofstream(home.path / "manifests" / "ROOT" / "manifest.json")
            << R"({"packages":{"homebrew":{"openssl@3":"3.6.3"}}})";
        fs::create_directories(home.path / "cache");
        auto write_index = [&](const std::string& version, int scheme) {
            std::ofstream(home.path / "cache" / "index.json") << R"({
  "packages": [{
    "name": "openssl@3", "version": ")" << version << R"(", "version_scheme": )"
                                                              << scheme << R"(,
    "description": "TLS", "homepage": "", "license": "", "artifact_type": "binary",
    "deprecated": false, "disabled": false,
    "dependencies": [], "build_dependencies": [], "archives": {}
  }]
})";
        };
        const std::string held_line =
            "openssl@3: installed 3.6.3, index has 3.5.9 (held: index pins 3.5.9 "
            "(version_scheme 1), below installed 3.6.3)";

        write_index("3.5.9", 1);
        auto held = run_den_with_cellar("doctor", home.path.string(), cellar.string());
        CHECK(held.output.find("[warning] 1 package(s) held") != std::string::npos);
        CHECK(held.output.find(held_line) != std::string::npos);
        CHECK(held.output.find("den use openssl@3 3.5.9") != std::string::npos);
        CHECK(held.output.find("[error] 1 package(s) held") == std::string::npos);

        write_index("3.6.3", 0);
        auto same = run_den_with_cellar("doctor", home.path.string(), cellar.string());
        CHECK(same.output.find("package(s) held") == std::string::npos);
        CHECK(same.output.find("den use openssl@3") == std::string::npos);
    }

} // TEST_SUITE cli::integration

} // namespace cli_test
} // namespace den

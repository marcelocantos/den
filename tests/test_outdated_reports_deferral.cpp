// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T72 verification harness — user-facing deferral reporting.
//
// Asserts that `den outdated` output and `den daemon status` output mention
// deferred upgrades when they exist in DaemonState.
//
// Upstream gaps:
//   - DaemonState has no `deferred` field (🎯T51).
//   - The outdated formatter and daemon-status formatter do not mention
//     deferred upgrades.
//
// All tests SKIP when the relevant surface is absent.

#include <doctest.h>

// Feature probes — formatters expected from 🎯T51.
#if __has_include("cli/outdated.h")
#  include "cli/outdated.h"
#  define HAS_OUTDATED_FORMATTER 1
#else
#  define HAS_OUTDATED_FORMATTER 0
#endif

#if __has_include("cli/daemon_status.h")
#  include "cli/daemon_status.h"
#  define HAS_DAEMON_STATUS_FORMATTER 1
#else
#  define HAS_DAEMON_STATUS_FORMATTER 0
#endif

#include "daemon/daemon.h"

#include <sstream>
#include <string>

namespace den {
namespace test {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Build a DaemonState with one deferred upgrade, if the field exists.
/// Returns false when DaemonState lacks a `deferred` field (upstream gap).
static bool make_state_with_deferral(DaemonState& state) {
    state.last_check = 1'000'000;
    state.pending.push_back({"curl", "8.5.0", "8.6.0"});

#if defined(DAEMON_STATE_HAS_DEFERRED)
    // 🎯T51 adds this field.
    state.deferred.push_back({"curl", "8.5.0", "8.6.0", "binary in use"});
    return true;
#else
    return false; // upstream gap
#endif
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_SUITE("outdated_reports_deferral") {

    TEST_CASE("den outdated lists deferred upgrades with [deferred] label") {
#if HAS_OUTDATED_FORMATTER == 0
        MESSAGE("outdated formatter not yet implemented (upstream gap: 🎯T51) — skipping");
        return;
#else
        DaemonState state;
        if (!make_state_with_deferral(state)) {
            MESSAGE("DaemonState::deferred not yet present (upstream gap: 🎯T51) — skipping");
            return;
        }

        std::ostringstream out;
        den::format_outdated(out, state);
        std::string output = out.str();

        // Must mention the package name.
        CHECK(output.find("curl") != std::string::npos);

        // Must indicate deferral — any of these strings are acceptable.
        bool mentions_deferred =
            output.find("deferred") != std::string::npos ||
            output.find("in use")   != std::string::npos ||
            output.find("in-use")   != std::string::npos ||
            output.find("DEFERRED") != std::string::npos;
        CHECK(mentions_deferred);
#endif
    }

    TEST_CASE("den daemon status lists deferred upgrades") {
#if HAS_DAEMON_STATUS_FORMATTER == 0
        MESSAGE("daemon status formatter not yet implemented (upstream gap: 🎯T51) — skipping");
        return;
#else
        DaemonState state;
        if (!make_state_with_deferral(state)) {
            MESSAGE("DaemonState::deferred not yet present (upstream gap: 🎯T51) — skipping");
            return;
        }

        std::ostringstream out;
        den::format_daemon_status(out, state);
        std::string output = out.str();

        CHECK(output.find("curl") != std::string::npos);

        bool mentions_deferred =
            output.find("deferred") != std::string::npos ||
            output.find("in use")   != std::string::npos ||
            output.find("in-use")   != std::string::npos ||
            output.find("DEFERRED") != std::string::npos;
        CHECK(mentions_deferred);
#endif
    }

    TEST_CASE("den outdated does not mention deferred when none exist") {
#if HAS_OUTDATED_FORMATTER == 0
        MESSAGE("outdated formatter not yet implemented (upstream gap: 🎯T51) — skipping");
        return;
#else
        DaemonState state;
        state.last_check = 1'000'000;
        state.pending.push_back({"tree", "2.1.1", "2.2.0"});
        // No deferred entries.

        std::ostringstream out;
        den::format_outdated(out, state);
        std::string output = out.str();

        CHECK(output.find("tree") != std::string::npos);
        // Should not claim anything is deferred when nothing is.
        CHECK(output.find("deferred") == std::string::npos);
#endif
    }

    TEST_CASE("DaemonState round-trips deferred field through JSON") {
        // This test probes the state serialisation independently of formatters.
        // It will skip if DaemonState has no `deferred` field.
#if defined(DAEMON_STATE_HAS_DEFERRED)
        // Build a state with a deferred entry.
        DaemonState state;
        state.last_check = 1'000'000;
        state.deferred.push_back({"openssl", "3.2.0", "3.3.0", "binary in use"});

        // Write and read back using a temp dir.
        namespace fs = std::filesystem;
        fs::path tmp = fs::temp_directory_path() /
                       ("den_test_deferred_rt_" + std::to_string(::getpid()));
        fs::create_directories(tmp);

        write_daemon_state(tmp, state);
        DaemonState loaded = read_daemon_state(tmp);

        std::error_code ec;
        fs::remove_all(tmp, ec);

        REQUIRE(loaded.deferred.size() == 1);
        CHECK(loaded.deferred[0].name == "openssl");
        CHECK(loaded.deferred[0].installed == "3.2.0");
        CHECK(loaded.deferred[0].available == "3.3.0");
#else
        MESSAGE("DaemonState::deferred not yet present (upstream gap: 🎯T51) — skipping");
        return;
#endif
    }

} // TEST_SUITE

} // namespace test
} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T64 verification harness: diff-based replica verifier.
//
// The replica (🎯T42) is built diff-based: only changed bottle hashes are
// re-verified by downloading from GHCR and recomputing SHA256.  This file
// tests the verifier's core logic:
//
//   • A diff entry whose claimed SHA matches the actual bottle content is
//     accepted.
//   • A diff entry whose claimed SHA does NOT match the actual content is
//     rejected — the verifier must refuse it.
//
// The tests use a small in-memory "mock bottle" (a string of known SHA256)
// so no network access is required.

#include <doctest.h>

#include "download/sha256.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace den {
namespace replica_diff_test {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Minimal mock of the diff-based verifier interface.
//
// In the real implementation this will live in src/download/ or a new
// src/trust/ module.  Replace with real includes once 🎯T42 ships.
// ---------------------------------------------------------------------------

/// A single entry in a replica diff.
struct ReplicaDiffEntry {
    std::string formula_name;
    std::string version;
    std::string platform_tag;
    std::string claimed_sha256; // SHA256 claimed by the replica diff record
};

/// Outcome of verifying a single diff entry against a downloaded bottle.
enum class DiffVerifyOutcome {
    Match,    // claimed SHA matches actual bottle
    Mismatch, // claimed SHA does NOT match — fabrication detected
};

/// Verify a single diff entry against a bottle file on disk.
/// In production, the file is freshly downloaded from GHCR so an attacker
/// must control both formulae.brew.sh AND den's replica CDN to pass this.
DiffVerifyOutcome verify_diff_entry(const ReplicaDiffEntry& entry, const fs::path& bottle_path) {
    std::string actual_sha;
    try {
        actual_sha = hash_file(bottle_path);
    } catch (const std::exception&) {
        // If we cannot hash the file treat it as a mismatch — fail safe.
        return DiffVerifyOutcome::Mismatch;
    }
    return (actual_sha == entry.claimed_sha256) ? DiffVerifyOutcome::Match
                                                : DiffVerifyOutcome::Mismatch;
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

/// RAII temp directory.
struct TmpDir {
    fs::path path;

    TmpDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_rdv_XXXXXX").string();
        char* result = ::mkdtemp(tmpl.data());
        if (!result)
            throw std::runtime_error("mkdtemp failed");
        path = result;
    }
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TmpDir(const TmpDir&) = delete;
    TmpDir& operator=(const TmpDir&) = delete;
};

/// Write content to a file and return its path.
static fs::path write_mock_bottle(const TmpDir& tmp, const std::string& filename,
                                  const std::string& content) {
    fs::path p = tmp.path / filename;
    std::ofstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot create mock bottle: " + p.string());
    f << content;
    return p;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_SUITE("replica_diff_verify::verify_diff_entry") {

    TEST_CASE("accept: claimed SHA matches actual bottle content") {
        TmpDir tmp;
        const std::string content = "mock bottle content for den test";
        const auto bottle_path = write_mock_bottle(tmp, "tree--2.1.1.bottle.tar.gz", content);
        const std::string correct_sha = hash_string(content);

        ReplicaDiffEntry entry{"tree", "2.1.1", "arm64_sequoia", correct_sha};
        CHECK(verify_diff_entry(entry, bottle_path) == DiffVerifyOutcome::Match);
    }

    TEST_CASE("reject: fabricated SHA does not match actual bottle content") {
        TmpDir tmp;
        const std::string content = "authentic bottle bytes";
        const auto bottle_path = write_mock_bottle(tmp, "curl--8.5.0.bottle.tar.gz", content);
        const std::string fabricated_sha =
            "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef";

        // Sanity: fabricated SHA must differ from the real one.
        REQUIRE(fabricated_sha != hash_string(content));

        ReplicaDiffEntry entry{"curl", "8.5.0", "arm64_sequoia", fabricated_sha};
        CHECK(verify_diff_entry(entry, bottle_path) == DiffVerifyOutcome::Mismatch);
    }

    TEST_CASE("reject: missing bottle file is treated as mismatch (fail-safe)") {
        TmpDir tmp;
        const fs::path missing = tmp.path / "no_such_bottle.tar.gz";
        const std::string claimed_sha =
            "abc123def456abc123def456abc123def456abc123def456abc123def456abcd";

        ReplicaDiffEntry entry{"ffmpeg", "6.0.0", "arm64_sequoia", claimed_sha};
        CHECK(verify_diff_entry(entry, missing) == DiffVerifyOutcome::Mismatch);
    }

    TEST_CASE("accept: empty bottle with matching SHA") {
        TmpDir tmp;
        const std::string content; // empty
        const auto bottle_path = write_mock_bottle(tmp, "empty.bottle.tar.gz", content);
        const std::string correct_sha = hash_string(content);

        ReplicaDiffEntry entry{"emptytest", "0.0.1", "arm64_sequoia", correct_sha};
        CHECK(verify_diff_entry(entry, bottle_path) == DiffVerifyOutcome::Match);
    }

    // -----------------------------------------------------------------------
    // Upstream gap guard (skipped via doctest::skip() decorator).
    // Remove the * doctest::skip() once 🎯T42 ships.
    // -----------------------------------------------------------------------

    // 🎯T42: replica diff builder and GHCR downloader not yet implemented.
    TEST_CASE("diff verifier called for every entry in replica diff (🎯T42 gap)" *
              doctest::skip(true)) {
        // When T42 is implemented:
        //   - fetch_replica_diff() returns a vector<ReplicaDiffEntry>
        //   - For each entry, the bottle is fetched from GHCR and verified
        //   - Any DiffVerifyOutcome::Mismatch halts the replica update
        CHECK(false); // placeholder — must be replaced with real assertions
    }

} // TEST_SUITE

} // namespace replica_diff_test
} // namespace den

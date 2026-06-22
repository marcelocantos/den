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
#include "trust/trust_model.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace den {
namespace replica_diff_test {

namespace fs = std::filesystem;

// ReplicaDiffEntry / DiffVerifyOutcome / verify_diff_entry now come from the
// real implementation in src/trust/trust_model.* (included above). The replica
// diff verifier is invoked by the .github/workflows/replica-verify.yml CI
// pipeline, which downloads each changed bottle from GHCR and recomputes its
// SHA256 before the hash is committed to data/known_hashes.json.

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
    // 🎯T42: the diff verifier is driven over a batch of entries by the
    // replica-verify CI pipeline. Here we model that loop: every entry is run
    // through verify_diff_entry, and a single fabricated entry must halt the
    // batch (any Mismatch fails the run). The real pipeline downloads each
    // bottle from GHCR before hashing; the loop semantics are identical.
    // -----------------------------------------------------------------------

    TEST_CASE("diff verifier rejects a batch containing one fabricated entry") {
        TmpDir tmp;
        const std::string good_a = "real bottle alpha";
        const std::string good_b = "real bottle beta";
        const auto path_a = write_mock_bottle(tmp, "a.bottle.tar.gz", good_a);
        const auto path_b = write_mock_bottle(tmp, "b.bottle.tar.gz", good_b);

        struct Item {
            ReplicaDiffEntry entry;
            fs::path bottle;
        };
        std::vector<Item> batch{
            {{"a", "1.0.0", "arm64_sequoia", hash_string(good_a)}, path_a},
            {{"b", "2.0.0", "arm64_sequoia", hash_string(good_b)}, path_b},
            // Fabricated: claimed SHA does not match path_a's content.
            {{"c", "3.0.0", "arm64_sequoia",
              "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"},
             path_a},
        };

        bool batch_ok = true;
        for (const auto& item : batch) {
            if (verify_diff_entry(item.entry, item.bottle) != DiffVerifyOutcome::Match) {
                batch_ok = false;
                break; // a single mismatch halts the replica update
            }
        }
        CHECK_FALSE(batch_ok);
    }

    TEST_CASE("diff verifier accepts a fully consistent batch") {
        TmpDir tmp;
        const std::string c1 = "consistent one";
        const std::string c2 = "consistent two";
        const auto p1 = write_mock_bottle(tmp, "p1.bottle.tar.gz", c1);
        const auto p2 = write_mock_bottle(tmp, "p2.bottle.tar.gz", c2);

        std::vector<std::pair<ReplicaDiffEntry, fs::path>> batch{
            {{"p1", "1.0.0", "arm64_sequoia", hash_string(c1)}, p1},
            {{"p2", "1.0.0", "arm64_sequoia", hash_string(c2)}, p2},
        };

        bool batch_ok = true;
        for (const auto& [entry, bottle] : batch) {
            if (verify_diff_entry(entry, bottle) != DiffVerifyOutcome::Match) {
                batch_ok = false;
                break;
            }
        }
        CHECK(batch_ok);
    }

} // TEST_SUITE

} // namespace replica_diff_test
} // namespace den

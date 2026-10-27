// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T42: the trust replica snapshot is resolved relative to the installed
// binary (or den_home), never the current directory, with the copy compiled
// into the binary as the fallback. The release tarball ships it beside den.

#include <doctest.h>

#include "core/error.h"
#include "core/self_exe.h"
#include "doctor/trust_checks.h"
#include "selfupdate/selfupdate.h"
#include "trust/trust_model.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace den;

namespace {

const std::string kSha(64, 'a');
const std::string kOtherSha(64, 'b');

struct ReplTmp {
    fs::path path;
    ReplTmp() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("den-replica-test-" + std::to_string(rd()));
        fs::create_directories(path);
        path = fs::canonical(path);
    }
    ~ReplTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ReplTmp(const ReplTmp&) = delete;
    ReplTmp& operator=(const ReplTmp&) = delete;
};

// Restores the working directory on scope exit.
struct Chdir {
    fs::path old = fs::current_path();
    explicit Chdir(const fs::path& to) { fs::current_path(to); }
    ~Chdir() {
        std::error_code ec;
        fs::current_path(old, ec);
    }
};

std::string replica_doc(const std::string& key, const std::string& sha) {
    return R"({"schema_version": 1, "hashes": {")" + key + R"(": ")" + sha + R"("}})";
}

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

fs::path fake_exe(const fs::path& p) {
    write(p, "#!/bin/sh\n");
    return p;
}

Config cfg_for(const fs::path& root) {
    Config c;
    c.den_home = root / "den-home";
    return c;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_SUITE("replica_resolution") {

    TEST_CASE("embedded snapshot matches data/known_hashes.json") {
        auto embedded = embedded_replica_json();
        REQUIRE_FALSE(embedded.empty());
        auto from_embedded = parse_replica_document(std::string(embedded));
        auto from_file =
            parse_replica_document(slurp(fs::path(DEN_SOURCE_DIR) / "data" / "known_hashes.json"));
        CHECK(from_embedded == from_file);
        CHECK_FALSE(from_embedded.empty());
    }

    TEST_CASE("den_home override wins") {
        ReplTmp t;
        auto cfg = cfg_for(t.path);
        auto exe = fake_exe(t.path / "prefix" / "bin" / "den");
        write(t.path / "prefix" / "share" / "den" / "known_hashes.json", replica_doc("x", kSha));
        write(cfg.den_home / "trust" / "known_hashes.json", replica_doc("y", kSha));
        auto src = locate_local_replica(cfg, exe, "{}");
        CHECK(src.kind == ReplicaSource::Kind::File);
        CHECK(src.path == cfg.den_home / "trust" / "known_hashes.json");
    }

    TEST_CASE("resolves <exe_dir>/../share/den, ignoring the working directory") {
        ReplTmp t;
        auto cfg = cfg_for(t.path);
        auto exe = fake_exe(t.path / "prefix" / "bin" / "den");
        auto shipped = t.path / "prefix" / "share" / "den" / "known_hashes.json";
        write(shipped, replica_doc("good", kSha));

        // Decoys at every old CWD-relative location.
        auto cwd = t.path / "elsewhere" / "sub";
        write(cwd / "data" / "known_hashes.json", replica_doc("decoy", kOtherSha));
        write(t.path / "elsewhere" / "share" / "den" / "known_hashes.json",
              replica_doc("decoy", kOtherSha));
        fs::create_directories(cwd);
        Chdir in(cwd);

        auto src = locate_local_replica(cfg, exe, "{}");
        REQUIRE(src.kind == ReplicaSource::Kind::File);
        CHECK(src.path == shipped);
        auto loaded = load_replica(src, "{}");
        CHECK(loaded.count("good") == 1);
        CHECK(loaded.count("decoy") == 0);
    }

    TEST_CASE("CWD-relative copies alone are not used (falls back to embedded)") {
        ReplTmp t;
        auto cfg = cfg_for(t.path);
        auto exe = fake_exe(t.path / "opt" / "bin" / "den");
        auto cwd = t.path / "work" / "sub";
        write(cwd / "data" / "known_hashes.json", replica_doc("decoy", kOtherSha));
        write(t.path / "work" / "share" / "den" / "known_hashes.json",
              replica_doc("decoy", kOtherSha));
        Chdir in(cwd);
        auto src = locate_local_replica(cfg, exe, replica_doc("emb", kSha));
        CHECK(src.kind == ReplicaSource::Kind::Embedded);
        CHECK(src.describe() == "embedded snapshot");
        auto loaded = load_replica(src, replica_doc("emb", kSha));
        CHECK(loaded.count("emb") == 1);
    }

    TEST_CASE("symlinked binary resolves the keg's share as well as the prefix's") {
        ReplTmp t;
        auto cfg = cfg_for(t.path);
        auto keg = t.path / "Cellar" / "den" / "1.0";
        auto real = fake_exe(keg / "bin" / "den");
        auto link = t.path / "prefix" / "bin" / "den";
        fs::create_directories(link.parent_path());
        fs::create_symlink(real, link);

        // Only the keg has the snapshot.
        write(keg / "share" / "den" / "known_hashes.json", replica_doc("keg", kSha));
        auto src = locate_local_replica(cfg, link, "");
        REQUIRE(src.kind == ReplicaSource::Kind::File);
        CHECK(src.path == keg / "share" / "den" / "known_hashes.json");

        // The prefix's own share takes precedence when present.
        write(t.path / "prefix" / "share" / "den" / "known_hashes.json", replica_doc("pfx", kSha));
        src = locate_local_replica(cfg, link, "");
        CHECK(src.path == t.path / "prefix" / "share" / "den" / "known_hashes.json");
    }

    TEST_CASE("unpacked release tarball layout (den + share/den beside it)") {
        ReplTmp t;
        auto exe = fake_exe(t.path / "den-1.0-darwin-arm64" / "den");
        auto shipped = t.path / "den-1.0-darwin-arm64" / "share" / "den" / "known_hashes.json";
        write(shipped, replica_doc("tar", kSha));
        auto src = locate_local_replica(cfg_for(t.path), exe, "");
        REQUIRE(src.kind == ReplicaSource::Kind::File);
        CHECK(src.path == shipped);
    }

    TEST_CASE("dev build at <repo>/build/den finds <repo>/data") {
        ReplTmp t;
        auto exe = fake_exe(t.path / "repo" / "build" / "den");
        auto data = t.path / "repo" / "data" / "known_hashes.json";
        write(data, replica_doc("dev", kSha));
        auto src = locate_local_replica(cfg_for(t.path), exe, "");
        REQUIRE(src.kind == ReplicaSource::Kind::File);
        CHECK(src.path == data);
    }

    TEST_CASE("nothing on disk and no embedded snapshot → None, empty replica") {
        ReplTmp t;
        auto src = locate_local_replica(cfg_for(t.path), std::nullopt, "");
        CHECK(src.kind == ReplicaSource::Kind::None);
        CHECK(load_replica(src, "").empty());
    }

    TEST_CASE("a corrupt on-disk snapshot fails closed (does not fall back to embedded)") {
        ReplTmp t;
        auto exe = fake_exe(t.path / "prefix" / "bin" / "den");
        write(t.path / "prefix" / "share" / "den" / "known_hashes.json", "{oops");
        auto src = locate_local_replica(cfg_for(t.path), exe, replica_doc("emb", kSha));
        REQUIRE(src.kind == ReplicaSource::Kind::File);
        CHECK_THROWS_AS(load_replica(src, replica_doc("emb", kSha)), UserError);
    }

    TEST_CASE("the running binary resolves a replica (embedded at minimum)") {
        ReplTmp t;
        Chdir in(t.path); // no CWD-relative help
        CHECK(current_executable_path().has_value());
        auto src = locate_local_replica(cfg_for(t.path));
        CHECK(src.kind != ReplicaSource::Kind::None);
        CHECK_FALSE(load_local_replica(cfg_for(t.path)).empty());
        auto report = build_trust_report(cfg_for(t.path));
        CHECK(report.replica_source_active);
        CHECK(report.replica_source == src.describe());
    }

    TEST_CASE("self-update refreshes an installed share copy, never creates one") {
        ReplTmp t;
        auto extracted = t.path / "extract";
        write(extracted / "share" / "den" / "known_hashes.json", replica_doc("new", kSha));
        auto exe = fake_exe(t.path / "home" / ".den" / "bin" / "den");
        auto installed = t.path / "home" / ".den" / "share" / "den" / "known_hashes.json";

        refresh_packaged_replica(extracted, exe);
        CHECK_FALSE(fs::exists(installed));

        write(installed, replica_doc("old", kSha));
        refresh_packaged_replica(extracted, exe);
        CHECK(slurp(installed) == replica_doc("new", kSha));

        // A tarball without the file leaves the installed copy alone.
        fs::remove_all(extracted / "share");
        refresh_packaged_replica(extracted, exe);
        CHECK(slurp(installed) == replica_doc("new", kSha));
    }

    TEST_CASE("scripts/package-release.sh ships den + share/den/known_hashes.json") {
        ReplTmp t;
        auto build = t.path / "build";
        write(build / "den", "#!/bin/sh\necho den\n");
        fs::permissions(build / "den", fs::perms::owner_all);
        auto archive = t.path / "den-test.tar.gz";
        std::string cmd = "sh '" + std::string(DEN_SOURCE_DIR) + "/scripts/package-release.sh' '" +
                          build.string() + "' '" + archive.string() + "'";
        REQUIRE(std::system(cmd.c_str()) == 0);

        auto out = t.path / "out";
        fs::create_directories(out);
        REQUIRE(std::system(
                    ("tar xzf '" + archive.string() + "' -C '" + out.string() + "'").c_str()) == 0);
        CHECK(fs::is_regular_file(out / "den"));
        auto shipped = out / "share" / "den" / "known_hashes.json";
        REQUIRE(fs::is_regular_file(shipped));
        CHECK(slurp(shipped) == slurp(fs::path(DEN_SOURCE_DIR) / "data" / "known_hashes.json"));

        // The unpacked layout resolves its own share copy.
        auto src = locate_local_replica(cfg_for(t.path), out / "den", "");
        CHECK(src.path == shipped);
    }

} // TEST_SUITE

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Regression: den dropped Homebrew's formula revision from keg directory
// names. Bottles are built into Cellar/<name>/<version>_<revision> and every
// dependent's load commands and wrapper scripts name that directory, so
// pouring libngtcp2 1.25.0_1 into Cellar/libngtcp2/1.25.0 broke curl
// (dyld: Library not loaded: …/libngtcp2/1.25.0_1/lib/libngtcp2.16.dylib), and
// likewise libimobiledevice 1.4.0_2, opus-tools → opusfile 0.12_3 and the
// exiftool 13.55_1 wrapper. Found by the 2026-10 den readiness audit.
//
// Also: den never created <prefix>/opt/<name>, so relocated node shebangs
// (#!<prefix>/opt/node/bin/node in firebase-cli, vite) had no interpreter.

#include <doctest.h>

#include "download/sha256.h"
#include "index/index.h"
#include "store/link.h"
#include "store/store.h"

#include <nlohmann/json.hpp>

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
namespace test {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct RevTmp {
    fs::path path;
    RevTmp() {
        std::string tmpl = fs::temp_directory_path() / "den_kegrev_XXXXXX";
        char* r = ::mkdtemp(tmpl.data());
        if (!r) {
            throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
        }
        path = fs::path(r).lexically_normal();
    }
    ~RevTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    RevTmp(const RevTmp&) = delete;
    RevTmp& operator=(const RevTmp&) = delete;
};

void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << s;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

json formula_json(const std::string& name, const std::string& stable, int revision,
                  int rebuild = 0) {
    json f = {
        {"name", name},
        {"desc", "test"},
        {"homepage", "https://example.com"},
        {"license", "MIT"},
        {"versions", {{"stable", stable}, {"bottle", true}}},
        {"dependencies", json::array()},
        {"build_dependencies", json::array()},
        {"bottle",
         {{"stable",
           {{"rebuild", rebuild},
            {"files",
             {{"arm64_sequoia",
               {{"cellar", ":any"},
                {"url", "https://ghcr.io/v2/homebrew/core/" + name + "/blobs/sha256:aa"},
                {"sha256", std::string(64, 'a')}}}}}}}}},
    };
    if (revision >= 0) {
        f["revision"] = revision;
    }
    return f;
}

struct DenRun {
    std::string output;
    int exit_code;
};

/// Run the built den against a scratch DEN_HOME / prefix / Cellar / HOME.
DenRun run_den(const std::string& args, const fs::path& home, const fs::path& prefix) {
    // DEN_PREFIX picks den's store; HOMEBREW_PREFIX/CELLAR only locate brew.
    const std::string cmd =
        "HOME=" + home.string() + " DEN_HOME=" + (home / "den").string() +
        " DEN_PREFIX=" + prefix.string() + " HOMEBREW_PREFIX=" + prefix.string() +
        " HOMEBREW_CELLAR=" + (prefix / "Cellar").string() + " ./den " + args + " 2>&1";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("popen failed");
    }
    std::string out;
    std::array<char, 1024> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        out += buf.data();
    }
    const int raw = ::pclose(pipe);
    return {out, WIFEXITED(raw) ? WEXITSTATUS(raw) : -1};
}

/// Build a bottle tarball laid out as Homebrew pours it (<name>/<dir>/…),
/// drop it into den's content-addressed archive cache so no download
/// happens, and return its sha256.
std::string stage_bottle(const fs::path& home, const std::string& name, const std::string& dir) {
    const auto src = home / "bottle-src";
    fs::remove_all(src);
    write(src / name / dir / "bin" / name, "#!/bin/sh\necho " + name + "-ok\n");
    fs::permissions(src / name / dir / "bin" / name, fs::perms::owner_all, fs::perm_options::add);
    const auto tarball = home / (name + ".tar.gz");
    const std::string cmd = "tar -czf '" + tarball.string() + "' -C '" + src.string() + "' " + name;
    REQUIRE(std::system(cmd.c_str()) == 0);
    const auto data = slurp(tarball);
    const auto sha = hash_string(data);
    write(home / "den" / "cache" / "archives" / (sha + ".tar.gz"), data);
    return sha;
}

/// Write a one-formula den index (index.json) for the bottle.
void write_index(const fs::path& home, const std::string& name, const std::string& version,
                 int revision, const std::string& sha) {
    json pkg = {
        {"name", name},
        {"version", version},
        {"description", "test"},
        {"homepage", ""},
        {"license", ""},
        {"artifact_type", "binary"},
        {"deprecated", false},
        {"disabled", false},
        {"dependencies", json::array()},
        {"build_dependencies", json::array()},
        {"archives",
         {{"all",
           {{"url", "file:///nonexistent/" + name + ".tar.gz"},
            {"sha256", sha},
            {"relocation", "portable"}}}}},
    };
    if (revision > 0) {
        pkg["revision"] = revision;
    }
    const auto idx = home / "den" / "cache" / "index.json";
    write(idx, json{{"packages", json::array({pkg})}}.dump(2));
    // Drop any binary lookup cache so the fresh JSON is read.
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(idx.parent_path(), ec)) {
        const auto fn = e.path().filename().string();
        if (fn != "index.json" && fn.rfind("index", 0) == 0) {
            fs::remove(e.path(), ec);
        }
    }
}

} // namespace

TEST_SUITE("keg_revision") {

    TEST_CASE("index folds the formula revision into the keg version") {
        json formulae = json::array({
            formula_json("libngtcp2", "1.25.0", 1),
            formula_json("opusfile", "0.12", 3, /*rebuild=*/2), formula_json("jq", "1.8.1", 0),
            formula_json("tree", "2.2.1", -1), // no revision key at all
        });
        auto idx = transform_homebrew(formulae.dump(), "[]");

        const auto* ng = idx.find("libngtcp2");
        REQUIRE(ng != nullptr);
        CHECK(ng->version == "1.25.0_1");
        CHECK(ng->revision == 1);
        CHECK(ng->upstream_version() == "1.25.0");

        // Bottle rebuild does not change the keg directory.
        const auto* op = idx.find("opusfile");
        REQUIRE(op != nullptr);
        CHECK(op->version == "0.12_3");
        CHECK(op->upstream_version() == "0.12");

        const auto* jq = idx.find("jq");
        REQUIRE(jq != nullptr);
        CHECK(jq->version == "1.8.1");
        CHECK(jq->revision == 0);
        CHECK(jq->upstream_version() == "1.8.1");

        const auto* tree = idx.find("tree");
        REQUIRE(tree != nullptr);
        CHECK(tree->version == "2.2.1");
        CHECK(tree->revision == 0);
    }

    TEST_CASE("revision survives index.json and the binary lookup cache") {
        RevTmp t;
        PackageIndex idx;
        Package p;
        p.name = "libimobiledevice";
        p.version = "1.4.0_2";
        p.revision = 2;
        idx.packages[p.name] = p;
        Package q;
        q.name = "jq";
        q.version = "1.8.1";
        idx.packages[q.name] = q;
        const auto path = t.path / "index.json";
        save_index(idx, path);

        auto from_json = load_index_json(path);
        REQUIRE(from_json.find("libimobiledevice") != nullptr);
        CHECK(from_json.find("libimobiledevice")->version == "1.4.0_2");
        CHECK(from_json.find("libimobiledevice")->revision == 2);
        CHECK(from_json.find("libimobiledevice")->upstream_version() == "1.4.0");
        CHECK(from_json.find("jq")->revision == 0);

        auto lazy = load_index(path); // served from the mmap'd cache
        REQUIRE(lazy.find("libimobiledevice") != nullptr);
        CHECK(lazy.find("libimobiledevice")->revision == 2);
        CHECK(lazy.find("libimobiledevice")->version == "1.4.0_2");
        CHECK(lazy.find("jq")->revision == 0);
    }

    TEST_CASE("upstream_version only strips the matching revision suffix") {
        Package p;
        p.version = "2024_01";
        CHECK(p.upstream_version() == "2024_01"); // revision 0: untouched
        p.revision = 1;
        p.version = "2024_01_1";
        CHECK(p.upstream_version() == "2024_01");
        p.version = "3.0"; // inconsistent: never mangle
        CHECK(p.upstream_version() == "3.0");
    }

    TEST_CASE("install pours a revisioned bottle into <version>_<revision> with an opt link") {
        RevTmp t;
        const auto prefix = t.path / "prefix";
        const auto cellar = prefix / "Cellar";
        fs::create_directories(cellar);
        const auto sha = stage_bottle(t.path, "libngtcp2", "1.25.0_1");
        write_index(t.path, "libngtcp2", "1.25.0_1", 1, sha);

        auto r = run_den("install libngtcp2", t.path, prefix);
        INFO(r.output);
        REQUIRE(r.exit_code == 0);

        const auto keg = cellar / "libngtcp2" / "1.25.0_1";
        CHECK(fs::is_regular_file(keg / "bin" / "libngtcp2"));
        CHECK_FALSE(fs::exists(cellar / "libngtcp2" / "1.25.0"));
        CHECK(keg_owned_by_den(keg));
        CHECK(is_installed(cellar, "libngtcp2", "1.25.0_1"));

        // The shared opt link exists, is relative like Homebrew's, and
        // resolves to the poured keg.
        const auto opt = prefix / "opt" / "libngtcp2";
        REQUIRE(fs::is_symlink(opt));
        CHECK(fs::read_symlink(opt) == fs::path("../Cellar/libngtcp2/1.25.0_1"));
        CHECK(fs::equivalent(opt, keg));
        CHECK(fs::is_regular_file(opt / "bin" / "libngtcp2"));

        // The manifest records the revisioned version, so outdated is quiet.
        auto o = run_den("outdated", t.path, prefix);
        INFO(o.output);
        CHECK(o.output.find("->") == std::string::npos);
    }

    TEST_CASE("a stale index without the revision refuses instead of mis-pouring") {
        RevTmp t;
        const auto prefix = t.path / "prefix";
        const auto cellar = prefix / "Cellar";
        fs::create_directories(cellar);
        const auto sha = stage_bottle(t.path, "opusfile", "0.12_3");
        write_index(t.path, "opusfile", "0.12", 0, sha); // pre-revision index

        auto r = run_den("install opusfile", t.path, prefix);
        INFO(r.output);
        CHECK(r.exit_code != 0);
        CHECK(r.output.find("den update") != std::string::npos);
        CHECK_FALSE(fs::exists(cellar / "opusfile" / "0.12"));
        CHECK_FALSE(fs::exists(cellar / "opusfile" / "0.12_3"));
    }

    TEST_CASE("upgrade migrates an unrevisioned den keg to <version>_<revision>") {
        RevTmp t;
        const auto prefix = t.path / "prefix";
        const auto cellar = prefix / "Cellar";

        // State left by an older den: keg poured at the bare version, recorded
        // in the manifest, and (from this fix's opt handling) linked in opt.
        const auto sha = stage_bottle(t.path, "exiftool", "13.55_1");
        write_index(t.path, "exiftool", "13.55", 0, std::string(64, 'f'));
        const auto old_keg = cellar / "exiftool" / "13.55";
        write(old_keg / "bin" / "exiftool", "#!/bin/sh\necho old\n");
        mark_keg_owned(old_keg, "exiftool", "13.55");
        fs::create_directories(prefix / "opt");
        fs::create_symlink("../Cellar/exiftool/13.55", prefix / "opt" / "exiftool");
        write(t.path / "den" / "manifests" / "ROOT" / "manifest.json",
              R"({"packages": {"homebrew": {"exiftool": "13.55"}}, "auto_deps": {}})");

        // After `den update` the index carries the revision.
        write_index(t.path, "exiftool", "13.55_1", 1, sha);
        auto o = run_den("outdated", t.path, prefix);
        INFO(o.output);
        CHECK(o.output.find("13.55 -> 13.55_1") != std::string::npos);

        auto u = run_den("upgrade exiftool", t.path, prefix);
        INFO(u.output);
        REQUIRE(u.exit_code == 0);
        const auto new_keg = cellar / "exiftool" / "13.55_1";
        CHECK(fs::is_regular_file(new_keg / "bin" / "exiftool"));
        // opt moved from den's old keg to den's new one.
        CHECK(fs::read_symlink(prefix / "opt" / "exiftool") ==
              fs::path("../Cellar/exiftool/13.55_1"));
        CHECK(slurp(t.path / "den" / "manifests" / "ROOT" / "manifest.json").find("13.55_1") !=
              std::string::npos);

        // Cleanup drops the old den keg; opt keeps pointing at the new one.
        auto c = run_den("cleanup", t.path, prefix);
        INFO(c.output);
        CHECK(c.exit_code == 0);
        CHECK_FALSE(fs::exists(old_keg));
        CHECK(fs::exists(new_keg));
        CHECK(fs::read_symlink(prefix / "opt" / "exiftool") ==
              fs::path("../Cellar/exiftool/13.55_1"));
    }
}

TEST_SUITE("prefix_opt") {

    TEST_CASE("ensure_prefix_opt_link creates a missing or dangling link") {
        RevTmp t;
        const auto cellar = t.path / "Cellar";
        const auto keg = cellar / "node" / "25.1.0";
        write(keg / "bin" / "node", "x");
        CHECK(ensure_prefix_opt_link(cellar, "node", keg) == OptLinkAction::Created);
        CHECK(fs::read_symlink(t.path / "opt" / "node") == fs::path("../Cellar/node/25.1.0"));
        CHECK(ensure_prefix_opt_link(cellar, "node", keg) == OptLinkAction::Unchanged);

        // Dangling link (its keg was deleted out from under it) is replaced.
        fs::remove(t.path / "opt" / "node");
        fs::create_symlink("../Cellar/node/1.0", t.path / "opt" / "node");
        CHECK(ensure_prefix_opt_link(cellar, "node", keg) == OptLinkAction::Created);
        CHECK(fs::equivalent(t.path / "opt" / "node", keg));
    }

    TEST_CASE("ensure_prefix_opt_link never repoints a link to a keg den does not own") {
        RevTmp t;
        const auto cellar = t.path / "Cellar";
        const auto brew = cellar / "libgit2" / "1.8.4"; // Homebrew's: no den receipt
        write(brew / "lib" / "libgit2.1.8.dylib", "x");
        const auto den = cellar / "libgit2" / "1.9.7_2";
        write(den / "lib" / "libgit2.1.9.dylib", "x");
        mark_keg_owned(den, "libgit2", "1.9.7_2");
        fs::create_directories(t.path / "opt");
        fs::create_symlink(brew, t.path / "opt" / "libgit2");

        CHECK(ensure_prefix_opt_link(cellar, "libgit2", den) == OptLinkAction::Kept);
        CHECK(fs::read_symlink(t.path / "opt" / "libgit2") == brew);

        // A real directory at the link path is never replaced either.
        const auto other = cellar / "foo" / "1.0";
        write(other / "x", "x");
        fs::create_directories(t.path / "opt" / "foo");
        CHECK(ensure_prefix_opt_link(cellar, "foo", other) == OptLinkAction::Kept);
        CHECK(fs::is_directory(t.path / "opt" / "foo"));
        CHECK_FALSE(fs::is_symlink(t.path / "opt" / "foo"));
    }

    TEST_CASE("ensure_prefix_opt_link moves den's own link forward, never backward") {
        RevTmp t;
        const auto cellar = t.path / "Cellar";
        const auto v1 = cellar / "node" / "24.0.0";
        const auto v2 = cellar / "node" / "25.1.0";
        write(v1 / "bin" / "node", "x");
        write(v2 / "bin" / "node", "x");
        mark_keg_owned(v1, "node", "24.0.0");
        mark_keg_owned(v2, "node", "25.1.0");

        CHECK(ensure_prefix_opt_link(cellar, "node", v1) == OptLinkAction::Created);
        CHECK(ensure_prefix_opt_link(cellar, "node", v2) == OptLinkAction::Repointed);
        CHECK(fs::read_symlink(t.path / "opt" / "node") == fs::path("../Cellar/node/25.1.0"));
        CHECK(ensure_prefix_opt_link(cellar, "node", v1) == OptLinkAction::Kept);
        CHECK(fs::read_symlink(t.path / "opt" / "node") == fs::path("../Cellar/node/25.1.0"));
    }

    TEST_CASE("cleanup removes the opt link with its keg and hands it to the survivor") {
        RevTmp t;
        const auto den_home = t.path / "den";
        const auto cellar = t.path / "Cellar";
        const auto keep = cellar / "node" / "24.0.0";
        const auto gone = cellar / "node" / "25.1.0";
        write(keep / "bin" / "node", "x");
        write(gone / "bin" / "node", "x");
        mark_keg_owned(keep, "node", "24.0.0");
        mark_keg_owned(gone, "node", "25.1.0");
        REQUIRE(ensure_prefix_opt_link(cellar, "node", gone) == OptLinkAction::Created);
        write(den_home / "manifests" / "ROOT" / "manifest.json",
              R"({"packages": {"homebrew": {"node": "24.0.0"}}, "auto_deps": {}})");

        auto report = cleanup_kegs(cellar, den_home, den_home / "cache", false);
        CHECK(report.removed.size() == 1);
        CHECK_FALSE(fs::exists(gone));
        CHECK(fs::read_symlink(t.path / "opt" / "node") == fs::path("../Cellar/node/24.0.0"));

        // Last keg gone: the link goes too, rather than dangling.
        const auto solo = cellar / "vite" / "8.3.5";
        write(solo / "bin" / "vite", "x");
        mark_keg_owned(solo, "vite", "8.3.5");
        REQUIRE(ensure_prefix_opt_link(cellar, "vite", solo) == OptLinkAction::Created);
        cleanup_kegs(cellar, den_home, den_home / "cache", false);
        CHECK_FALSE(fs::exists(solo));
        CHECK_FALSE(fs::is_symlink(t.path / "opt" / "vite"));
    }
}

} // namespace test
} // namespace den

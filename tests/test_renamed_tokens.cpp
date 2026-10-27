// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Regression: den did not follow Homebrew renames. Casks list their previous
// tokens in "old_tokens" (vibe-notch ← claude-island, tigervnc ←
// tigervnc-viewer) and formulae in "oldnames"; brew installs the current
// package when given an old name, but den reported "not found". Found by the
// 2026-10 den readiness audit.

#include <doctest.h>

#include "download/sha256.h"
#include "index/index.h"
#include "index/index_cache.h"

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

struct RenameTmp {
    fs::path path;
    RenameTmp() {
        std::string tmpl = fs::temp_directory_path() / "den_rename_XXXXXX";
        char* r = ::mkdtemp(tmpl.data());
        if (!r) {
            throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
        }
        path = fs::path(r).lexically_normal();
    }
    ~RenameTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    RenameTmp(const RenameTmp&) = delete;
    RenameTmp& operator=(const RenameTmp&) = delete;
};

void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << s;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

json cask_json(const std::string& token, const json& old_tokens) {
    return {
        {"token", token},
        {"name", json::array({token})},
        {"desc", "test"},
        {"homepage", "https://example.com"},
        {"version", "1.0"},
        {"url", "https://example.com/" + token + ".zip"},
        {"sha256", std::string(64, 'a')},
        {"artifacts", json::array({{{"app", json::array({token + ".app"})}}})},
        {"old_tokens", old_tokens},
    };
}

json formula_json(const std::string& name, const json& oldnames) {
    return {
        {"name", name},
        {"desc", "test"},
        {"homepage", "https://example.com"},
        {"license", "MIT"},
        {"versions", {{"stable", "1.0"}, {"bottle", false}}},
        {"dependencies", json::array()},
        {"build_dependencies", json::array()},
        {"oldnames", oldnames},
    };
}

struct DenRun {
    std::string output;
    int exit_code;
};

DenRun run_den(const std::string& args, const fs::path& home, const fs::path& prefix) {
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

/// Stage a bottle for `name` in den's archive cache (no download) and write a
/// one-package index that lists `old_name` as its previous name.
void stage_renamed_formula(const fs::path& home, const std::string& name,
                           const std::string& old_name) {
    const auto src = home / "bottle-src";
    write(src / name / "1.0" / "bin" / name, "#!/bin/sh\necho " + name + "-ok\n");
    fs::permissions(src / name / "1.0" / "bin" / name, fs::perms::owner_all, fs::perm_options::add);
    const auto tarball = home / (name + ".tar.gz");
    const std::string cmd = "tar -czf '" + tarball.string() + "' -C '" + src.string() + "' " + name;
    REQUIRE(std::system(cmd.c_str()) == 0);
    const auto data = slurp(tarball);
    const auto sha = hash_string(data);
    write(home / "den" / "cache" / "archives" / (sha + ".tar.gz"), data);

    json pkg = {
        {"name", name},
        {"version", "1.0"},
        {"description", "test"},
        {"homepage", ""},
        {"license", ""},
        {"artifact_type", "binary"},
        {"deprecated", false},
        {"disabled", false},
        {"dependencies", json::array()},
        {"build_dependencies", json::array()},
        {"old_names", json::array({old_name})},
        {"archives",
         {{"all",
           {{"url", "file:///nonexistent/" + name + ".tar.gz"},
            {"sha256", sha},
            {"relocation", "portable"}}}}},
    };
    write(home / "den" / "cache" / "index.json", json{{"packages", json::array({pkg})}}.dump(2));
}

} // namespace

TEST_SUITE("index::renamed_tokens") {

    TEST_CASE("transform records cask old_tokens and formula oldnames") {
        json casks = json::array({
            cask_json("vibe-notch", json::array({"claude-island"})),
            cask_json("tigervnc", json::array({"tigervnc-viewer"})),
            cask_json("plain", nullptr),
        });
        json formulae = json::array({formula_json("newtool", json::array({"oldtool"}))});
        auto idx = transform_homebrew(formulae.dump(), casks.dump());

        const auto* vn = idx.find("vibe-notch");
        REQUIRE(vn != nullptr);
        CHECK(vn->old_names == std::vector<std::string>{"claude-island"});
        const auto* pl = idx.find("plain");
        REQUIRE(pl != nullptr);
        CHECK(pl->old_names.empty());
        const auto* nt = idx.find("newtool");
        REQUIRE(nt != nullptr);
        CHECK(nt->old_names == std::vector<std::string>{"oldtool"});

        CHECK(idx.find("claude-island") == nullptr);
        const auto* r = idx.find_renamed("claude-island");
        REQUIRE(r != nullptr);
        CHECK(r->name == vn->name);
        const auto* tv = idx.find_renamed("tigervnc-viewer");
        REQUIRE(tv != nullptr);
        CHECK(tv->name == idx.find("tigervnc")->name);
        const auto* ft = idx.find_renamed("oldtool");
        REQUIRE(ft != nullptr);
        CHECK(ft->name == "newtool");
        CHECK(idx.find_renamed("nothing-like-this") == nullptr);
    }

    TEST_CASE("a live package wins over a rename that claims its name") {
        json formulae = json::array({
            formula_json("newtool", json::array({"oldtool"})),
            formula_json("oldtool", json::array()),
        });
        auto idx = transform_homebrew(formulae.dump(), "[]");
        CHECK(idx.find("oldtool") != nullptr);
        CHECK(idx.find_renamed("oldtool") == nullptr);
    }

    TEST_CASE("old names survive index.json and the binary lookup cache") {
        RenameTmp t;
        json casks = json::array({cask_json("vibe-notch", json::array({"claude-island"}))});
        auto idx = transform_homebrew("[]", casks.dump());
        const auto path = t.path / "index.json";
        save_index(idx, path);

        // Lazily served from the binary cache.
        auto cached = load_index(path);
        const auto* r = cached.find_renamed("claude-island");
        REQUIRE(r != nullptr);
        CHECK(r->old_names == std::vector<std::string>{"claude-island"});

        // Straight from JSON.
        auto full = load_index_json(path);
        const auto* j = full.find_renamed("claude-island");
        REQUIRE(j != nullptr);
        CHECK(j->name == r->name);

        // Record codec.
        auto enc = index_cache::encode_package(*r);
        auto dec = index_cache::decode_package(enc.data(), enc.size());
        CHECK(dec.old_names == r->old_names);
    }

    TEST_CASE("den install <old name> installs the renamed package") {
        RenameTmp t;
        const auto prefix = t.path / "prefix";
        stage_renamed_formula(t.path, "newtool", "oldtool");

        auto info = run_den("info oldtool", t.path, prefix);
        INFO(info.output);
        CHECK(info.exit_code == 0);
        CHECK(info.output.find("oldtool has been renamed to newtool") != std::string::npos);

        auto r = run_den("install oldtool", t.path, prefix);
        INFO(r.output);
        REQUIRE(r.exit_code == 0);
        CHECK(r.output.find("oldtool has been renamed to newtool") != std::string::npos);
        CHECK(fs::is_regular_file(prefix / "Cellar" / "newtool" / "1.0" / "bin" / "newtool"));
        CHECK_FALSE(fs::exists(prefix / "Cellar" / "oldtool"));
    }
}

} // namespace test
} // namespace den

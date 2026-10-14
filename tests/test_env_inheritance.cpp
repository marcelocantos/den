// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Regression: env-origin inheritance. Child envs inherit their parent's
// packages (README: "den env create /ml # child of root, inherits all
// packages"; agents-guide: "Child environments inherit from parents and can
// override specific versions").

#include <doctest.h>

#include "env/manifest.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace den {
namespace test {

namespace fs = std::filesystem;

namespace {
struct InhTmp {
    fs::path path;
    InhTmp() {
        path = fs::temp_directory_path() /
               ("den_test_inherit_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~InhTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void put(const fs::path& h, const std::string& env, const std::string& name, const std::string& ver,
         std::optional<std::string> origin = std::nullopt) {
    auto m = read_manifest(h, env);
    m.packages["homebrew"][name] = ver;
    if (origin) {
        m.origin = origin;
    }
    write_manifest(h, env, m);
}
} // namespace

TEST_SUITE("env::inheritance") {

    TEST_CASE("env_parent walks path components") {
        CHECK(env_parent("/") == "");
        CHECK(env_parent("/ml") == "/");
        CHECK(env_parent("/ml/") == "/");
        CHECK(env_parent("/work/legacy") == "/work");
        CHECK(env_parent("/a/b/c") == "/a/b");
    }

    TEST_CASE("child without explicit origin inherits root packages") {
        InhTmp t;
        put(t.path, "/", "tree", "2.3.2");
        put(t.path, "/ml", "numpy-dep", "1.0");
        auto r = resolve(t.path, "/ml");
        CHECK(r.at("tree") == "2.3.2");
        CHECK(r.at("numpy-dep") == "1.0");
        // Root does not see the child's packages.
        CHECK(resolve(t.path, "/").count("numpy-dep") == 0);
    }

    TEST_CASE("child overrides parent version; grandchild inherits through the chain") {
        InhTmp t;
        put(t.path, "/", "python", "3.12");
        put(t.path, "/", "jq", "1.7");
        put(t.path, "/ml", "python", "3.11");
        put(t.path, "/ml/exp", "torch", "2.6");
        auto r = resolve(t.path, "/ml/exp");
        CHECK(r.at("python") == "3.11");
        CHECK(r.at("jq") == "1.7");
        CHECK(r.at("torch") == "2.6");
        auto pp = resolve_per_provider(t.path, "/ml/exp");
        CHECK(pp.at("homebrew").at("python") == "3.11");
        CHECK(pp.at("homebrew").at("jq") == "1.7");
    }

    TEST_CASE("missing intermediate env does not break the chain") {
        InhTmp t;
        put(t.path, "/", "jq", "1.7");
        put(t.path, "/work/legacy", "make", "4.4"); // /work has no manifest
        auto r = resolve(t.path, "/work/legacy");
        CHECK(r.at("jq") == "1.7");
        CHECK(r.at("make") == "4.4");
    }

    TEST_CASE("explicit origin wins over path parent") {
        InhTmp t;
        put(t.path, "/", "jq", "1.7");
        put(t.path, "/base", "cmake", "3.30");
        put(t.path, "/proj", "ninja", "1.12", std::string("/base"));
        auto r = resolve(t.path, "/proj");
        CHECK(r.at("cmake") == "3.30");
        CHECK(r.at("jq") == "1.7"); // /base inherits root
        CHECK(r.at("ninja") == "1.12");
    }

    TEST_CASE("origin cycles terminate") {
        InhTmp t;
        put(t.path, "/a", "x", "1", std::string("/b"));
        put(t.path, "/b", "y", "2", std::string("/a"));
        auto r = resolve(t.path, "/a");
        CHECK(r.at("x") == "1");
        CHECK(r.at("y") == "2");
    }
}

} // namespace test
} // namespace den

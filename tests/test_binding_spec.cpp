// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T69 verification harness — binding spec (verifies T28).
//
// Tests each binding directive type against the environment surface it
// produces after `apply_bindings` runs over a fake keg:
//
//   path:       bin/foo          → foo appears on PATH (symlink in env/bin/)
//   env:        FOO=/keg/path   → FOO set to absolute path
//   env-append: BAR=/keg/lib    → BAR contains the keg-lib segment
//   alias:      py3 -> python3  → env/bin/py3 symlink → python3 target
//
// Expected header: `store/binding_spec.h`, exporting:
//   struct BindingSpec { ... };
//   BindingSpec parse_binding_spec(const std::string& yaml_text);
//   void apply_bindings(const BindingSpec&, const fs::path& keg,
//                       const fs::path& env_dir);
//
// If the header is absent the entire suite is skipped.

#include <doctest.h>

#if __has_include("store/binding_spec.h")
#include "store/binding_spec.h"
#define T28_IMPLEMENTED 1
#else
#define T28_IMPLEMENTED 0
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace den {
namespace test {

namespace fs = std::filesystem;

struct BindTmpDir {
    fs::path path;

    BindTmpDir() {
        path =
            fs::temp_directory_path() /
            ("den_test_bind_" +
             std::to_string(
                 std::hash<std::string>{}(std::to_string(reinterpret_cast<uintptr_t>(this))) ^
                 static_cast<size_t>(std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::create_directories(path);
    }

    ~BindTmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    BindTmpDir(const BindTmpDir&) = delete;
    BindTmpDir& operator=(const BindTmpDir&) = delete;

    // Create a fake keg with a bin/ and lib/pkgconfig/ tree.
    fs::path make_keg(const std::string& name) {
        auto keg = path / "store" / name / "1.0";
        fs::create_directories(keg / "bin");
        fs::create_directories(keg / "lib" / "pkgconfig");
        std::ofstream(keg / "bin" / name) << "#!/bin/sh\n";
        std::ofstream(keg / "bin" / (name + "3")) << "#!/bin/sh\n";
        std::ofstream(keg / "lib" / "pkgconfig" / (name + ".pc")) << "Name: " << name << "\n";
        return keg;
    }

    fs::path make_env() {
        auto e = path / "env";
        fs::create_directories(e / "bin");
        return e;
    }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_SUITE("binding_spec [T28]") {

#if !T28_IMPLEMENTED
    TEST_CASE("SKIP — T28 (explicit bindings) not yet implemented") {
        WARN("T28 not implemented — skipping binding spec tests");
    }
#else
    // ------------------------------------------------------------------
    // path: directive
    // ------------------------------------------------------------------

    TEST_CASE("path: directive creates a symlink in env/bin/") {
        BindTmpDir tmp;
        auto keg = tmp.make_keg("ffmpeg");
        auto env = tmp.make_env();

        const std::string spec_yaml = "bindings:\n  - path: bin/ffmpeg\n";
        auto spec = parse_binding_spec(spec_yaml);
        apply_bindings(spec, keg, env);

        REQUIRE(fs::is_symlink(env / "bin" / "ffmpeg"));
        CHECK(fs::read_symlink(env / "bin" / "ffmpeg") == keg / "bin" / "ffmpeg");
    }

    TEST_CASE("path: directive does not expose other binaries") {
        BindTmpDir tmp;
        auto keg = tmp.make_keg("ffmpeg");
        auto env = tmp.make_env();

        // Only expose ffmpeg, not ffmpeg3.
        const std::string spec_yaml = "bindings:\n  - path: bin/ffmpeg\n";
        auto spec = parse_binding_spec(spec_yaml);
        apply_bindings(spec, keg, env);

        CHECK_FALSE(fs::exists(env / "bin" / "ffmpeg3"));
    }

    // ------------------------------------------------------------------
    // env: directive
    // ------------------------------------------------------------------

    TEST_CASE("env: directive is recorded in the env's var store") {
        BindTmpDir tmp;
        auto keg = tmp.make_keg("openssl");
        auto env = tmp.make_env();

        const std::string spec_yaml = "bindings:\n  - env: OPENSSL_DIR={keg}\n";
        auto spec = parse_binding_spec(spec_yaml);
        apply_bindings(spec, keg, env);

        // The implementation records env vars in a side-car file (e.g.
        // env/.den/vars.json or env/.den/env_vars) that den shell reads.
        // Assert the side-car file exists and contains the variable.
        bool var_found = false;
        for (const auto& entry : fs::recursive_directory_iterator(env / ".den")) {
            if (!entry.is_regular_file())
                continue;
            std::ifstream f(entry.path());
            std::string content((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
            if (content.find("OPENSSL_DIR") != std::string::npos) {
                var_found = true;
                break;
            }
        }
        CHECK(var_found);
    }

    // ------------------------------------------------------------------
    // env-append: directive
    // ------------------------------------------------------------------

    TEST_CASE("env-append: directive appends to the var in the side-car") {
        BindTmpDir tmp;
        auto keg = tmp.make_keg("pkg-config");
        auto env = tmp.make_env();

        const std::string spec_yaml =
            "bindings:\n  - env-append: PKG_CONFIG_PATH={keg}/lib/pkgconfig\n";
        auto spec = parse_binding_spec(spec_yaml);
        apply_bindings(spec, keg, env);

        bool append_found = false;
        for (const auto& entry : fs::recursive_directory_iterator(env / ".den")) {
            if (!entry.is_regular_file())
                continue;
            std::ifstream f(entry.path());
            std::string content((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
            if (content.find("PKG_CONFIG_PATH") != std::string::npos &&
                content.find("pkgconfig") != std::string::npos) {
                append_found = true;
                break;
            }
        }
        CHECK(append_found);
    }

    TEST_CASE("two env-append directives for the same var accumulate") {
        BindTmpDir tmp;
        auto keg1 = tmp.make_keg("openssl");
        auto keg2 = tmp.make_keg("libffi");
        auto env = tmp.make_env();

        const std::string spec1 =
            "bindings:\n  - env-append: PKG_CONFIG_PATH={keg}/lib/pkgconfig\n";
        const std::string spec2 =
            "bindings:\n  - env-append: PKG_CONFIG_PATH={keg}/lib/pkgconfig\n";

        apply_bindings(parse_binding_spec(spec1), keg1, env);
        apply_bindings(parse_binding_spec(spec2), keg2, env);

        // Both keg paths should appear in the side-car for PKG_CONFIG_PATH.
        std::string full_content;
        for (const auto& entry : fs::recursive_directory_iterator(env / ".den")) {
            if (!entry.is_regular_file())
                continue;
            std::ifstream f(entry.path());
            full_content +=
                std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }
        CHECK(full_content.find("openssl") != std::string::npos);
        CHECK(full_content.find("libffi") != std::string::npos);
    }

    // ------------------------------------------------------------------
    // alias: directive
    // ------------------------------------------------------------------

    TEST_CASE("alias: directive creates a named symlink pointing to the target") {
        BindTmpDir tmp;
        auto keg = tmp.make_keg("python");
        auto env = tmp.make_env();

        // python3 exists in keg/bin; expose it as py3.
        const std::string spec_yaml =
            "bindings:\n  - path: bin/python3\n  - alias: py3 -> python3\n";
        auto spec = parse_binding_spec(spec_yaml);
        apply_bindings(spec, keg, env);

        REQUIRE(fs::is_symlink(env / "bin" / "py3"));
        // The alias must ultimately resolve to the python3 binary.
        auto target = fs::read_symlink(env / "bin" / "py3");
        // Either a direct link to the keg binary or to the env/bin/python3 link.
        bool points_to_python3 = (target == keg / "bin" / "python3") ||
                                 (target == fs::path("python3")) ||
                                 (target == env / "bin" / "python3");
        CHECK(points_to_python3);
    }

    TEST_CASE("alias: to a non-existent target is an error or no-op") {
        BindTmpDir tmp;
        auto keg = tmp.make_keg("python");
        auto env = tmp.make_env();

        const std::string spec_yaml = "bindings:\n  - alias: ghost -> does_not_exist\n";
        auto spec = parse_binding_spec(spec_yaml);

        // Implementations may throw or silently skip; either is acceptable, but
        // the env must not contain a dangling symlink that passes is_symlink but
        // fails is_regular_file after resolution.
        try {
            apply_bindings(spec, keg, env);
        } catch (...) {
            // Acceptable — T28 may choose to fail loudly.
        }
        // If the symlink was created, it must not be dangling.
        if (fs::is_symlink(env / "bin" / "ghost")) {
            CHECK(fs::exists(env / "bin" / "ghost"));
        }
    }

    // ------------------------------------------------------------------
    // Combination
    // ------------------------------------------------------------------

    TEST_CASE("mixed spec applies all directives correctly") {
        BindTmpDir tmp;
        auto keg = tmp.make_keg("mylib");
        auto env = tmp.make_env();

        const std::string spec_yaml = "bindings:\n"
                                      "  - path: bin/mylib\n"
                                      "  - env: MYLIB_ROOT={keg}\n"
                                      "  - env-append: PKG_CONFIG_PATH={keg}/lib/pkgconfig\n"
                                      "  - alias: ml -> mylib\n";
        auto spec = parse_binding_spec(spec_yaml);
        apply_bindings(spec, keg, env);

        CHECK(fs::is_symlink(env / "bin" / "mylib"));
        CHECK(fs::is_symlink(env / "bin" / "ml"));

        std::string all;
        for (const auto& entry : fs::recursive_directory_iterator(env / ".den")) {
            if (!entry.is_regular_file())
                continue;
            std::ifstream f(entry.path());
            all +=
                std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }
        CHECK(all.find("MYLIB_ROOT") != std::string::npos);
        CHECK(all.find("PKG_CONFIG_PATH") != std::string::npos);
    }
#endif // T28_IMPLEMENTED

} // TEST_SUITE

} // namespace test
} // namespace den

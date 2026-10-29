// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// A Ruby build that fails and falls back to den's native recipe parser is not
// silent: it warns, the receipt records "fallback": "native", and `den doctor`
// names the keg. build.strict (default false) turns the fallback into an
// error.

#include <doctest.h>

#include "build/source_build.h"
#include "core/config.h"
#include "core/error.h"
#include "doctor/doctor.h"
#include "settings/settings.h"
#include "store/store.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace den {
namespace build_strict_test {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

struct TmpDir {
    fs::path path;
    TmpDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_strict_XXXXXX").string();
        if (!::mkdtemp(tmpl.data()))
            throw std::runtime_error("mkdtemp failed");
        path = tmpl;
    }
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TmpDir(const TmpDir&) = delete;
    TmpDir& operator=(const TmpDir&) = delete;
};

struct CaptureStderr {
    std::ostringstream buf;
    std::streambuf* old;
    CaptureStderr() : old(std::cerr.rdbuf(buf.rdbuf())) {}
    ~CaptureStderr() { std::cerr.rdbuf(old); }
};

SourceBuildReceipt built_receipt() {
    SourceBuildReceipt r;
    r.source = {"", "https://example.com/tree-1.0.tar.gz", std::string(64, 'c')};
    r.formula_tap = "homebrew/core";
    r.formula_path = "Formula/t/tree.rb";
    r.opt_prefix_baked = "/tmp/prefix/opt/tree";
    return r;
}

Config isolated_config(const fs::path& home) {
    Config cfg;
    cfg.den_home = home;
    cfg.prefix = home / "prefix";
    cfg.store = cfg.prefix / "Cellar";
    cfg.cache = home / "cache";
    cfg.homebrew_prefix = home / "brew";
    cfg.homebrew_cellar = home / "brew" / "Cellar";
    cfg.arch = Arch::Arm64;
    return cfg;
}

const std::string kWhy = "bundled Ruby build failed (exit 1): Error: undefined method `foo'";

} // namespace

TEST_SUITE("build_strict::settings") {

    TEST_CASE("build.strict defaults to false and is a boolean key") {
        TmpDir tmp;
        CHECK_FALSE(read_settings(tmp.path).build.strict);

        set_setting(tmp.path, "build.strict", "true");
        CHECK(read_settings(tmp.path).build.strict);
        std::ifstream in(tmp.path / "config.json");
        auto j = json::parse(in);
        CHECK(j["build"]["strict"] == true); // a JSON boolean, not "true"

        set_setting(tmp.path, "build.strict", "false");
        CHECK_FALSE(read_settings(tmp.path).build.strict);
        CHECK_THROWS_AS(set_setting(tmp.path, "build.strict", "yes"), std::invalid_argument);
    }

    TEST_CASE("build.strict survives a settings round-trip with other keys") {
        TmpDir tmp;
        Settings s;
        s.build.strict = true;
        s.daemon.auto_upgrade = false;
        write_settings(tmp.path, s);
        auto back = read_settings(tmp.path);
        CHECK(back.build.strict);
        CHECK_FALSE(back.daemon.auto_upgrade);
    }
}

TEST_SUITE("build_strict::fallback") {

    TEST_CASE("by default a native fallback warns and is recorded in the receipt") {
        TmpDir tmp;
        auto r = built_receipt();
        CaptureStderr err;
        allow_native_fallback(tmp.path, "tree", "1.0", kWhy, r);
        const auto out = err.buf.str();
        CHECK(out.find("Warning: " + kWhy) != std::string::npos);
        CHECK(out.find("building tree 1.0 with den's native recipe parser") != std::string::npos);
        CHECK(out.find("build.strict true") != std::string::npos);
        CHECK(r.fallback == "native");
        CHECK(r.fallback_reason == kWhy);
    }

    TEST_CASE("build.strict makes a native fallback an error") {
        TmpDir tmp;
        set_setting(tmp.path, "build.strict", "true");
        auto r = built_receipt();
        CaptureStderr err;
        try {
            allow_native_fallback(tmp.path, "tree", "1.0", kWhy, r);
            FAIL("expected UserError");
        } catch (const UserError& e) {
            const std::string what = e.what();
            CHECK(what.find(kWhy) != std::string::npos);
            CHECK(what.find("build.strict is set") != std::string::npos);
            CHECK(what.find("den set build.strict false") != std::string::npos);
        }
        CHECK(r.fallback.empty());
        CHECK(err.buf.str().empty());
    }

    TEST_CASE("the receipt records fallback=native, and only for a fallback build") {
        TmpDir tmp;
        auto keg = tmp.path / "tree/1.0";
        fs::create_directories(keg);
        auto r = built_receipt();
        r.fallback = "native";
        r.fallback_reason = kWhy;
        mark_keg_owned(keg, "tree", "1.0", 0, &r);
        {
            std::ifstream in(keg / kDenReceiptFilename);
            auto j = json::parse(in);
            CHECK(j["fallback"] == "native");
            CHECK(j["fallback_reason"] == kWhy);
        }
        auto back = read_source_build_receipt(keg);
        REQUIRE(back);
        CHECK(back->fallback == "native");
        CHECK(back->fallback_reason == kWhy);

        auto ruby = tmp.path / "jq/1.7";
        fs::create_directories(ruby);
        const auto plain = built_receipt();
        mark_keg_owned(ruby, "jq", "1.7", 0, &plain);
        std::ifstream in(ruby / kDenReceiptFilename);
        auto j = json::parse(in);
        CHECK_FALSE(j.contains("fallback"));
        CHECK(read_source_build_receipt(ruby)->fallback.empty());
    }
}

TEST_SUITE("build_strict::doctor") {

    TEST_CASE("doctor lists a native-fallback keg and warns about it") {
        TmpDir tmp;
        auto cfg = isolated_config(tmp.path);
        auto fallback = built_receipt();
        fallback.fallback = "native";
        fallback.fallback_reason = kWhy;
        fs::create_directories(cfg.store / "tree/1.0");
        mark_keg_owned(cfg.store / "tree/1.0", "tree", "1.0", 0, &fallback);
        const auto ruby = built_receipt();
        fs::create_directories(cfg.store / "jq/1.7");
        mark_keg_owned(cfg.store / "jq/1.7", "jq", "1.7", 0, &ruby);

        auto note = format_source_built_kegs(source_built_kegs(cfg.store, nullptr));
        CHECK(note.find("tree 1.0 (homebrew/core) [fallback=native]") != std::string::npos);
        CHECK(note.find("jq 1.7 (homebrew/core)\n") != std::string::npos);

        auto findings = doctor(cfg);
        int matched = 0;
        for (const auto& f : findings) {
            if (f.message.find("fallback=native") == std::string::npos)
                continue;
            ++matched;
            CHECK(f.severity == Severity::Warning);
            CHECK(f.message.find("1 source-built keg(s)") != std::string::npos);
            CHECK(f.message.find("tree/1.0: " + kWhy) != std::string::npos);
            CHECK(f.message.find("jq/1.7") == std::string::npos);
            CHECK(f.message.find("build.strict true") != std::string::npos);
        }
        CHECK(matched == 1);
    }

    TEST_CASE("doctor does not warn when every source build used Ruby") {
        TmpDir tmp;
        auto cfg = isolated_config(tmp.path);
        const auto ruby = built_receipt();
        fs::create_directories(cfg.store / "jq/1.7");
        mark_keg_owned(cfg.store / "jq/1.7", "jq", "1.7", 0, &ruby);
        for (const auto& f : doctor(cfg))
            CHECK(f.message.find("fallback=native") == std::string::npos);
    }
}

} // namespace build_strict_test
} // namespace den

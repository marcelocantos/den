// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Casks link their own binaries ("==> Linked <name>"), so the env
// materialise that follows a cask-only install or uninstall creates
// nothing and used to print "Materialised: 0 symlinks." as if the
// operation had done nothing. The count is now omitted in that case.

#include <doctest.h>

#include "cask/cask.h"
#include "cli/install.h"
#include "core/config.h"
#include "index/index.h"
#include "provider/stub_provider.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct Scratch {
    fs::path root;
    Config cfg;
    Scratch() {
        root = fs::temp_directory_path() / ("den-install-output-" + std::to_string(::getpid()));
        fs::remove_all(root);
        cfg.den_home = root / "den";
        cfg.store = root / "store";
        cfg.cache = cfg.den_home / "cache";
        fs::create_directories(cfg.den_home);
        fs::create_directories(cfg.store);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    // A minimal den cask receipt, as install_cask records it.
    void stage_cask(const std::string& token, const std::string& version) {
        auto dir = cask_version_path(cfg, token, version);
        fs::create_directories(dir);
        std::ofstream(dir / std::string(kDenCaskReceiptFilename))
            << R"({"poured_by":"den","token":")" << token << R"(","version":")" << version
            << R"("})";
    }
};

struct CaptureCout {
    std::ostringstream out;
    std::streambuf* old;
    CaptureCout() : old(std::cout.rdbuf(out.rdbuf())) {}
    ~CaptureCout() { std::cout.rdbuf(old); }
};

} // namespace

TEST_SUITE("cli::install_output") {
    TEST_CASE("the link count is omitted only for a cask-only no-op materialise") {
        CHECK(report_link_count(0, false));
        CHECK(report_link_count(3, false));
        CHECK(report_link_count(3, true));
        CHECK_FALSE(report_link_count(0, true));
    }

    TEST_CASE("cask_only needs every name to be a den-installed cask") {
        Scratch s;
        s.stage_cask("tool", "1.0");
        CHECK(cask_only(s.cfg, {"tool"}));
        CHECK(cask_only(s.cfg, {"cask/tool"}));
        CHECK_FALSE(cask_only(s.cfg, {"tool", "jq"}));
        CHECK_FALSE(cask_only(s.cfg, {"jq"}));
        CHECK_FALSE(cask_only(s.cfg, {}));
        // A caskroom directory without a den receipt is not a den cask.
        fs::create_directories(cask_version_path(s.cfg, "stray", "2.0"));
        CHECK_FALSE(cask_only(s.cfg, {"stray"}));
    }

    TEST_CASE("uninstalling a cask does not report 'Materialised: 0 symlinks'") {
        Scratch s;
        PackageIndex idx;
        StubProvider p;
        install_packages(s.cfg, p, idx, {"demo"});
        s.stage_cask("demo", "0.1.0");
        CaptureCout cap;
        uninstall_packages(s.cfg, p, {"demo"});
        INFO(cap.out.str());
        CHECK(cap.out.str().find("Removed 'demo'") != std::string::npos);
        CHECK(cap.out.str().find("Materialised:") == std::string::npos);
    }

    TEST_CASE("uninstalling a formula still reports the link count") {
        Scratch s;
        PackageIndex idx;
        StubProvider p;
        install_packages(s.cfg, p, idx, {"demo"});
        CaptureCout cap;
        uninstall_packages(s.cfg, p, {"demo"});
        INFO(cap.out.str());
        CHECK(cap.out.str().find("Materialised: 0 symlinks.") != std::string::npos);
    }
}

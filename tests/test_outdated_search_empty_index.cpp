// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// `den outdated` and `den search` read the package index. When it is missing,
// empty, or unparseable they used to print "Index is empty" and exit 0, which
// reads as "nothing outdated" / "no matches". They now go through
// require_index(), like `den upgrade` (#166): the CLI prints
// "error: the package index is empty; run `den update` first" and exits 1.
//
// These run the commands in-process through Cli::run against a temporary
// DEN_HOME/DEN_PREFIX, restoring the environment afterwards.

#include <doctest.h>

#include "cli/cli.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct OsTmp {
    fs::path path;
    OsTmp() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("den-os-emptyindex-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~OsTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    OsTmp(const OsTmp&) = delete;
    OsTmp& operator=(const OsTmp&) = delete;
};

// Sets an environment variable for the lifetime of the guard.
class EnvGuard {
  public:
    EnvGuard(const char* name, const std::string& value) : name_(name) {
        if (const char* old = std::getenv(name))
            old_ = old;
        ::setenv(name, value.c_str(), 1);
    }
    ~EnvGuard() {
        if (old_)
            ::setenv(name_, old_->c_str(), 1);
        else
            ::unsetenv(name_);
    }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

  private:
    const char* name_;
    std::optional<std::string> old_;
};

// Redirects std::cout and std::cerr into one buffer for the guard's lifetime.
class StreamCapture {
  public:
    StreamCapture() : out_(std::cout.rdbuf(buf_.rdbuf())), err_(std::cerr.rdbuf(buf_.rdbuf())) {}
    ~StreamCapture() {
        std::cout.rdbuf(out_);
        std::cerr.rdbuf(err_);
    }
    StreamCapture(const StreamCapture&) = delete;
    StreamCapture& operator=(const StreamCapture&) = delete;
    std::string str() const { return buf_.str(); }

  private:
    std::ostringstream buf_;
    std::streambuf* out_;
    std::streambuf* err_;
};

struct RunResult {
    int rc;
    std::string output;
};

// A temporary den home with its own prefix; nothing outside it is read.
struct Home {
    OsTmp tmp;
    fs::path prefix = tmp.path / "prefix";
    EnvGuard home{"DEN_HOME", tmp.path.string()};
    EnvGuard den_prefix{"DEN_PREFIX", prefix.string()};
    EnvGuard brew_prefix{"HOMEBREW_PREFIX", prefix.string()};
    EnvGuard brew_cellar{"HOMEBREW_CELLAR", (prefix / "Cellar").string()};

    fs::path index() const { return tmp.path / "cache" / "index.json"; }
    void write_index(const std::string& body) const {
        fs::create_directories(index().parent_path());
        std::ofstream(index()) << body;
    }

    RunResult run(std::vector<std::string> args) const {
        args.insert(args.begin(), "den");
        std::vector<char*> argv;
        for (auto& a : args)
            argv.push_back(a.data());
        argv.push_back(nullptr);
        StreamCapture cap;
        den::Cli cli;
        int rc = cli.run(static_cast<int>(args.size()), argv.data());
        return {rc, cap.str()};
    }
};

const std::string kMsg = "error: the package index is empty; run `den update` first";

const std::vector<std::vector<std::string>> kForms = {
    {"outdated"},
    {"search", "python"},
    {"search", "nosuchpkg"},
};

void check_refused(const Home& h) {
    for (const auto& form : kForms) {
        CAPTURE(form.front());
        CAPTURE(form.back());
        auto r = h.run(form);
        CAPTURE(r.output);
        CHECK(r.rc == 1);
        CHECK(r.output.find(kMsg) != std::string::npos);
        CHECK(r.output.find("Index is empty") == std::string::npos);
    }
}

const char* kPopulated = R"({
  "packages": [{
    "name": "realpkg", "version": "1.0.0", "description": "present",
    "homepage": "", "license": "", "artifact_type": "binary",
    "deprecated": false, "disabled": false,
    "dependencies": [], "build_dependencies": [], "archives": {}
  }]
})";

} // namespace

TEST_SUITE("outdated_search::empty_index") {

    TEST_CASE("outdated and search refuse a missing index") {
        Home h;
        REQUIRE_FALSE(fs::exists(h.index()));
        check_refused(h);
    }

    TEST_CASE("outdated and search refuse an index with no packages") {
        Home h;
        h.write_index(R"({"packages": []})");
        check_refused(h);
    }

    TEST_CASE("outdated and search refuse an unparseable index") {
        Home h;
        h.write_index("{not json");
        check_refused(h);
    }

    TEST_CASE("outdated and search succeed with a populated index") {
        Home h;
        h.write_index(kPopulated);

        auto out = h.run({"outdated"});
        CAPTURE(out.output);
        CHECK(out.rc == 0);
        CHECK(out.output.find("No packages installed") != std::string::npos);

        auto hit = h.run({"search", "realpkg"});
        CAPTURE(hit.output);
        CHECK(hit.rc == 0);
        CHECK(hit.output.find("realpkg") != std::string::npos);

        auto miss = h.run({"search", "nosuchpkg"});
        CAPTURE(miss.output);
        CHECK(miss.rc == 0);
        CHECK(miss.output.find("No packages matching 'nosuchpkg'.") != std::string::npos);
        CHECK(miss.output.find(kMsg) == std::string::npos);
    }
}

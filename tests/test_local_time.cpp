// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// den prints one human-readable time, local "YYYY-MM-DD HH:MM", through
// format_local_time: `den log`, `den trust review`, the trust ledger's
// "since" and the daemon's disabled_blocked entries.

#include <doctest.h>

#include "activity/activity.h"
#include "cli/cli.h"
#include "cli/trust_review.h"
#include "core/local_time.h"
#include "trust/degraded_installs.h"

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace den;
namespace fs = std::filesystem;

namespace {

const uint64_t kAt = 1793000000;

// The format den has always printed, computed independently.
std::string reference(uint64_t secs) {
    std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
    REQUIRE(localtime_r(&t, &tm));
    char buf[32];
    REQUIRE(std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm) > 0);
    return buf;
}

bool has(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

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

struct Tmp {
    fs::path path;
    Tmp() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("den-local-time-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    Tmp(const Tmp&) = delete;
    Tmp& operator=(const Tmp&) = delete;
};

DegradedInstall entry(const std::string& name, long long at) {
    DegradedInstall d;
    d.name = name;
    d.version = "1.0";
    d.reason = "no-replica-entry";
    d.recorded_at = at;
    return d;
}

} // namespace

TEST_SUITE("local_time") {
    TEST_CASE("format_local_time prints local YYYY-MM-DD HH:MM") {
        CHECK(format_local_time(kAt) == reference(kAt));
        CHECK(format_local_time(0) == reference(0));
        CHECK(format_local_time(kAt, "") == reference(kAt));
        CHECK(format_local_time(kAt).size() == 16);
    }

    TEST_CASE("the trust ledger's since") {
        DegradedInstallSummary s;
        s.total = 1;
        s.by_reason["no-replica-entry"] = 1;
        s.installs.push_back(entry("jq", static_cast<long long>(kAt)));
        s.since = static_cast<long long>(kAt);
        CHECK(has(format_trust_ledger(s), " since " + reference(kAt)));
        CHECK(format_trust_ledger(s).ends_with(" since " + reference(kAt)));
    }

    TEST_CASE("den trust review rows, and ? for an unknown time") {
        DegradedInstallSummary s;
        s.refused = 1;
        s.refused_installs.push_back(entry("jq", static_cast<long long>(kAt)));
        s.total = 1;
        s.installs.push_back(entry("oniguruma", 0));
        std::ostringstream out;
        format_trust_review(out, s);
        CHECK(has(out.str(), "  jq@1.0  [no-replica-entry]  " + reference(kAt) + "\n"));
        CHECK(has(out.str(), "  oniguruma@1.0  [no-replica-entry]  ?\n"));
    }

    TEST_CASE("den log") {
        Tmp tmp;
        EnvGuard home{"HOME", tmp.path.string()};
        EnvGuard den_home{"DEN_HOME", (tmp.path / "den").string()};
        EnvGuard den_prefix{"DEN_PREFIX", (tmp.path / "prefix").string()};
        fs::create_directories(tmp.path / "den");
        record_activity(tmp.path / "den", {{kAt, "jq", "1.7.1", "1.8.2", "daemon"}});

        std::vector<std::string> args{"den", "log"};
        std::vector<char*> argv;
        for (auto& a : args)
            argv.push_back(a.data());
        argv.push_back(nullptr);
        std::ostringstream buf;
        auto* old = std::cout.rdbuf(buf.rdbuf());
        den::Cli cli;
        (void)cli.run(static_cast<int>(args.size()), argv.data());
        std::cout.rdbuf(old);
        CHECK(buf.str() == reference(kAt) + "  jq                      1.7.1 -> 1.8.2  (daemon)\n");
    }
}

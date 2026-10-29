// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// trust.unattended: what an install nobody is watching does when only one
// hash source vouches for it.
//
//   • Detection: the daemon, `--unattended` and den's launchd job are
//     unattended; a missing TTY on its own is not.
//   • Bottles: warn pours and records; refuse holds the package back, records
//     it as refused and keeps it pending. Mismatches and an unreadable
//     snapshot refuse in every mode.
//   • Source builds: off unattended unless trust.unattended_source_builds is
//     "allowlist" and the formula is allowlisted; even then a single-source
//     source hash defers rather than builds.
//   • The daemon screens its pending upgrades first, holds dependents of a
//     held formula too, and notifies once per newly held set.
//   • `den trust accept` lets one exact version through.

#include <doctest.h>

#include "cli/daemon_status.h"
#include "cli/trust_review.h"
#include "core/config.h"
#include "core/error.h"
#include "daemon/daemon.h"
#include "daemon/trust_screen.h"
#include "doctor/trust_checks.h"
#include "download/sha256.h"
#include "index/index.h"
#include "index/package.h"
#include "provider/exec.h"
#include "provider/homebrew_provider.h"
#include "settings/settings.h"
#include "trust/degraded_installs.h"
#include "trust/unattended.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct Tmp {
    fs::path path;
    explicit Tmp(const std::string& tag) {
        path = fs::temp_directory_path() /
               ("den-unattended-" + tag + "-" + std::to_string(::getpid()) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    Tmp(const Tmp&) = delete;
    Tmp& operator=(const Tmp&) = delete;
};

// set_unattended() is process-wide: restore interactive on scope exit.
struct UnattendedScope {
    explicit UnattendedScope(std::string reason) { set_unattended(std::move(reason)); }
    ~UnattendedScope() { set_unattended(std::nullopt); }
};

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

Config tmp_config(const fs::path& tmp) {
    Config cfg;
    cfg.den_home = tmp / "den";
    cfg.cache = cfg.den_home / "cache";
    cfg.store = tmp / "prefix" / "Cellar";
    cfg.prefix = cfg.store.parent_path();
    cfg.homebrew_cellar = cfg.store;
    fs::create_directories(cfg.cache / "archives");
    fs::create_directories(cfg.store);
    return cfg;
}

TrustPolicy policy(bool unattended, UnattendedMode mode, bool source_builds = false,
                   std::set<std::string> allow = {}) {
    TrustPolicy p;
    if (unattended)
        p.unattended = "test";
    p.mode = mode;
    p.source_builds = source_builds;
    p.source_build_allowlist = std::move(allow);
    return p;
}

DegradedInstall refused(const std::string& name, const std::string& version,
                        const std::string& reason = "unattended-refuse") {
    DegradedInstall d{name, version, reason, "default", {}};
    d.status = "no-replica-entry";
    d.bottle_sha = std::string(64, 'a');
    return d;
}

// A cached bottle for probe 1.0 that the replica has no entry for, so
// install() pours without the network and the cross-check is WarnProceed.
struct Fixture {
    Tmp tmp;
    Config cfg;
    PackageIndex idx;

    explicit Fixture(const std::string& tag) : tmp(tag), cfg(tmp_config(tmp.path)) {
        auto stage = tmp.path / "stage";
        write(stage / "probe" / "1.0" / "bin" / "probe", "#!/bin/sh\necho ok\n");
        auto tarball = tmp.path / "probe.tar.gz";
        REQUIRE(
            run_tool({"tar", "-czf", tarball.string(), "-C", stage.string(), "probe"}).exit_code ==
            0);
        auto sha = hash_file(tarball);
        fs::copy_file(tarball, cfg.cache / "archives" / (sha + ".tar.gz"));

        Package p;
        p.name = "probe";
        p.version = "1.0";
        Package::Archive ar;
        ar.url = "https://bottles.invalid/probe.tar.gz";
        ar.sha256 = sha;
        ar.relocation = RelocationType::Portable;
        p.archives["default"] = ar;
        idx.packages["probe"] = p;

        write(cfg.den_home / "trust" / "known_hashes.json",
              R"({"schema_version": 1, "hashes": {"unrelated--9.9--default": ")" +
                  std::string(64, 'b') + R"("}})");
    }

    fs::path keg() const { return cfg.store / "probe" / "1.0"; }

    void install() {
        HomebrewProvider provider(&idx);
        provider.install(cfg, "probe", "");
    }
};

Package formula(const std::string& name, const std::string& version) {
    Package p;
    p.name = name;
    p.version = version;
    return p;
}

} // namespace

TEST_SUITE("trust_unattended::policy") {

    TEST_CASE("parse_unattended_mode fails closed on unknown values") {
        CHECK(parse_unattended_mode("warn") == UnattendedMode::Warn);
        CHECK(parse_unattended_mode("refuse") == UnattendedMode::Refuse);
        CHECK(parse_unattended_mode("") == UnattendedMode::Refuse);
        CHECK(parse_unattended_mode("Warn ") == UnattendedMode::Refuse);
        CHECK(parse_unattended_mode("off") == UnattendedMode::Refuse);
    }

    TEST_CASE("detect_unattended: override, launchd label, nothing else") {
        CHECK(detect_unattended(std::string("daemon"), nullptr) == std::string("daemon"));
        CHECK(detect_unattended(std::string("--unattended"), "dev.den.daemon") ==
              std::string("--unattended"));
        auto launchd = detect_unattended(std::nullopt, "dev.den.daemon");
        REQUIRE(launchd);
        CHECK(launchd->find("dev.den.daemon") != std::string::npos);
        CHECK_FALSE(detect_unattended(std::nullopt, nullptr));
        CHECK_FALSE(detect_unattended(std::nullopt, ""));
        CHECK_FALSE(detect_unattended(std::nullopt, "application.com.apple.Terminal.123"));
    }

    TEST_CASE("set_unattended drives unattended_reason") {
        set_unattended(std::nullopt);
        if (const char* xpc = std::getenv("XPC_SERVICE_NAME");
            !xpc || std::string(xpc) != kDaemonLaunchdLabel)
            CHECK_FALSE(unattended_reason());
        {
            UnattendedScope u("--unattended");
            CHECK(unattended_reason() == std::string("--unattended"));
        }
        if (const char* xpc = std::getenv("XPC_SERVICE_NAME");
            !xpc || std::string(xpc) != kDaemonLaunchdLabel)
            CHECK_FALSE(unattended_reason());
    }

    TEST_CASE("bottle_action matrix") {
        auto interactive = policy(false, UnattendedMode::Refuse);
        auto warn = policy(true, UnattendedMode::Warn);
        auto refuse = policy(true, UnattendedMode::Refuse);
        for (const auto* p : {&interactive, &warn, &refuse}) {
            CHECK(bottle_action(*p, TrustDecision::Proceed, false) == BottleAction::Install);
            CHECK(bottle_action(*p, TrustDecision::Refuse, false) == BottleAction::Refuse);
            CHECK(bottle_action(*p, TrustDecision::Refuse, true) == BottleAction::Refuse);
        }
        CHECK(bottle_action(interactive, TrustDecision::WarnProceed, false) ==
              BottleAction::InstallDegraded);
        CHECK(bottle_action(warn, TrustDecision::WarnProceed, false) ==
              BottleAction::InstallDegraded);
        CHECK(bottle_action(refuse, TrustDecision::WarnProceed, false) == BottleAction::HoldBack);
        CHECK(bottle_action(refuse, TrustDecision::WarnProceed, true) ==
              BottleAction::InstallDegraded);
    }

    TEST_CASE("source_build_action matrix") {
        auto interactive = policy(false, UnattendedMode::Warn);
        auto off = policy(true, UnattendedMode::Warn, false, {"git"});
        auto allow = policy(true, UnattendedMode::Warn, true, {"git"});

        // Interactive: build unless the hashes disagree.
        CHECK(source_build_action(interactive, "zsh", TrustDecision::Proceed, false) ==
              SourceBuildAction::Build);
        CHECK(source_build_action(interactive, "zsh", TrustDecision::WarnProceed, false) ==
              SourceBuildAction::Build);
        CHECK(source_build_action(interactive, "zsh", TrustDecision::Refuse, false) ==
              SourceBuildAction::Refuse);

        // Unattended, source builds off: defer even an allowlisted, verified one.
        CHECK(source_build_action(off, "git", TrustDecision::Proceed, false) ==
              SourceBuildAction::Defer);
        CHECK(source_build_action(off, "git", TrustDecision::Refuse, false) ==
              SourceBuildAction::Refuse);

        // Allowlist mode: only allowlisted, cross-checked builds run.
        CHECK(source_build_action(allow, "git", TrustDecision::Proceed, false) ==
              SourceBuildAction::Build);
        CHECK(source_build_action(allow, "git", TrustDecision::WarnProceed, false) ==
              SourceBuildAction::Defer);
        CHECK(source_build_action(allow, "zsh", TrustDecision::Proceed, false) ==
              SourceBuildAction::Defer);

        // An accepted version builds unattended; a mismatch never does.
        CHECK(source_build_action(off, "zsh", TrustDecision::WarnProceed, true) ==
              SourceBuildAction::Build);
        CHECK(source_build_action(off, "zsh", TrustDecision::Refuse, true) ==
              SourceBuildAction::Refuse);
    }
}

TEST_SUITE("trust_unattended::settings") {

    TEST_CASE("defaults: warn, notify, source builds off, five-formula allowlist") {
        Tmp t("defaults");
        auto s = read_settings(t.path);
        CHECK(s.trust.unattended == "warn");
        CHECK(s.trust.notify);
        CHECK(s.trust.unattended_source_builds == "off");
        CHECK(s.trust.source_build_allowlist ==
              std::vector<std::string>{"gettext", "git", "openssl@3", "openssl@4", "pkgconf"});
        auto p = make_trust_policy(s.trust, std::nullopt);
        CHECK_FALSE(p.unattended);
        CHECK(p.mode == UnattendedMode::Warn);
        CHECK_FALSE(p.source_builds);
        CHECK(p.source_build_allowlist.count("git") == 1);
    }

    TEST_CASE("den set: trust keys round-trip and enumerations are validated") {
        Tmp t("set");
        set_setting(t.path, "trust.unattended", "refuse");
        set_setting(t.path, "trust.notify", "false");
        set_setting(t.path, "trust.unattended_source_builds", "allowlist");
        set_setting(t.path, "trust.source_build_allowlist", "git, pkgconf");
        CHECK_THROWS_AS(set_setting(t.path, "trust.unattended", "refuse!"), std::invalid_argument);
        CHECK_THROWS_AS(set_setting(t.path, "trust.unattended_source_builds", "on"),
                        std::invalid_argument);
        CHECK_THROWS_AS(set_setting(t.path, "trust.notify", "yes"), std::invalid_argument);

        auto s = read_settings(t.path);
        CHECK(s.trust.unattended == "refuse");
        CHECK_FALSE(s.trust.notify);
        CHECK(s.trust.unattended_source_builds == "allowlist");
        CHECK(s.trust.source_build_allowlist == std::vector<std::string>{"git", "pkgconf"});

        auto p = make_trust_policy(s.trust, std::string("daemon"));
        CHECK(p.unattended == std::string("daemon"));
        CHECK(p.mode == UnattendedMode::Refuse);
        CHECK_FALSE(p.notify);
        CHECK(p.source_builds);
        CHECK(p.source_build_allowlist == std::set<std::string>{"git", "pkgconf"});

        // write_settings stores the allowlist as an array; it reads back.
        write_settings(t.path, s);
        CHECK(read_settings(t.path).trust.source_build_allowlist ==
              std::vector<std::string>{"git", "pkgconf"});
    }

    TEST_CASE("a malformed trust.unattended value in the file fails closed") {
        Tmp t("malformed");
        auto s = read_settings(t.path);
        s.trust.unattended = "sometimes";
        write_settings(t.path, s);
        auto p = make_trust_policy(read_settings(t.path).trust, std::string("daemon"));
        CHECK(p.mode == UnattendedMode::Refuse);
    }
}

TEST_SUITE("trust_unattended::ledger") {

    TEST_CASE("refused entries are counted, accepted once, and never overwrite an accept") {
        Tmp t("ledger");
        Config cfg = tmp_config(t.path);
        CHECK(format_trust_ledger(summarize_degraded_installs(cfg)) == "0 refused, 0 degraded");
        CHECK_FALSE(accept_refused_install(cfg, "jq", "1.8.2"));

        record_refused_install(cfg, refused("jq", "1.8.2"));
        record_refused_install(cfg, refused("oniguruma", "6.9.10", "depends on jq: x"));
        auto s = summarize_degraded_installs(cfg);
        CHECK(s.total == 0); // refused entries are not degraded kegs
        CHECK(s.refused == 2);
        REQUIRE(s.refused_installs.size() == 2);
        CHECK(s.refused_installs[0].name == "jq");
        CHECK(s.refused_installs[0].state == "refused");
        CHECK(s.refused_installs[0].status == "no-replica-entry");
        CHECK(s.refused_installs[0].bottle_sha == std::string(64, 'a'));
        CHECK(s.refused_installs[1].reason == "depends on jq: x");
        CHECK(s.since.has_value());
        CHECK(format_trust_ledger(s).starts_with("2 refused, 0 degraded since "));

        CHECK_FALSE(is_install_accepted(cfg, "jq", "1.8.2"));
        CHECK(accept_refused_install(cfg, "jq", "1.8.2"));
        CHECK(is_install_accepted(cfg, "jq", "1.8.2"));
        CHECK_FALSE(is_install_accepted(cfg, "jq", "1.8.3"));

        // A later unattended pass does not undo the accept.
        record_refused_install(cfg, refused("jq", "1.8.2"));
        CHECK(is_install_accepted(cfg, "jq", "1.8.2"));

        s = summarize_degraded_installs(cfg);
        CHECK(s.refused == 1);
        REQUIRE(s.accepted.size() == 1);
        CHECK(s.accepted[0].name == "jq");

        std::ostringstream review;
        format_trust_review(review, s);
        CHECK(review.str().find("oniguruma@6.9.10  [depends on jq: x") != std::string::npos);
        CHECK(review.str().find("Accepted, waiting to install:\n  jq@1.8.2") != std::string::npos);
    }

    TEST_CASE("clear_refused_install drops refused and accepted entries, not degraded ones") {
        Tmp t("clear");
        Config cfg = tmp_config(t.path);
        record_refused_install(cfg, refused("git", "2.56.0", "unattended-source-build"));
        record_refused_install(cfg, refused("pkgconf", "2.5.1", "unattended-source-build"));
        REQUIRE(accept_refused_install(cfg, "pkgconf", "2.5.1"));
        auto keg = t.path / "prefix" / "Cellar" / "jq" / "1.8.2";
        fs::create_directories(keg);
        record_degraded_install(cfg,
                                DegradedInstall{"jq", "1.8.2", "no-replica-entry", "default", keg});

        clear_refused_install(cfg, "git", "2.56.0");
        clear_refused_install(cfg, "pkgconf", "2.5.1");
        clear_refused_install(cfg, "jq", "1.8.2");
        auto s = summarize_degraded_installs(cfg);
        CHECK(s.refused == 0);
        CHECK(s.accepted.empty());
        CHECK(s.total == 1);
    }

    TEST_CASE("accept on an unreadable ledger throws rather than starting afresh") {
        Tmp t("unreadable");
        Config cfg = tmp_config(t.path);
        write(degraded_installs_path(cfg), "{oops");
        CHECK_THROWS(accept_refused_install(cfg, "jq", "1.8.2"));
        CHECK_FALSE(is_install_accepted(cfg, "jq", "1.8.2"));
    }

    TEST_CASE("parse_formula_at_version splits at the last '@'") {
        auto a = parse_formula_at_version("jq@1.8.2");
        REQUIRE(a);
        CHECK(a->first == "jq");
        CHECK(a->second == "1.8.2");
        auto b = parse_formula_at_version("openssl@3@3.6.3");
        REQUIRE(b);
        CHECK(b->first == "openssl@3");
        CHECK(b->second == "3.6.3");
        CHECK_FALSE(parse_formula_at_version("jq"));
        CHECK_FALSE(parse_formula_at_version("jq@"));
        CHECK_FALSE(parse_formula_at_version("@1.0"));
    }

    TEST_CASE("doctor and status surface refused entries") {
        Tmp t("doctor");
        Config cfg = tmp_config(t.path);
        record_refused_install(cfg, refused("jq", "1.8.2"));

        auto report = build_trust_report(cfg);
        bool found = false;
        for (const auto& f : findings_from_trust_report(report)) {
            if (f.message.find("1 refused, 0 degraded since ") != std::string::npos &&
                f.message.find("jq@1.8.2") != std::string::npos &&
                f.message.find("den trust review") != std::string::npos)
                found = true;
        }
        CHECK(found);

        std::ostringstream out;
        format_trust_status(out, summarize_degraded_installs(cfg),
                            make_trust_policy(read_settings(cfg.den_home).trust, std::nullopt));
        CHECK(out.str().find("Trust: 1 refused, 0 degraded since ") != std::string::npos);
        CHECK(out.str().find("trust.unattended=warn") != std::string::npos);
    }
}

TEST_SUITE("trust_unattended::install") {

    TEST_CASE("unattended + refuse holds the bottle back, records it, pours nothing") {
        Fixture f("holdback");
        set_setting(f.cfg.den_home, "trust.unattended", "refuse");
        UnattendedScope u("test");

        bool held = false;
        try {
            f.install();
        } catch (const TrustHeldBack& e) {
            held = true;
            CHECK(e.name() == "probe");
            CHECK(e.version() == "1.0");
        }
        CHECK(held);
        CHECK_FALSE(fs::exists(f.keg()));
        auto s = summarize_degraded_installs(f.cfg);
        CHECK(s.total == 0);
        REQUIRE(s.refused == 1);
        CHECK(s.refused_installs[0].reason == "unattended-refuse");
        CHECK(s.refused_installs[0].status == "no-replica-entry");

        // `den trust accept probe@1.0` lets this exact version through once.
        REQUIRE(accept_refused_install(f.cfg, "probe", "1.0"));
        f.install();
        CHECK(fs::exists(f.keg() / "bin" / "probe"));
        s = summarize_degraded_installs(f.cfg);
        CHECK(s.refused == 0);
        CHECK(s.accepted.empty());
        CHECK(s.total == 1); // poured on a single source: degraded
    }

    TEST_CASE("unattended + warn pours and records a degraded install") {
        Fixture f("warn");
        UnattendedScope u("test");
        f.install();
        CHECK(fs::exists(f.keg()));
        auto s = summarize_degraded_installs(f.cfg);
        CHECK(s.total == 1);
        CHECK(s.refused == 0);
    }

    TEST_CASE("interactive + refuse still pours with the warning") {
        Fixture f("interactive");
        set_setting(f.cfg.den_home, "trust.unattended", "refuse");
        set_unattended(std::nullopt);
        if (const char* xpc = std::getenv("XPC_SERVICE_NAME");
            xpc && std::string(xpc) == kDaemonLaunchdLabel)
            return; // the test binary itself runs as den's launchd job
        f.install();
        CHECK(fs::exists(f.keg()));
        CHECK(summarize_degraded_installs(f.cfg).total == 1);
    }

    TEST_CASE("evaluate_bottle_trust reports the cross-check without installing") {
        Fixture f("evaluate");
        auto t = evaluate_bottle_trust(f.cfg, f.idx.packages["probe"]);
        REQUIRE(t);
        CHECK(t->decision == TrustDecision::WarnProceed);
        CHECK(t->reason_tag == "no-replica-entry");
        CHECK(t->bottle_tag == "default");
        CHECK_FALSE(fs::exists(f.keg()));

        Package cask = formula("nothing", "1.0"); // no archives
        CHECK_FALSE(evaluate_bottle_trust(f.cfg, cask));
    }
}

TEST_SUITE("trust_unattended::daemon") {

    TEST_CASE("screen_pending_upgrades holds dependents and asks once per package") {
        std::vector<PendingUpgrade> pending{
            {"curl", "8.0", "8.1"}, {"git", "2.55", "2.56"}, {"jq", "1.7", "1.8"}};
        std::map<std::string, int> asked;
        auto to_install = [](const std::string& n) -> std::vector<std::string> {
            if (n == "curl")
                return {"curl", "openssl@3"};
            if (n == "git")
                return {"git", "openssl@3", "pcre2"};
            return {n};
        };
        auto refusal = [&](const std::string& n) -> std::optional<std::string> {
            ++asked[n];
            if (n == "openssl@3")
                return std::string("trust.unattended=refuse, no-replica-entry");
            return std::nullopt;
        };
        auto held = screen_pending_upgrades(pending, to_install, refusal);
        REQUIRE(held.size() == 2);
        CHECK(held["curl"] == "depends on openssl@3: trust.unattended=refuse, no-replica-entry");
        CHECK(held["git"] == "depends on openssl@3: trust.unattended=refuse, no-replica-entry");
        CHECK(held.count("jq") == 0);
        for (const auto& [n, c] : asked)
            CHECK_MESSAGE(c == 1, n);
    }

    TEST_CASE("a pending formula that is itself refused carries its own reason") {
        std::vector<PendingUpgrade> pending{{"probe", "0.9", "1.0"}};
        auto held = screen_pending_upgrades(
            pending, [](const std::string& n) { return std::vector<std::string>{n}; },
            [](const std::string&) { return std::optional<std::string>("nope"); });
        CHECK(held["probe"] == "nope");
    }

    TEST_CASE("held_back_notification names the count and points to den doctor") {
        CHECK(held_back_notification({"jq"}) ==
              "den held back 1 upgrade (unattended trust policy): jq. See `den doctor`.");
        CHECK(held_back_notification({"a", "b"}).starts_with("den held back 2 upgrades"));
    }

    TEST_CASE("the cycle skips held upgrades, keeps them pending, and notifies once") {
        Tmp t("cycle");
        auto cellar = t.path / "Cellar";
        fs::create_directories(cellar / "jq" / "1.7.1" / "bin");
        fs::create_directories(cellar / "pv" / "1.11.0" / "bin");
        DaemonState state;
        std::vector<std::string> applied;
        std::vector<std::vector<std::string>> notified;
        MaintenanceHooks h;
        h.refresh_index = [] {
            PackageIndex idx;
            idx.packages["jq"] = formula("jq", "1.8.2");
            idx.packages["pv"] = formula("pv", "1.12.0");
            return idx;
        };
        h.installed = [] {
            return std::map<std::string, std::string>{{"jq", "1.7.1"}, {"pv", "1.11.0"}};
        };
        h.apply = [&](const PendingUpgrade& p) { applied.push_back(p.name); };
        h.in_window = [](const std::string&) { return true; };
        h.trust_screen = [](const std::vector<PendingUpgrade>& pending) {
            std::map<std::string, std::string> held;
            for (const auto& p : pending)
                if (p.name == "jq")
                    held[p.name] = "trust.unattended=refuse, no-replica-entry";
            return held;
        };
        h.notify_held = [&](const std::vector<std::string>& names) {
            notified.push_back(names);
            return true;
        };
        MaintenanceSettings s;
        s.auto_upgrade = true;

        auto rep = run_maintenance_cycle(cellar, t.path / "home", s, state, h);
        CHECK(applied == std::vector<std::string>{"pv"});
        CHECK(rep.applied == 1);
        CHECK(rep.trust_held == 1);
        REQUIRE(state.pending.size() == 1);
        CHECK(state.pending[0].name == "jq");
        REQUIRE(state.trust_held.size() == 1);
        CHECK(state.trust_held[0].available == "1.8.2");
        CHECK(state.trust_held[0].reason == "trust.unattended=refuse, no-replica-entry");
        REQUIRE(notified.size() == 1);
        CHECK(notified[0] == std::vector<std::string>{"jq"});
        CHECK(state.last_trust_notification.has_value());

        // Next pass: still held, nothing new, no second notification.
        applied.clear();
        run_maintenance_cycle(cellar, t.path / "home", s, state, h);
        CHECK(applied == std::vector<std::string>{"pv"}); // the fake never records pv
        CHECK(state.trust_held.size() == 1);
        CHECK(notified.size() == 1);

        std::ostringstream out;
        format_daemon_status(out, state);
        CHECK(out.str().find("Held back by the unattended trust policy") != std::string::npos);
        CHECK(out.str().find("jq") != std::string::npos);
    }

    TEST_CASE("a failing trust screen fails closed: nothing is upgraded") {
        Tmp t("screenfail");
        auto cellar = t.path / "Cellar";
        fs::create_directories(cellar / "jq" / "1.7.1" / "bin");
        DaemonState state;
        int applies = 0;
        MaintenanceHooks h;
        h.refresh_index = [] {
            PackageIndex idx;
            idx.packages["jq"] = formula("jq", "1.8.2");
            return idx;
        };
        h.installed = [] { return std::map<std::string, std::string>{{"jq", "1.7.1"}}; };
        h.apply = [&](const PendingUpgrade&) { ++applies; };
        h.in_window = [](const std::string&) { return true; };
        h.trust_screen =
            [](const std::vector<PendingUpgrade>&) -> std::map<std::string, std::string> {
            throw std::runtime_error("ledger unreadable");
        };
        MaintenanceSettings s;
        s.auto_upgrade = true;
        run_maintenance_cycle(cellar, t.path / "home", s, state, h);
        CHECK(applies == 0);
        CHECK(state.pending.size() == 1);
        bool logged = false;
        for (const auto& e : state.errors)
            if (e.find("trust screen: ledger unreadable") != std::string::npos)
                logged = true;
        CHECK(logged);
    }

    TEST_CASE("daemon state round-trips trust_held and last_trust_notification") {
        Tmp t("state");
        DaemonState st;
        st.trust_held.push_back({"jq", "1.7.1", "1.8.2", "depends on oniguruma: x"});
        st.last_trust_notification = 1730000000;
        write_daemon_state(t.path, st);
        auto back = read_daemon_state(t.path);
        REQUIRE(back.trust_held.size() == 1);
        CHECK(back.trust_held[0].name == "jq");
        CHECK(back.trust_held[0].installed == "1.7.1");
        CHECK(back.trust_held[0].available == "1.8.2");
        CHECK(back.trust_held[0].reason == "depends on oniguruma: x");
        CHECK(back.last_trust_notification == std::optional<uint64_t>(1730000000));
    }
}

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// trust.unattended across entry points and config:
//
//   • `den --unattended upgrade` and the daemon's maintenance cycle screen
//     through the same code (screen_unattended_upgrades): on one fixture they
//     hold back the same packages for the same reasons, upgrade the same
//     rest, and leave the same trust ledger.
//   • Every hold is in the ledger with its reason: single-source,
//     hash-mismatch, replica-unreadable, or "depends on <pkg>".
//   • A bad trust.* value fails closed and `den doctor` warns about it.

#include <doctest.h>

#include "cli/install.h"
#include "core/config.h"
#include "core/error.h"
#include "daemon/daemon.h"
#include "daemon/trust_screen.h"
#include "doctor/trust_checks.h"
#include "env/environment.h"
#include "env/manifest.h"
#include "index/index.h"
#include "index/package.h"
#include "provider/homebrew_provider.h"
#include "provider/package_provider.h"
#include "settings/settings.h"
#include "store/link.h"
#include "store/store.h"
#include "trust/degraded_installs.h"
#include "trust/unattended.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct PathsTmp {
    fs::path path;
    explicit PathsTmp(const std::string& tag) {
        path = fs::temp_directory_path() /
               ("den-unattended-paths-" + tag + "-" + std::to_string(::getpid()) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~PathsTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    PathsTmp(const PathsTmp&) = delete;
    PathsTmp& operator=(const PathsTmp&) = delete;
};

struct Unattended {
    explicit Unattended(std::string reason) { set_unattended(std::move(reason)); }
    ~Unattended() { set_unattended(std::nullopt); }
};

void put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

Config paths_config(const fs::path& root) {
    Config c;
    c.den_home = root / "den";
    c.prefix = root / "prefix";
    c.homebrew_prefix = c.prefix;
    c.store = c.homebrew_prefix / "Cellar";
    c.homebrew_cellar = c.store;
    c.homebrew_caskroom = c.homebrew_prefix / "Caskroom";
    c.homebrew_taps = c.homebrew_prefix / "Library" / "Taps";
    c.cache = c.den_home / "cache";
    c.appdir = root / "Applications";
    c.arch = Arch::Arm64;
    fs::create_directories(c.den_home);
    fs::create_directories(c.store);
    return c;
}

void pour_keg(const Config& cfg, const std::string& name, const std::string& version) {
    auto keg = cfg.store / name / version;
    put(keg / "bin" / name, name + " " + version + "\n");
    mark_keg_owned(keg, name, version);
    ensure_prefix_opt_link(cfg.store, name, keg);
}

// Pours like the homebrew provider, without a network or a bottle.
class KegProvider : public PackageProvider {
  public:
    std::vector<std::string> installed_names;
    std::string_view provider_name() const override { return "homebrew"; }
    bool accepts(std::string_view) const override { return true; }
    InstallResult install(const Config& cfg, std::string_view name,
                          std::string_view version) override {
        std::string v(version);
        installed_names.push_back(std::string(name));
        if (!fs::exists(cfg.store / std::string(name) / v))
            pour_keg(cfg, std::string(name), v);
        return {v, {}};
    }
    void uninstall(const Config&, std::string_view) override {}
    std::vector<InstalledPackage> list_installed(const Config& cfg) const override {
        return ::den::list_installed(cfg.store);
    }
    fs::path package_root(const Config& cfg, std::string_view name,
                          std::string_view version) const override {
        return cfg.store / std::string(name) / std::string(version);
    }
    std::vector<fs::path> binary_paths(const Config&, const InstalledPackage& pkg) const override {
        return {pkg.path / "bin"};
    }
};

std::string sha_of(char c) {
    return std::string(64, c);
}

Package bottled(const std::string& name, const std::string& version, char sha,
                std::vector<std::string> deps = {}) {
    Package p;
    p.name = name;
    p.version = version;
    p.dependencies = std::move(deps);
    Package::Archive ar;
    ar.url = "https://bottles.invalid/" + name + ".tar.gz";
    ar.sha256 = sha_of(sha);
    ar.relocation = RelocationType::Portable;
    p.archives["default"] = ar;
    return p;
}

// Installed: probe 0.9, app 1.0, ok 1.0, bad 1.0. The index offers
//   probe 1.0  single source (no replica entry)  → held: single-source
//   app 2.0    verified, but needs probe 1.0      → held: depends on probe
//   ok 2.0     verified                           → upgraded
//   bad 2.0    replica disagrees                  → held: hash-mismatch
// under trust.unattended=refuse.
struct Fleet {
    PathsTmp tmp;
    Config cfg;
    PackageIndex idx;
    KegProvider provider;

    explicit Fleet(const std::string& tag) : tmp(tag), cfg(paths_config(tmp.path)) {
        idx.packages["probe"] = bottled("probe", "1.0", 'a');
        idx.packages["app"] = bottled("app", "2.0", 'b', {"probe"});
        idx.packages["ok"] = bottled("ok", "2.0", 'c');
        idx.packages["bad"] = bottled("bad", "2.0", 'd');
        put(cfg.den_home / "trust" / "known_hashes.json",
            R"({"schema_version": 1, "hashes": {"app--2.0--default": ")" + sha_of('b') +
                R"(", "ok--2.0--default": ")" + sha_of('c') + R"(", "bad--2.0--default": ")" +
                sha_of('e') + R"("}})");
        const std::map<std::string, std::string> before{
            {"probe", "0.9"}, {"app", "1.0"}, {"ok", "1.0"}, {"bad", "1.0"}};
        for (const auto& [n, v] : before)
            pour_keg(cfg, n, v);
        with_manifest(cfg.den_home, "/", [&](Manifest& m) {
            for (const auto& [n, v] : before)
                m.packages["homebrew"][n] = v;
        });
        materialise(cfg.den_home, cfg.store, "/", &idx);
        set_setting(cfg.den_home, "trust.unattended", "refuse");
    }

    std::map<std::string, std::string> manifest() const {
        return resolve(cfg.den_home, active_env_path(cfg.den_home));
    }

    // name@version → "reason | status" for every refused ledger entry.
    std::map<std::string, std::string> ledger() const {
        std::map<std::string, std::string> out;
        for (const auto& d : summarize_degraded_installs(cfg).refused_installs)
            out[d.name + "@" + d.version] = d.reason + " | " + d.status;
        return out;
    }
};

struct Outcome {
    std::map<std::string, std::string> held; // name → reason
    std::map<std::string, std::string> manifest;
    std::map<std::string, std::string> ledger;
    std::vector<std::string> installed;
};

Outcome run_cli(Fleet& f, std::string* out, std::string* error) {
    Unattended u("--unattended");
    std::ostringstream sink;
    auto* old = std::cout.rdbuf(sink.rdbuf());
    try {
        upgrade_packages(f.cfg, f.provider, f.idx, {});
    } catch (const UserError& e) {
        *error = e.what();
    }
    std::cout.rdbuf(old);
    *out = sink.str();
    Outcome o;
    // "  <name> <from> -> <to>: <reason>" lines under "Held back N package(s)".
    std::istringstream lines(*out);
    bool in_held = false;
    for (std::string line; std::getline(lines, line);) {
        if (line.starts_with("Held back ")) {
            in_held = true;
            continue;
        }
        if (in_held && line.starts_with("  ")) {
            auto sp = line.find(' ', 2);
            auto colon = line.find(": ");
            o.held[line.substr(2, sp - 2)] = line.substr(colon + 2);
        } else {
            in_held = false;
        }
    }
    o.manifest = f.manifest();
    o.ledger = f.ledger();
    o.installed = f.provider.installed_names;
    return o;
}

Outcome run_daemon_cycle(Fleet& f, int* notifications) {
    Unattended u("daemon");
    DaemonState state;
    MaintenanceHooks h;
    h.refresh_index = [&f] { return f.idx; };
    h.installed = [&f] { return f.manifest(); };
    h.apply = [&f](const PendingUpgrade& pu) {
        apply_daemon_upgrade(f.cfg, f.provider, f.idx, pu);
    };
    h.in_window = [](const std::string&) { return true; };
    h.trust_screen = live_trust_screen(f.cfg, f.idx); // what make_live_hooks installs
    h.notify_held = [notifications](const std::vector<std::string>&) {
        ++*notifications;
        return true;
    };
    MaintenanceSettings s;
    s.auto_download = false;
    s.auto_upgrade = true;
    run_maintenance_cycle(f.cfg.store, f.cfg.den_home, s, state, h);
    Outcome o;
    for (const auto& t : state.trust_held)
        o.held[t.name] = t.reason;
    o.manifest = f.manifest();
    o.ledger = f.ledger();
    o.installed = f.provider.installed_names;
    return o;
}

} // namespace

TEST_SUITE("trust_unattended::entry_points") {

    TEST_CASE("`den --unattended upgrade` and the daemon cycle give the same result") {
        Fleet cli_fleet("cli");
        Fleet daemon_fleet("daemon");
        std::string out, error;
        auto cli = run_cli(cli_fleet, &out, &error);
        int notifications = 0;
        auto daemon = run_daemon_cycle(daemon_fleet, &notifications);

        // The expected result, so a shared bug cannot pass as agreement.
        REQUIRE(cli.held.size() == 3);
        CHECK(cli.held["probe"] == "single-source (no-replica-entry)");
        CHECK(cli.held["app"] == "depends on probe: single-source (no-replica-entry)");
        CHECK(cli.held["bad"].starts_with("hash-mismatch: hash mismatch"));
        CHECK(cli.manifest == std::map<std::string, std::string>{
                                  {"probe", "0.9"}, {"app", "1.0"}, {"ok", "2.0"}, {"bad", "1.0"}});
        CHECK(cli.installed == std::vector<std::string>{"ok"});
        CHECK(cli.ledger ==
              std::map<std::string, std::string>{
                  {"app@2.0", "depends on probe: single-source (no-replica-entry) | "},
                  {"bad@2.0", "hash-mismatch | found"},
                  {"probe@1.0", "single-source | no-replica-entry"}});

        // Both entry points agree.
        CHECK(cli.held == daemon.held);
        CHECK(cli.manifest == daemon.manifest);
        CHECK(cli.ledger == daemon.ledger);
        CHECK(cli.installed == daemon.installed);

        // Reported once each: one held-back block and a non-zero exit from the
        // CLI after the rest upgraded; one notification from the daemon.
        size_t blocks = 0;
        for (size_t at = out.find("Held back "); at != std::string::npos;
             at = out.find("Held back ", at + 1))
            ++blocks;
        CHECK(blocks == 1);
        CHECK(out.find("Held back 3 package(s) (unattended trust policy):") != std::string::npos);
        CHECK(out.find("1 package(s) upgraded") != std::string::npos);
        CHECK(error ==
              "3 package(s) held back by the unattended trust policy; review with `den trust "
              "review`");
        CHECK(notifications == 1);
    }

    TEST_CASE("an interactive `den upgrade` does not screen") {
        Fleet f("interactive");
        set_unattended(std::nullopt);
        if (unattended_reason())
            return; // running inside den's launchd job
        std::ostringstream sink;
        auto* old = std::cout.rdbuf(sink.rdbuf());
        try {
            upgrade_packages(f.cfg, f.provider, f.idx, {"ok"});
        } catch (...) {
        }
        std::cout.rdbuf(old);
        CHECK(sink.str().find("Held back") == std::string::npos);
        CHECK(f.manifest().at("ok") == "2.0");
        CHECK(f.ledger().empty());
    }
}

TEST_SUITE("trust_unattended::ledger_reasons") {

    TEST_CASE("hold_reason names single-source, mismatch, unreadable and unverifiable") {
        CHECK(hold_reason(TrustDecision::WarnProceed, "no-replica-entry") == "single-source");
        CHECK(hold_reason(TrustDecision::WarnProceed, "replica-inactive") == "single-source");
        CHECK(hold_reason(TrustDecision::WarnProceed, "homebrew-hash-missing") == "single-source");
        CHECK(hold_reason(TrustDecision::Refuse, "found") == "hash-mismatch");
        CHECK(hold_reason(TrustDecision::Refuse, "replica-unreadable") == "replica-unreadable");
        CHECK(hold_reason(TrustDecision::Refuse, "homebrew-hash-missing") == "unverifiable");
    }

    TEST_CASE("only single-source holds and deferred source builds can be accepted") {
        CHECK(hold_is_acceptable("single-source"));
        CHECK(hold_is_acceptable("unattended-source-build"));
        CHECK_FALSE(hold_is_acceptable("hash-mismatch"));
        CHECK_FALSE(hold_is_acceptable("replica-unreadable"));
        CHECK_FALSE(hold_is_acceptable("unverifiable"));
        CHECK_FALSE(hold_is_acceptable("depends on probe: single-source (no-replica-entry)"));
    }

    TEST_CASE("an unreadable snapshot holds every bottle back, in the ledger, in warn mode too") {
        Fleet f("unreadable");
        set_setting(f.cfg.den_home, "trust.unattended", "warn");
        put(f.cfg.den_home / "trust" / "known_hashes.json", "{ not json");
        Unattended u("daemon");
        std::vector<PendingUpgrade> pending{{"ok", "1.0", "2.0"}, {"probe", "0.9", "1.0"}};
        auto held = screen_unattended_upgrades(f.cfg, f.idx, pending, current_trust_policy(f.cfg));
        REQUIRE(held.size() == 2);
        CHECK(held["ok"].starts_with("replica-unreadable: replica snapshot"));
        auto ledger = f.ledger();
        CHECK(ledger["ok@2.0"] == "replica-unreadable | replica-unreadable");
        CHECK(ledger["probe@1.0"] == "replica-unreadable | replica-unreadable");

        CHECK(summarize_degraded_installs(f.cfg).refused == 2);
    }

    TEST_CASE("an unattended install that refuses on a mismatch is in the ledger") {
        Fleet f("install-mismatch");
        Unattended u("--unattended");
        HomebrewProvider hb(&f.idx);
        CHECK_THROWS_AS(hb.install(f.cfg, "bad", ""), UserError);
        CHECK(f.ledger()["bad@2.0"] == "hash-mismatch | found");
    }

    TEST_CASE("an interactive install that refuses leaves the ledger alone") {
        Fleet f("install-interactive");
        set_unattended(std::nullopt);
        if (unattended_reason())
            return;
        HomebrewProvider hb(&f.idx);
        CHECK_THROWS_AS(hb.install(f.cfg, "bad", ""), UserError);
        CHECK(f.ledger().empty());
    }
}

TEST_SUITE("trust_unattended::config") {

    TEST_CASE("a bad trust.unattended fails closed and doctor warns") {
        PathsTmp t("badmode");
        Config cfg = paths_config(t.path);
        put(cfg.den_home / "config.json", R"({"trust": {"unattended": "sometimes"}})");
        auto settings = read_settings(cfg.den_home).trust;
        CHECK(make_trust_policy(settings, std::string("daemon")).mode == UnattendedMode::Refuse);

        auto findings = findings_from_trust_report(build_trust_report(cfg));
        bool warned = false;
        for (const auto& fd : findings)
            if (fd.severity == Severity::Warning &&
                fd.message.find("trust.unattended is 'sometimes'") != std::string::npos &&
                fd.message.find("treat it as refuse") != std::string::npos)
                warned = true;
        CHECK(warned);
    }

    TEST_CASE("a trust.unattended of the wrong type fails closed instead of defaulting to warn") {
        PathsTmp t("badtype");
        Config cfg = paths_config(t.path);
        put(cfg.den_home / "config.json",
            R"({"trust": {"unattended": true, "unattended_source_builds": 1}})");
        auto settings = read_settings(cfg.den_home).trust;
        CHECK(settings.unattended == "true");
        auto policy = make_trust_policy(settings, std::string("daemon"));
        CHECK(policy.mode == UnattendedMode::Refuse);
        CHECK_FALSE(policy.source_builds);
        auto problems = trust_settings_problems(settings);
        REQUIRE(problems.size() == 2);
        CHECK(problems[0].find("trust.unattended is 'true'") != std::string::npos);
        CHECK(problems[1].find("trust.unattended_source_builds is '1'") != std::string::npos);
        CHECK(problems[1].find("treat it as off") != std::string::npos);
    }

    TEST_CASE("good and default trust settings raise no doctor warning") {
        PathsTmp t("goodmode");
        Config cfg = paths_config(t.path);
        CHECK(trust_settings_problems(read_settings(cfg.den_home).trust).empty());
        set_setting(cfg.den_home, "trust.unattended", "refuse");
        set_setting(cfg.den_home, "trust.unattended_source_builds", "allowlist");
        CHECK(trust_settings_problems(read_settings(cfg.den_home).trust).empty());
        for (const auto& fd : findings_from_trust_report(build_trust_report(cfg)))
            CHECK(fd.message.find("trust.unattended") == std::string::npos);
    }
}

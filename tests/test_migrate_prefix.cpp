// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// `den migrate-prefix`: move a legacy den home (one that installs into
// Homebrew's Cellar) into a prefix of its own.
//
// Every end-to-end test here builds a *fake* Homebrew prefix in a temp dir
// and points HOMEBREW_PREFIX / HOMEBREW_CELLAR / HOME at scratch paths: they
// never touch the real /opt/homebrew, LaunchAgents, or den home.

#include <doctest.h>

#include "core/config.h"
#include "core/error.h"
#include "daemon/daemon.h"
#include "download/sha256.h"
#include "env/environment.h"
#include "index/index.h"
#include "migrate/migrate_prefix.h"
#include "settings/settings.h"
#include "store/pins.h"
#include "store/store.h"
#include "supervisor/supervisor.h"

#include <archive.h>
#include <archive_entry.h>
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct MpTmp {
    fs::path path;
    MpTmp() {
        auto tmpl = (fs::temp_directory_path() / "den-mp-XXXXXX").string();
        char* p = ::mkdtemp(tmpl.data());
        REQUIRE(p != nullptr);
        path = fs::canonical(p);
    }
    ~MpTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    MpTmp(const MpTmp&) = delete;
    MpTmp& operator=(const MpTmp&) = delete;
};

struct ScopedEnv {
    std::string name;
    std::optional<std::string> old;
    ScopedEnv(std::string n, std::optional<std::string> value) : name(std::move(n)) {
        if (const char* v = std::getenv(name.c_str())) {
            old = v;
        }
        if (value) {
            ::setenv(name.c_str(), value->c_str(), 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }
    ~ScopedEnv() {
        if (old) {
            ::setenv(name.c_str(), old->c_str(), 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
};

void put(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << content;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::map<std::string, std::string> tree_snapshot(const fs::path& root) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto& p = it->path();
        auto st = fs::symlink_status(p, ec);
        std::string sig = std::to_string(static_cast<int>(st.type()));
        if (fs::is_regular_file(st)) {
            sig += ":" + slurp(p);
        } else if (fs::is_symlink(st)) {
            sig += ":" + fs::read_symlink(p, ec).string();
        }
        auto t = fs::last_write_time(p, ec);
        sig += ":" + std::to_string(static_cast<long long>(t.time_since_epoch().count()));
        out[fs::relative(p, root).string()] = sig;
    }
    return out;
}

struct Result {
    int code;
    std::string out;
};

Result sh(const std::string& cmd) {
    FILE* pipe = ::popen((cmd + " 2>&1").c_str(), "r");
    REQUIRE(pipe != nullptr);
    std::array<char, 4096> buf{};
    std::string out;
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        out += buf.data();
    }
    int st = ::pclose(pipe);
    return {WIFEXITED(st) ? WEXITSTATUS(st) : -1, out};
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

fs::path bottle(const fs::path& dir, const std::string& name, const std::string& ver) {
    const auto path = dir / (name + "-" + ver + ".tar.gz");
    const std::string body =
        "#!/bin/sh\necho \"" + name + " " + ver + " @@HOMEBREW_PREFIX@@/share/" + name + "\"\n";
    struct archive* a = archive_write_new();
    archive_write_add_filter_gzip(a);
    archive_write_set_format_pax_restricted(a);
    REQUIRE(archive_write_open_filename(a, path.c_str()) == ARCHIVE_OK);
    auto add_dir = [&](const std::string& rel) {
        struct archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, rel.c_str());
        archive_entry_set_filetype(e, AE_IFDIR);
        archive_entry_set_perm(e, 0755);
        archive_write_header(a, e);
        archive_entry_free(e);
    };
    add_dir(name);
    add_dir(name + "/" + ver);
    add_dir(name + "/" + ver + "/bin");
    struct archive_entry* e = archive_entry_new();
    const std::string rel = name + "/" + ver + "/bin/" + name;
    archive_entry_set_pathname(e, rel.c_str());
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, 0755);
    archive_entry_set_size(e, static_cast<la_int64_t>(body.size()));
    archive_write_header(a, e);
    archive_write_data(a, body.data(), body.size());
    archive_entry_free(e);
    archive_write_close(a);
    archive_write_free(a);
    return path;
}

struct IndexEntry {
    std::string name;
    std::string version;
    std::vector<std::string> deps;
};

// Offline index + archive cache for several formulae.
void seed_index(const fs::path& den_home, const std::vector<IndexEntry>& entries) {
    MpTmp staging;
    const auto archives = den_home / "cache" / "archives";
    fs::create_directories(archives);
    nlohmann::json pkgs = nlohmann::json::array();
    for (const auto& e : entries) {
        const auto b = bottle(staging.path, e.name, e.version);
        const auto sha = hash_file(b);
        fs::copy_file(b, archives / (sha + ".tar.gz"), fs::copy_options::overwrite_existing);
        pkgs.push_back({{"name", e.name},
                        {"version", e.version},
                        {"description", "migrate-prefix fixture"},
                        {"homepage", ""},
                        {"license", ""},
                        {"artifact_type", "binary"},
                        {"deprecated", false},
                        {"disabled", false},
                        {"dependencies", e.deps},
                        {"build_dependencies", nlohmann::json::array()},
                        {"archives",
                         {{"all",
                           {{"url", "file:///nonexistent/" + e.name + "-" + e.version + ".tar.gz"},
                            {"sha256", sha},
                            {"relocation", "needs_substitution"}}}}}});
    }
    put(den_home / "cache" / "index.json", nlohmann::json{{"packages", pkgs}}.dump(2));
}

const std::string kBrewPlist = R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict><key>Label</key><string>homebrew.mxcl.squid</string></dict></plist>
)";

// A legacy den home sharing a fake Homebrew prefix:
//   aaa 1.0  den-poured (DEN_RECEIPT) in brew's Cellar
//   bbb 1.0  brew receipt
//   ccc 1.0  missing from the Cellar; index has 1.1 (drift), which needs eee
//   ddd 1.0  unreceipted, pinned at 1.0
struct Legacy {
    fs::path root, brew, home, den_home, to;
    std::string env; // shell prefix that runs ./den against the fixture

    explicit Legacy(const fs::path& r) : root(r) {
        brew = root / "fakebrew";
        home = root / "home";
        den_home = root / "den";
        to = den_home / "prefix";
        for (const std::string n : {"aaa", "bbb", "ddd"}) {
            put(brew / "Cellar" / n / "1.0" / "bin" / n, "#!/bin/sh\necho old " + n + "\n");
            fs::create_directories(brew / "bin");
            fs::create_symlink("../Cellar/" + n + "/1.0/bin/" + n, brew / "bin" / n);
        }
        mark_keg_owned(brew / "Cellar" / "aaa" / "1.0", "aaa", "1.0");
        put(brew / "Cellar" / "bbb" / "1.0" / "INSTALL_RECEIPT.json", "{}");
        put(den_home / "manifests" / "ROOT" / "manifest.json",
            R"({"packages":{"homebrew":{"aaa":"1.0","bbb":"1.0","ccc":"1.0","ddd":"1.0"}},)"
            R"("auto_deps":{}})");
        seed_index(den_home, {{"aaa", "1.0", {}},
                              {"bbb", "1.0", {}},
                              {"ccc", "1.1", {"eee"}},
                              {"ddd", "1.0", {}},
                              {"eee", "1.0", {}}});
        write_pins(den_home, {{"ddd", "1.0"}});
        // A den service whose spec names brew's prefix.
        ServiceSpec spec;
        spec.name = "mpsvc";
        spec.argv = {"/bin/sh", "-c", "sleep 300"};
        spec.env = {{"PATH", (brew / "bin").string() + ":/usr/bin:/bin"},
                    {"TOOL", (brew / "opt" / "aaa" / "bin" / "aaa").string()}};
        {
            ScopedEnv h("DEN_HOME", den_home.string());
            ScopedEnv dp("DEN_PREFIX", std::nullopt);
            ScopedEnv bp("HOMEBREW_PREFIX", brew.string());
            ScopedEnv bc("HOMEBREW_CELLAR", (brew / "Cellar").string());
            save_spec(Config::detect(), spec);
        }
        // LaunchAgents: den's daemon plist and a brew service plist.
        put(home / "Library" / "LaunchAgents" / "dev.den.daemon.plist",
            launchd_plist("/usr/local/bin/den", den_home));
        put(home / "Library" / "LaunchAgents" / "homebrew.mxcl.squid.plist", kBrewPlist);
        put(home / ".zshrc", "eval \"$(den init)\"\neval \"$(den init)\"\n");

        env = "env -u DEN_PREFIX -u HOMEBREW_CASKROOM -u DEN_MIGRATE_PREFIX_FAULT "
              "-u DEN_MIGRATE_PREFIX_FREE_BYTES HOME=" +
              home.string() + " DEN_HOME=" + den_home.string() +
              " HOMEBREW_PREFIX=" + brew.string() + " HOMEBREW_CELLAR=" + brew.string() +
              "/Cellar ";
    }
    Result den(const std::string& args, const std::string& extra_env = "") const {
        return sh(env + extra_env + " ./den " + args);
    }
    std::string config_prefix() const {
        auto r = den("config");
        auto pos = r.out.find("prefix:            ");
        if (pos == std::string::npos) {
            return {};
        }
        auto start = pos + std::string("prefix:            ").size();
        auto end = r.out.find_first_of(" \n", start);
        return r.out.substr(start, end - start);
    }
    std::string manifest_version(const std::string& pkg) const {
        std::ifstream f(den_home / "manifests" / "ROOT" / "manifest.json");
        auto j = nlohmann::json::parse(f);
        return j["packages"]["homebrew"].value(pkg, "");
    }
};

std::string journal_phase(const fs::path& den_home) {
    auto j = read_migrate_prefix_journal(den_home);
    return j ? j->phase : "";
}

// Hold the daemon's pid-file lock so is_daemon_running() sees a live daemon.
struct FakeDaemon {
    int fd = -1;
    explicit FakeDaemon(const fs::path& den_home) {
        fs::create_directories(den_home);
        fd = ::open((den_home / "daemon.pid").c_str(), O_RDWR | O_CREAT, 0644);
        REQUIRE(fd >= 0);
        REQUIRE(::flock(fd, LOCK_EX | LOCK_NB) == 0);
    }
    ~FakeDaemon() {
        ::flock(fd, LOCK_UN);
        ::close(fd);
    }
};

} // namespace

TEST_SUITE("migrate_prefix") {

    TEST_CASE("rebase_prefix_paths rewrites whole paths, colon lists and KEY=value") {
        const fs::path from = "/opt/homebrew";
        const fs::path to = "/Users/u/.den/prefix";
        CHECK(rebase_prefix_paths("/opt/homebrew/bin/redis-server", from, to) ==
              "/Users/u/.den/prefix/bin/redis-server");
        CHECK(rebase_prefix_paths("/opt/homebrew", from, to) == "/Users/u/.den/prefix");
        CHECK(rebase_prefix_paths("/opt/homebrew/bin:/usr/bin:/opt/homebrew/sbin", from, to) ==
              "/Users/u/.den/prefix/bin:/usr/bin:/Users/u/.den/prefix/sbin");
        CHECK(rebase_prefix_paths("--config=/opt/homebrew/etc/x.conf", from, to) ==
              "--config=/Users/u/.den/prefix/etc/x.conf");
        CHECK(rebase_prefix_paths("/opt/homebrewish/bin", from, to) == "/opt/homebrewish/bin");
        CHECK(rebase_prefix_paths("--verbose", from, to) == "--verbose");
    }

    TEST_CASE("rebase_service_spec touches argv, env and working_dir only") {
        ServiceSpec s;
        s.name = "redis";
        s.argv = {"/opt/homebrew/opt/redis/bin/redis-server", "/opt/homebrew/etc/redis.conf"};
        s.env = {{"PATH", "/opt/homebrew/bin:/bin"}, {"X", "y"}};
        s.working_dir = "/opt/homebrew/var";
        s.depends_on = {"other"};
        auto r = rebase_service_spec(s, "/opt/homebrew", "/d/prefix");
        CHECK(r.argv[0] == "/d/prefix/opt/redis/bin/redis-server");
        CHECK(r.argv[1] == "/d/prefix/etc/redis.conf");
        CHECK(r.env[0].second == "/d/prefix/bin:/bin");
        CHECK(r.env[1].second == "y");
        CHECK(r.working_dir == "/d/prefix/var");
        CHECK(r.depends_on == s.depends_on);
        CHECK(r.name == "redis");
    }

    TEST_CASE("plist_with_prefix_path prepends <to>/bin once") {
        const auto plist = launchd_plist("/x/den", "/x/.den");
        auto once = plist_with_prefix_path(plist, "/x/.den/prefix");
        REQUIRE(once);
        CHECK(has(*once, "<string>/x/.den/prefix/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:"
                         "/bin</string>"));
        auto twice = plist_with_prefix_path(*once, "/x/.den/prefix");
        REQUIRE(twice);
        CHECK(*twice == *once);
        CHECK_FALSE(plist_with_prefix_path("<plist/>", "/x"));
    }

    TEST_CASE("launchd_plist leads PATH with den's own prefix, not Homebrew's") {
        CHECK(has(launchd_plist("/x/den", "/x/.den", "/x/.den/prefix"),
                  "<string>/x/.den/prefix/bin:/opt/homebrew/bin:"));
        CHECK(has(launchd_plist("/x/den", "/x/.den", "/opt/homebrew"),
                  "<string>/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin</string>"));
        CHECK(has(launchd_plist("/x/den", "/x/.den"),
                  "<string>/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin</string>"));
    }

    TEST_CASE("bottle_unpacked_size sums regular file sizes") {
        MpTmp tmp;
        const auto b = bottle(tmp.path, "zzz", "2.0");
        const std::string body = "#!/bin/sh\necho \"zzz 2.0 @@HOMEBREW_PREFIX@@/share/zzz\"\n";
        CHECK(bottle_unpacked_size(b) == body.size());
    }

    TEST_CASE("free_bytes_at measures the nearest existing ancestor, override wins") {
        MpTmp tmp;
        fs::path at;
        auto free = free_bytes_at(tmp.path / "not" / "yet", &at);
        CHECK(at == tmp.path);
        CHECK(free.has_value());
        ScopedEnv o("DEN_MIGRATE_PREFIX_FREE_BYTES", "1234");
        CHECK(free_bytes_at(tmp.path, nullptr) == 1234u);
    }

    TEST_CASE("journal round-trips and is written atomically") {
        MpTmp tmp;
        MigratePrefixJournal j;
        j.to = "/a/prefix";
        j.from = "/opt/homebrew";
        j.phase = "pour";
        j.done = {"aaa/1.0"};
        j.failed = {"pour: bbb: boom"};
        j.previous_prefix_recorded = true;
        j.manifest_changes = {{"/", "ccc", "1.0", "1.1"}, {"/", "eee", "", "1.0"}};
        write_migrate_prefix_journal(tmp.path, j);
        CHECK_FALSE(fs::exists(migrate_prefix_journal_path(tmp.path).string() + ".tmp"));
        auto r = read_migrate_prefix_journal(tmp.path);
        REQUIRE(r);
        CHECK(r->to == j.to);
        CHECK(r->phase == "pour");
        CHECK(r->done == j.done);
        CHECK(r->failed == j.failed);
        CHECK(r->previous_prefix_recorded);
        CHECK_FALSE(r->previous_prefix);
        REQUIRE(r->manifest_changes.size() == 2);
        CHECK(r->manifest_changes[1].from.empty());
    }

    TEST_CASE("Config::detect ignores a half-poured private prefix until the switch") {
        MpTmp tmp;
        Legacy f(tmp.path);
        ScopedEnv h("DEN_HOME", f.den_home.string());
        ScopedEnv dp("DEN_PREFIX", std::nullopt);
        ScopedEnv bp("HOMEBREW_PREFIX", f.brew.string());
        ScopedEnv bc("HOMEBREW_CELLAR", (f.brew / "Cellar").string());
        fs::create_directories(f.to / "Cellar" / "aaa" / "1.0");
        MigratePrefixJournal j;
        j.to = f.to.string();
        j.from = f.brew.string();
        for (const std::string phase : {"pour", "link", "materialise", "rolled_back"}) {
            j.phase = phase;
            write_migrate_prefix_journal(f.den_home, j);
            CAPTURE(phase);
            CHECK(Config::detect().prefix_source == PrefixSource::LegacyShared);
        }
        j.phase = "done";
        write_migrate_prefix_journal(f.den_home, j);
        CHECK(Config::detect().prefix_source == PrefixSource::DenHome);
    }

    TEST_CASE("plan classifies den, brew, unreceipted, missing, drift, pin and new deps") {
        MpTmp tmp;
        Legacy f(tmp.path);
        ScopedEnv h("DEN_HOME", f.den_home.string());
        ScopedEnv dp("DEN_PREFIX", std::nullopt);
        ScopedEnv bp("HOMEBREW_PREFIX", f.brew.string());
        ScopedEnv bc("HOMEBREW_CELLAR", (f.brew / "Cellar").string());
        const auto cfg = Config::detect();
        REQUIRE(cfg.shares_homebrew_cellar());
        const auto idx = load_index(cfg.cache / "index.json");
        const auto plan = plan_migrate_prefix(cfg, idx, f.to, f.home, true);
        std::map<std::string, KegPlan> by;
        for (const auto& k : plan.kegs) {
            by[k.name] = k;
        }
        REQUIRE(by.size() == 5);
        CHECK(by["aaa"].owner == KegOwner::Den);
        CHECK(by["aaa"].action == KegAction::Reinstall);
        CHECK(by["bbb"].owner == KegOwner::Brew);
        CHECK(by["ccc"].owner == KegOwner::Missing);
        CHECK(by["ccc"].action == KegAction::Drift);
        CHECK(by["ccc"].target == "1.1");
        CHECK(by["ddd"].owner == KegOwner::Unreceipted);
        CHECK(by["ddd"].action == KegAction::Pinned);
        CHECK(by["eee"].action == KegAction::NewDep);
        CHECK(by["aaa"].bytes_source == "old keg");
        CHECK(by["ccc"].bytes_source == "bottle");
        CHECK(plan.total_known);
        uint64_t sum = 0;
        for (const auto& k : plan.kegs) {
            sum += k.bytes.value_or(0);
        }
        CHECK(plan.total_bytes == sum);
        CHECK(plan.brew_plists == std::vector<std::string>{"homebrew.mxcl.squid"});
        REQUIRE(plan.services.size() == 1);
        CHECK(plan.services[0].changes);
        REQUIRE(plan.shell_rc_init_lines.size() == 1);
        CHECK(plan.shell_rc_init_lines[0].second == 2);
    }

    TEST_CASE("plan never downgrades: an index older than the manifest blocks") {
        MpTmp tmp;
        Legacy f(tmp.path);
        put(f.den_home / "manifests" / "ROOT" / "manifest.json",
            R"({"packages":{"homebrew":{"aaa":"1.0","bbb":"2.0"}},"auto_deps":{}})");
        ScopedEnv h("DEN_HOME", f.den_home.string());
        ScopedEnv dp("DEN_PREFIX", std::nullopt);
        ScopedEnv bp("HOMEBREW_PREFIX", f.brew.string());
        ScopedEnv bc("HOMEBREW_CELLAR", (f.brew / "Cellar").string());
        const auto cfg = Config::detect();
        const auto plan =
            plan_migrate_prefix(cfg, load_index(cfg.cache / "index.json"), f.to, f.home, true);
        bool saw = false;
        for (const auto& k : plan.kegs) {
            if (k.name == "bbb") {
                saw = true;
                CHECK(k.action == KegAction::Blocked);
                CHECK(has(k.reason, "never downgrades"));
            }
        }
        CHECK(saw);
    }

    TEST_CASE("e2e: dry run prints the plan with disk cost and changes nothing") {
        MpTmp tmp;
        Legacy f(tmp.path);
        const auto brew_before = tree_snapshot(f.brew);
        const auto home_before = tree_snapshot(f.home);
        const auto manifests_before = tree_snapshot(f.den_home / "manifests");
        auto r = f.den("migrate-prefix", "DEN_MIGRATE_PREFIX_FREE_BYTES=1000000000");
        MESSAGE("sample dry run:\n" << r.out);
        REQUIRE(r.code == 0);
        const auto to = f.to.string();
        const auto brew = f.brew.string();
        CHECK(has(r.out, "reinstall  aaa 1.0 -> " + to +
                             " (den-poured; old keg eligible for `den cleanup --prefix " + brew +
                             "` later)"));
        CHECK(has(r.out, "reinstall  bbb 1.0 -> " + to + " (brew receipt; brew still owns " + brew +
                             "/Cellar/bbb)"));
        CHECK(has(r.out, "reinstall  ccc 1.0 MISSING -> 1.1 (manifest drift)"));
        CHECK(has(r.out, "pin        ddd 1.0 (pinned, reinstall at pinned version)"));
        CHECK(has(r.out, "reinstall  eee 1.0 -> " + to + " (new dependency"));
        CHECK(has(r.out, "service    mpsvc: spec paths"));
        CHECK(has(r.out, "service    homebrew.mxcl.squid -> brew-owned, untouched"));
        CHECK(has(r.out, "plist      dev.den.daemon PATH -> " + to + "/bin:"));
        CHECK(has(r.out, "`den init` lines (report only)"));
        CHECK(has(r.out, "disk       ccc 1.1: "));
        CHECK(has(r.out, "disk       total "));
        CHECK(has(r.out, "free at " + f.den_home.string() + ": 953.67 MB (1000000000 bytes)"));
        CHECK(has(r.out, "5 kegs, 1 drift, 1 pinned, 0 blocked"));
        CHECK(has(r.out, brew + " writes: 0"));
        CHECK(has(r.out, "Dry run: nothing was changed"));
        CHECK_FALSE(fs::exists(f.to));
        CHECK_FALSE(fs::exists(migrate_prefix_journal_path(f.den_home)));
        CHECK(tree_snapshot(f.brew) == brew_before);
        CHECK(tree_snapshot(f.home) == home_before);
        CHECK(tree_snapshot(f.den_home / "manifests") == manifests_before);
        CHECK(f.config_prefix() == brew);
    }

    TEST_CASE("e2e: --apply refuses when the destination lacks space") {
        MpTmp tmp;
        Legacy f(tmp.path);
        const auto brew_before = tree_snapshot(f.brew);
        auto r = f.den("migrate-prefix --apply", "DEN_MIGRATE_PREFIX_FREE_BYTES=10");
        INFO(r.out);
        CHECK(r.code == 1);
        CHECK(has(r.out, "Refusing: the move writes"));
        CHECK(has(r.out, "only 10 B (10 bytes) is free"));
        CHECK_FALSE(fs::exists(f.to));
        CHECK_FALSE(fs::exists(migrate_prefix_journal_path(f.den_home)));
        CHECK(tree_snapshot(f.brew) == brew_before);
    }

    TEST_CASE("e2e: --apply refuses while the den daemon runs") {
        MpTmp tmp;
        Legacy f(tmp.path);
        FakeDaemon d(f.den_home);
        auto r = f.den("migrate-prefix --apply");
        INFO(r.out);
        CHECK(r.code == 1);
        CHECK(has(r.out, "Refusing: den services are running: dev.den.daemon"));
        CHECK(has(r.out, "--stop-services"));
        CHECK_FALSE(fs::exists(f.to));
    }

    TEST_CASE("e2e: pins fail closed when the index moved past the pin") {
        MpTmp tmp;
        Legacy f(tmp.path);
        write_pins(f.den_home, {{"ddd", "0.9"}});
        auto r = f.den("migrate-prefix --apply");
        INFO(r.out);
        CHECK(r.code == 1);
        CHECK(has(r.out, "blocked    ddd 1.0: pinned at 0.9"));
        CHECK(has(r.out, "Refusing: 1 keg(s) cannot be moved safely"));
        CHECK_FALSE(fs::exists(f.to));
        CHECK(f.config_prefix() == f.brew.string());
    }

    TEST_CASE("e2e: refuses a non-empty target no earlier run created") {
        MpTmp tmp;
        Legacy f(tmp.path);
        put(tmp.path / "elsewhere" / "file", "x");
        auto r = f.den("migrate-prefix --to " + (tmp.path / "elsewhere").string());
        CHECK(r.code == 1);
        CHECK(has(r.out, "already exists and is not empty"));
        auto inside = f.den("migrate-prefix --to " + (f.brew / "den").string());
        CHECK(inside.code == 1);
        CHECK(has(inside.out, "inside Homebrew's prefix"));
    }

    TEST_CASE("e2e: injected failures leave den unchanged; --resume finishes; --rollback "
              "undoes") {
        MpTmp tmp;
        Legacy f(tmp.path);
        const auto brew_before = tree_snapshot(f.brew);
        const auto brew_plist = f.home / "Library" / "LaunchAgents" / "homebrew.mxcl.squid.plist";
        const auto den_plist = f.home / "Library" / "LaunchAgents" / "dev.den.daemon.plist";
        const auto den_plist_before = slurp(den_plist);
        const auto spec_path = f.den_home / "services" / "mpsvc" / "spec.json";
        const auto spec_before = slurp(spec_path);

        // 1. Fail while pouring ccc: aaa/bbb are poured, den still uses brew's prefix.
        auto r1 = f.den("migrate-prefix --apply", "DEN_MIGRATE_PREFIX_FAULT=pour:ccc");
        INFO(r1.out);
        CHECK(r1.code == 1);
        CHECK(has(r1.out, "den is unchanged and still uses " + f.brew.string()));
        CHECK(journal_phase(f.den_home) == "pour");
        {
            auto j = read_migrate_prefix_journal(f.den_home);
            REQUIRE(j);
            REQUIRE_FALSE(j->failed.empty());
            CHECK(has(j->failed.back(), "pour: ccc: injected"));
            CHECK(std::find(j->done.begin(), j->done.end(), "aaa/1.0") != j->done.end());
        }
        CHECK(f.config_prefix() == f.brew.string());
        CHECK(f.manifest_version("ccc") == "1.0");
        CHECK(slurp(spec_path) == spec_before);

        // A plain --apply now refuses: the earlier run must be resumed or rolled back.
        auto again = f.den("migrate-prefix --apply");
        CHECK(again.code == 1);
        CHECK(has(again.out, "--resume"));

        // 2. Resume, failing at materialise: still unchanged.
        auto r2 = f.den("migrate-prefix --resume", "DEN_MIGRATE_PREFIX_FAULT=materialise");
        INFO(r2.out);
        CHECK(r2.code == 1);
        CHECK(has(r2.out, "den is unchanged"));
        CHECK(journal_phase(f.den_home) == "materialise");
        CHECK(f.config_prefix() == f.brew.string());

        // 3. Resume to the end. Resume re-verifies receipts; corrupt one first.
        fs::remove(f.to / "Cellar" / "aaa" / "1.0" / "DEN_RECEIPT.json");
        auto r3 = f.den("migrate-prefix --resume");
        INFO(r3.out);
        REQUIRE(r3.code == 0);
        CHECK(has(r3.out, "poured     aaa 1.0"));
        CHECK(has(r3.out, "Moved: den now uses " + f.to.string()));
        CHECK(journal_phase(f.den_home) == "done");
        CHECK(f.config_prefix() == f.to.string());
        for (const std::string n : {"aaa", "bbb", "ccc", "ddd", "eee"}) {
            CAPTURE(n);
            const auto ver = n == "ccc" ? "1.1" : "1.0";
            const auto keg = f.to / "Cellar" / n / ver;
            CHECK(keg_owned_by_den(keg));
            CHECK(fs::is_symlink(f.to / "opt" / n));
            CHECK(fs::is_symlink(f.to / "bin" / n));
            CHECK(fs::equivalent(f.to / "bin" / n, keg / "bin" / n));
            CHECK(has(slurp(keg / "bin" / n), f.to.string() + "/share/" + n));
        }
        CHECK(fs::equivalent(f.den_home / "envs" / "ROOT" / "bin", f.to / "bin"));
        CHECK(f.manifest_version("ccc") == "1.1");
        CHECK(f.manifest_version("eee") == "1.0");
        CHECK(has(slurp(spec_path), f.to.string() + "/bin:/usr/bin:/bin"));
        CHECK(has(slurp(spec_path), f.to.string() + "/opt/aaa/bin/aaa"));
        CHECK(has(slurp(den_plist), "<string>" + f.to.string() + "/bin:/opt/homebrew/bin:"));
        CHECK(slurp(brew_plist) == kBrewPlist);
        CHECK(tree_snapshot(f.brew) == brew_before);
        CHECK(has(f.den("migrate-prefix").out, "already uses " + f.to.string()));

        // 4. Roll back: brew's prefix again, manifests/spec/plist restored, <to> kept.
        auto rb = f.den("migrate-prefix --rollback");
        INFO(rb.out);
        REQUIRE(rb.code == 0);
        CHECK(has(rb.out, "Rolled back"));
        CHECK(journal_phase(f.den_home) == "rolled_back");
        CHECK(f.config_prefix() == f.brew.string());
        CHECK(f.manifest_version("ccc") == "1.0");
        CHECK(f.manifest_version("eee").empty());
        CHECK(slurp(spec_path) == spec_before);
        CHECK(slurp(den_plist) == den_plist_before);
        CHECK(slurp(brew_plist) == kBrewPlist);
        CHECK(fs::is_directory(f.to / "Cellar" / "ccc" / "1.1"));
        CHECK(fs::read_symlink(f.den_home / "envs" / "ROOT" / "bin") == f.brew / "bin");
        CHECK(tree_snapshot(f.brew) == brew_before);

        // 5. A later --apply reuses the kept prefix without re-pouring.
        auto re = f.den("migrate-prefix --apply");
        INFO(re.out);
        CHECK(re.code == 0);
        CHECK_FALSE(has(re.out, "poured     "));
        CHECK(f.config_prefix() == f.to.string());
        CHECK(tree_snapshot(f.brew) == brew_before);
    }

    TEST_CASE("e2e: running services refuse --apply; --stop-services stops, migrates, "
              "restarts") {
        MpTmp tmp;
        Legacy f(tmp.path);
        auto start = f.den("services start mpsvc");
        INFO(start.out);
        REQUIRE(has(start.out, "Started mpsvc"));
        struct StopAtEnd {
            const Legacy& f;
            ~StopAtEnd() { (void)f.den("services stop mpsvc"); }
        } stop_at_end{f};

        auto refused = f.den("migrate-prefix --apply");
        INFO(refused.out);
        CHECK(refused.code == 1);
        CHECK(has(refused.out, "Refusing: den services are running: mpsvc"));
        CHECK_FALSE(fs::exists(f.to));

        auto moved = f.den("migrate-prefix --apply --stop-services");
        INFO(moved.out);
        REQUIRE(moved.code == 0);
        CHECK(has(moved.out, "stopped    service mpsvc"));
        CHECK(has(moved.out, "rewrote    service mpsvc spec"));
        CHECK(has(moved.out, "started    service mpsvc"));
        CHECK(f.config_prefix() == f.to.string());
        auto status = f.den("services list");
        CHECK(has(status.out, "mpsvc"));
        ScopedEnv h("DEN_HOME", f.den_home.string());
        ScopedEnv dp("DEN_PREFIX", std::nullopt);
        bool running = false;
        for (const auto& s : list_services(Config::detect())) {
            running = running || (s.name == "mpsvc" && s.running);
        }
        CHECK(running);
    }

    TEST_CASE("e2e: each keg pours as its own transaction") {
        MpTmp tmp;
        Legacy f(tmp.path);
        // A failure after ccc's keg is written but before it verifies
        // discards exactly that keg and its opt link; earlier kegs stay done.
        auto r = f.den("migrate-prefix --apply", "DEN_MIGRATE_PREFIX_FAULT=verify:ccc");
        INFO(r.out);
        CHECK(r.code == 1);
        CHECK(has(r.out, "den is unchanged"));
        CHECK_FALSE(fs::exists(f.to / "Cellar" / "ccc"));
        CHECK_FALSE(fs::exists(fs::symlink_status(f.to / "opt" / "ccc")));
        CHECK(fs::is_directory(f.to / "Cellar" / "aaa" / "1.0"));
        {
            auto j = read_migrate_prefix_journal(f.den_home);
            REQUIRE(j);
            CHECK(j->pouring.empty());
            CHECK(std::find(j->done.begin(), j->done.end(), "aaa/1.0") != j->done.end());
            CHECK(std::find(j->done.begin(), j->done.end(), "ccc/1.1") == j->done.end());
        }
        CHECK(f.config_prefix() == f.brew.string());

        // A run killed mid-pour leaves the keg journalled as in flight: even
        // with a receipt it is not trusted, but discarded and poured again.
        {
            auto j = *read_migrate_prefix_journal(f.den_home);
            j.pouring = "bbb/1.0";
            write_migrate_prefix_journal(f.den_home, j);
        }
        auto dry = f.den("migrate-prefix");
        CHECK(has(dry.out, "reinstall  bbb 1.0"));
        CHECK(has(dry.out, "done       aaa 1.0"));
        // A keg whose opt link went missing no longer verifies either.
        fs::remove(f.to / "opt" / "aaa");
        auto res = f.den("migrate-prefix --resume");
        INFO(res.out);
        REQUIRE(res.code == 0);
        CHECK(has(res.out, "poured     bbb 1.0"));
        CHECK(has(res.out, "poured     aaa 1.0"));
        CHECK(has(res.out, "poured     ccc 1.1"));
        CHECK(has(res.out, "poured     eee 1.0"));
        CHECK(journal_phase(f.den_home) == "done");
        CHECK(read_migrate_prefix_journal(f.den_home)->pouring.empty());
        CHECK(f.config_prefix() == f.to.string());
    }

    TEST_CASE("e2e: a keg whose links conflict in the old prefix moves cleanly") {
        MpTmp tmp;
        Legacy f(tmp.path);
        // In the shared prefix, Homebrew owns the links for ssl@3 (they point
        // at its own keg), so den's keg of the same formula cannot be linked
        // there: the "not linking openssl@3: N conflicting path(s)" case.
        const std::string n = "ssl@3";
        const auto brew_keg = f.brew / "Cellar" / n / "0.9";
        put(brew_keg / "bin" / n, "#!/bin/sh\necho brew\n");
        put(brew_keg / "INSTALL_RECEIPT.json", "{}");
        put(f.brew / "Cellar" / n / "1.0" / "bin" / n, "#!/bin/sh\necho den\n");
        mark_keg_owned(f.brew / "Cellar" / n / "1.0", n, "1.0");
        fs::create_symlink("../Cellar/" + n + "/0.9/bin/" + n, f.brew / "bin" / n);
        put(f.den_home / "manifests" / "ROOT" / "manifest.json",
            R"({"packages":{"homebrew":{"aaa":"1.0","ssl@3":"1.0"}},"auto_deps":{}})");
        seed_index(f.den_home, {{"aaa", "1.0", {}}, {n, "1.0", {}}});
        {
            ScopedEnv h("DEN_HOME", f.den_home.string());
            ScopedEnv dp("DEN_PREFIX", std::nullopt);
            ScopedEnv bp("HOMEBREW_PREFIX", f.brew.string());
            ScopedEnv bc("HOMEBREW_CELLAR", (f.brew / "Cellar").string());
            const auto cfg = Config::detect();
            REQUIRE(cfg.shares_homebrew_cellar());
            std::string why;
            try {
                materialise(cfg.den_home, cfg.store, "/");
            } catch (const std::exception& e) {
                why = e.what();
            }
            INFO(why);
            CHECK(has(why, n));
            CHECK(fs::read_symlink(f.brew / "bin" / n) == "../Cellar/" + n + "/0.9/bin/" + n);
        }
        const auto brew_before = tree_snapshot(f.brew);

        auto r = f.den("migrate-prefix --apply");
        INFO(r.out);
        REQUIRE(r.code == 0);
        CHECK_FALSE(has(r.out, "conflicting"));
        CHECK(has(r.out, "Moved: den now uses " + f.to.string()));
        const auto keg = f.to / "Cellar" / n / "1.0";
        REQUIRE(fs::is_symlink(f.to / "bin" / n));
        CHECK(fs::equivalent(f.to / "bin" / n, keg / "bin" / n));
        CHECK(fs::equivalent(f.to / "opt" / n, keg));
        CHECK(fs::read_symlink(f.brew / "bin" / n) == "../Cellar/" + n + "/0.9/bin/" + n);
        CHECK(tree_snapshot(f.brew) == brew_before);
        // Linking again in the new prefix finds nothing in the way.
        auto again = f.den("doctor");
        CHECK_FALSE(has(again.out, "conflicting"));
    }
}

// --stop-services stops and restarts dev.den.daemon through the launchd
// restart helper (bootout gui/<uid>/<label>, bootstrap gui/<uid> <plist>,
// new pid confirmed, one codesigning spawn failure tolerated) rather than a
// shell `launchctl` that inherits the caller's session environment.
namespace {

struct MigrateFakeLaunchctl {
    std::vector<std::string> calls;
    std::vector<int> bootstrap_rcs;  // consumed front to back; default 0
    std::vector<std::string> prints; // consumed; the last one repeats
    std::chrono::steady_clock::time_point t{};

    int count(const std::string& prefix) const {
        int n = 0;
        for (const auto& c : calls)
            if (c.rfind(prefix, 0) == 0)
                ++n;
        return n;
    }

    den::LaunchdOps ops() {
        den::LaunchdOps o;
        o.domain = "gui/501";
        o.label = "dev.den.daemon";
        o.plist = "/Users/x/Library/LaunchAgents/dev.den.daemon.plist";
        o.confirm_timeout = std::chrono::seconds(15);
        o.launchctl = [this](const std::vector<std::string>& args) {
            std::string c;
            for (const auto& a : args)
                c += (c.empty() ? "" : " ") + a;
            calls.push_back(c);
            den::SpawnResult r;
            r.spawned = true;
            r.exit_code = 0;
            if (args[0] == "bootstrap" && !bootstrap_rcs.empty()) {
                r.exit_code = bootstrap_rcs.front();
                bootstrap_rcs.erase(bootstrap_rcs.begin());
                if (r.exit_code)
                    r.output = "Bootstrap failed: 5: Input/output error\n";
            } else if (args[0] == "print") {
                std::string p = prints.empty() ? "" : prints.front();
                if (prints.size() > 1)
                    prints.erase(prints.begin());
                r.output = "gui/501/dev.den.daemon = {\n" + p + "}\n";
            }
            return r;
        };
        o.sleep = [this](std::chrono::milliseconds d) { t += d; };
        o.now = [this] { return t; };
        return o;
    }
};

} // namespace

TEST_SUITE("migrate_prefix stop-services daemon restart") {
    TEST_CASE("stopping the daemon is a launchd bootout of the job") {
        MigrateFakeLaunchctl f;
        den::bootout_migrated_daemon(f.ops());
        REQUIRE(f.calls.size() == 1);
        CHECK(f.calls[0] == "bootout gui/501/dev.den.daemon");
    }

    TEST_CASE("restarting goes through the launchd helper and confirms a pid") {
        MigrateFakeLaunchctl f;
        f.prints = {"\tpid = 4242\n"};
        std::ostringstream out;
        CHECK(den::restart_migrated_daemon(f.ops(), out));
        REQUIRE(f.calls.size() >= 3);
        CHECK(f.calls[0] == "bootout gui/501/dev.den.daemon");
        CHECK(f.calls[1] == "bootstrap gui/501 /Users/x/Library/LaunchAgents/dev.den.daemon.plist");
        CHECK(f.calls[2] == "print gui/501/dev.den.daemon");
        CHECK(f.count("kickstart") == 0);
        CHECK(out.str().find("started    dev.den.daemon") != std::string::npos);
    }

    TEST_CASE("one codesigning spawn failure after the migration is tolerated") {
        MigrateFakeLaunchctl f;
        f.prints = {"\truns = 1\n\tlast exit reason = OS_REASON_CODESIGNING\n",
                    "\truns = 1\n\tlast exit reason = OS_REASON_CODESIGNING\n",
                    "\truns = 2\n\tpid = 300\n"};
        std::ostringstream out;
        CHECK(den::restart_migrated_daemon(f.ops(), out));
        CHECK(f.count("kickstart gui/501/dev.den.daemon") == 1);
        CHECK(out.str().find("started    dev.den.daemon") != std::string::npos);
    }

    TEST_CASE("a daemon that never comes back is reported with the install hint") {
        MigrateFakeLaunchctl f;
        f.bootstrap_rcs = {5, 5, 5};
        std::ostringstream out;
        CHECK_FALSE(den::restart_migrated_daemon(f.ops(), out));
        CHECK(out.str().find("could not restart dev.den.daemon; run `den daemon install`") !=
              std::string::npos);
        CHECK(out.str().find("started    dev.den.daemon") == std::string::npos);
    }

    TEST_CASE("no pid within the confirm window is a failure, not a silent success") {
        MigrateFakeLaunchctl f;
        f.prints = {"\tstate = not running\n"};
        std::ostringstream out;
        CHECK_FALSE(den::restart_migrated_daemon(f.ops(), out));
        CHECK(out.str().find("den daemon install") != std::string::npos);
    }
}

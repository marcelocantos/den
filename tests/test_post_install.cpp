// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Homebrew's declarative post_install steps, symlink subset (G4). openssl@3
// (and openssl@4, gnutls, libressl, ...) declares
//   symlink {{etc}}/ca-certificates/cert.pem -> {{pkgetc}}/cert.pem
// in the formula API's "post_install_steps"; brew creates that link on
// install. den now does the same, natively, without running formula code.

#include <doctest.h>

#include "env/environment.h"
#include "env/manifest.h"
#include "env/post_install.h"
#include "index/index.h"
#include "index/index_cache.h"
#include "store/store.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace den {
namespace test {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const char* kOpensslSteps = R"([{"type":"symlink","force":true,)"
                            R"("source":{"path":"{{etc}}/ca-certificates/cert.pem"},)"
                            R"("target":{"path":"{{pkgetc}}/cert.pem"}}])";

struct PostInstallFx {
    fs::path root;
    fs::path prefix;
    fs::path store;
    fs::path home;

    PostInstallFx() {
        root = fs::temp_directory_path() /
               ("den_post_install_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        prefix = root / "prefix";
        store = prefix / "Cellar";
        home = root / "den";
        fs::create_directories(store);
        fs::create_directories(home / "envs");
    }
    ~PostInstallFx() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    PostInstallContext ctx(const std::string& name = "openssl@3",
                           const std::string& version = "3.6.0") const {
        return {name, prefix, store / name / version};
    }

    static void write(const fs::path& p, const std::string& body) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << body;
    }

    fs::path keg(const std::string& name, const std::string& version) {
        auto k = store / name / version;
        fs::create_directories(k);
        mark_keg_owned(k, name, version);
        return k;
    }

    void want(const std::string& name, const std::string& version) {
        auto m = read_manifest(home, "/");
        m.packages["homebrew"][name] = version;
        write_manifest(home, "/", m);
    }

    fs::path cert_link() const { return prefix / "etc" / "openssl@3" / "cert.pem"; }
    fs::path cert_bundle() const { return prefix / "etc" / "ca-certificates" / "cert.pem"; }
};

bool is_link_to(const fs::path& link, const fs::path& target) {
    std::error_code ec;
    if (!fs::is_symlink(fs::symlink_status(link, ec))) {
        return false;
    }
    return fs::read_symlink(link, ec) == target;
}

std::string one_step(const json& step) {
    return json::array({step}).dump();
}

json symlink_step(const std::string& source, const std::string& target) {
    return {{"type", "symlink"}, {"source", {{"path", source}}}, {"target", {{"path", target}}}};
}

} // namespace

TEST_SUITE("post_install steps") {

    TEST_CASE("placeholders expand against the prefix and keg") {
        PostInstallFx fx;
        auto c = fx.ctx();
        auto k = fx.store / "openssl@3" / "3.6.0";
        fs::path out;
        auto expands_to = [&](const std::string& in, const fs::path& want) {
            CAPTURE(in);
            REQUIRE(expand_post_install_path(in, c, out));
            CHECK(out == want);
        };
        expands_to("{{etc}}/ca-certificates/cert.pem",
                   fx.prefix / "etc" / "ca-certificates" / "cert.pem");
        expands_to("{{pkgetc}}/cert.pem", fx.prefix / "etc" / "openssl@3" / "cert.pem");
        expands_to("{{HOMEBREW_PREFIX}}/var/x", fx.prefix / "var" / "x");
        expands_to("{{HOMEBREW_CELLAR}}", fx.store);
        expands_to("{{prefix}}/bin", k / "bin");
        expands_to("{{opt_prefix}}", fx.prefix / "opt" / "openssl@3");
        expands_to("{{pkgshare}}", k / "share" / "openssl@3");
        expands_to("{{var}}/log/{{name}}.log", fx.prefix / "var" / "log" / "openssl@3.log");
        expands_to("/abs/no/tokens", "/abs/no/tokens");

        std::string unknown;
        out = "untouched";
        CHECK_FALSE(expand_post_install_path("{{HOME}}/x", c, out, &unknown));
        CHECK(unknown == "{{HOME}}");
        CHECK(out == "untouched");
        CHECK_FALSE(expand_post_install_path("{{etc", c, out, &unknown));
    }

    TEST_CASE("openssl's cert.pem step plans one forced link") {
        PostInstallFx fx;
        auto plan = plan_post_install(kOpensslSteps, fx.ctx());
        CHECK(plan.unsupported.empty());
        REQUIRE(plan.links.size() == 1);
        CHECK(plan.links[0].link == fx.cert_link());
        CHECK(plan.links[0].points_to == fx.cert_bundle());
        CHECK(plan.links[0].force);

        CHECK(plan_post_install("", fx.ctx()).empty());
        CHECK(plan_post_install("[]", fx.ctx()).empty());
    }

    TEST_CASE("anything beyond plain symlinks inside the prefix is refused whole") {
        PostInstallFx fx;
        auto refused = [&](const std::string& steps, const std::string& fragment) {
            CAPTURE(steps);
            auto plan = plan_post_install(steps, fx.ctx());
            CHECK(plan.links.empty());
            CHECK(plan.unsupported.find(fragment) != std::string::npos);
        };
        // ca-certificates' own step runs a script: never executed by den.
        refused(R"([{"type":"run","command":{"path":"{{libexec}}/post-install"}}])", "type 'run'");
        // A supported step alongside an unsupported one: nothing is applied.
        refused(json::array({json::parse(kOpensslSteps)[0],
                             {{"type", "mkdir"}, {"path", {{"path", "{{var}}/x"}}}}})
                    .dump(),
                "type 'mkdir'");
        auto with = [](json step, const std::string& key, json value) {
            step[key] = std::move(value);
            return one_step(step);
        };
        auto base = symlink_step("{{etc}}/a", "{{pkgetc}}/a");
        refused(with(base, "glob", true), "option 'glob'");
        refused(with(base, "if", {{"exists", "{{etc}}/a"}}), "option 'if'");
        auto src = base;
        src["source"]["base"] = "prefix";
        refused(one_step(src), "source option 'base'");
        refused(one_step(symlink_step("{{HOME}}/a", "{{pkgetc}}/a")), "placeholder {{HOME}}");
        refused(one_step(symlink_step("etc/a", "{{pkgetc}}/a")), "not an absolute path");
        refused(one_step(symlink_step("/etc/passwd", "{{pkgetc}}/a")), "is outside");
        refused(one_step(symlink_step("{{etc}}/../../x", "{{pkgetc}}/a")), "is outside");
        // Links land only under <prefix>/etc or <prefix>/var, never in a keg,
        // bin/ or outside the prefix.
        refused(one_step(symlink_step("{{etc}}/a", "{{prefix}}/a")), "is outside");
        refused(one_step(symlink_step("{{etc}}/a", "{{HOMEBREW_PREFIX}}/bin/a")), "is outside");
        refused(one_step(symlink_step("{{etc}}/a", "{{etc}}/../bin/a")), "is outside");
        refused(one_step(symlink_step("{{etc}}/a", "{{etc}}")), "is outside");
        refused("{}", "not a list");
        refused("not json", "not a list");
        refused("[1]", "not an object");
        refused(R"([{"type":"symlink","target":{"path":"{{etc}}/a"}}])", "no source path");

        // A var/ link is fine.
        auto plan =
            plan_post_install(one_step(symlink_step("{{etc}}/a", "{{var}}/lib/a")), fx.ctx());
        CHECK(plan.unsupported.empty());
        CHECK(plan.links.size() == 1);
    }

    TEST_CASE("apply creates the link, then leaves it alone") {
        PostInstallFx fx;
        auto plan = plan_post_install(kOpensslSteps, fx.ctx());
        auto r = apply_post_install(plan, fx.prefix, true);
        CHECK(r.created == 1);
        CHECK(r.skipped.empty());
        // Like brew: the link is absolute and may dangle until the bundle exists.
        CHECK(is_link_to(fx.cert_link(), fx.cert_bundle()));

        for (bool replace : {false, true}) {
            auto again = apply_post_install(plan, fx.prefix, replace);
            CHECK(again.created == 0);
            CHECK(again.skipped.empty());
        }
    }

    TEST_CASE("an existing file is replaced only on a fresh link of a force step") {
        PostInstallFx fx;
        PostInstallFx::write(fx.cert_link(), "user bundle\n");
        auto plan = plan_post_install(kOpensslSteps, fx.ctx());

        auto heal = apply_post_install(plan, fx.prefix, /*replace=*/false);
        CHECK(heal.created == 0);
        REQUIRE(heal.skipped.size() == 1);
        CHECK(heal.skipped[0].find("already exists") != std::string::npos);
        CHECK(fs::is_regular_file(fs::symlink_status(fx.cert_link())));

        auto unforced = plan;
        unforced.links[0].force = false;
        CHECK(apply_post_install(unforced, fx.prefix, true).created == 0);
        CHECK(fs::is_regular_file(fs::symlink_status(fx.cert_link())));

        auto fresh = apply_post_install(plan, fx.prefix, /*replace=*/true);
        CHECK(fresh.created == 1);
        CHECK(is_link_to(fx.cert_link(), fx.cert_bundle()));

        // A stale link elsewhere is retargeted the same way.
        fs::remove(fx.cert_link());
        fs::create_symlink("/nowhere", fx.cert_link());
        CHECK(apply_post_install(plan, fx.prefix, false).created == 0);
        CHECK(is_link_to(fx.cert_link(), "/nowhere"));
        CHECK(apply_post_install(plan, fx.prefix, true).created == 1);
        CHECK(is_link_to(fx.cert_link(), fx.cert_bundle()));
    }

    TEST_CASE("a directory in the way, or a symlinked parent, is never touched") {
        PostInstallFx fx;
        auto plan = plan_post_install(kOpensslSteps, fx.ctx());

        fs::create_directories(fx.cert_link() / "inner");
        auto r = apply_post_install(plan, fx.prefix, true);
        CHECK(r.created == 0);
        REQUIRE(r.skipped.size() == 1);
        CHECK(r.skipped[0].find("directory is in the way") != std::string::npos);
        CHECK(fs::is_directory(fx.cert_link() / "inner"));
        fs::remove_all(fx.cert_link().parent_path());

        // A brew-folded etc/openssl@3 -> keg link: writing through it would
        // put den's link inside the keg.
        auto keg_etc = fx.store / "openssl@3" / "3.6.0" / "etc" / "openssl@3";
        fs::create_directories(keg_etc);
        fs::create_symlink(keg_etc, fx.cert_link().parent_path());
        r = apply_post_install(plan, fx.prefix, true);
        CHECK(r.created == 0);
        REQUIRE(r.skipped.size() == 1);
        CHECK(r.skipped[0].find("not a real directory") != std::string::npos);
        CHECK_FALSE(fs::exists(fs::symlink_status(keg_etc / "cert.pem")));
    }

    TEST_CASE("post_install_steps survive the formula API, JSON index and binary cache") {
        PostInstallFx fx;
        json formula = json::array({json{
            {"name", "openssl@3"},
            {"versions", {{"stable", "3.6.0"}}},
            {"keg_only", true},
            {"post_install_steps", json::parse(kOpensslSteps)},
        }});
        auto idx = transform_homebrew(formula.dump(), "[]");
        const auto* p = idx.find("openssl@3");
        REQUIRE(p != nullptr);
        REQUIRE_FALSE(p->post_install_steps.empty());
        CHECK(json::parse(p->post_install_steps) == json::parse(kOpensslSteps));

        auto path = fx.root / "index.json";
        save_index(idx, path);
        auto from_json = load_index_json(path);
        CHECK(from_json.find("openssl@3")->post_install_steps == p->post_install_steps);
        auto cached = load_index(path);
        CHECK(cached.backing != nullptr);
        CHECK(cached.find("openssl@3")->post_install_steps == p->post_install_steps);

        // No steps (or an empty list) stays empty.
        formula[0].erase("post_install_steps");
        CHECK(
            transform_homebrew(formula.dump(), "[]").find("openssl@3")->post_install_steps.empty());
        formula[0]["post_install_steps"] = json::array();
        CHECK(
            transform_homebrew(formula.dump(), "[]").find("openssl@3")->post_install_steps.empty());
    }

    TEST_CASE("materialise links a keg-only openssl's cert.pem into the prefix") {
        PostInstallFx fx;
        fx.keg("openssl@3", "3.6.0");
        fx.want("openssl@3", "3.6.0");
        PackageIndex idx;
        Package p;
        p.name = "openssl@3";
        p.version = "3.6.0";
        p.keg_only = true;
        p.post_install_steps = kOpensslSteps;
        idx.packages[p.name] = p;

        materialise(fx.home, fx.store, "/", &idx);
        CHECK(is_link_to(fx.cert_link(), fx.cert_bundle()));
        CHECK(fs::exists(fx.prefix / "opt" / "openssl@3"));

        // A deleted link heals on the next materialise...
        fs::remove(fx.cert_link());
        materialise(fx.home, fx.store, "/", &idx);
        CHECK(is_link_to(fx.cert_link(), fx.cert_bundle()));

        // ...but a file the user put there is not clobbered without a version change.
        fs::remove(fx.cert_link());
        PostInstallFx::write(fx.cert_link(), "mine\n");
        materialise(fx.home, fx.store, "/", &idx);
        CHECK(fs::is_regular_file(fs::symlink_status(fx.cert_link())));

        // An upgrade is a fresh link: the force step replaces it, as brew would.
        fx.keg("openssl@3", "3.6.1");
        fx.want("openssl@3", "3.6.1");
        idx.packages[p.name].version = "3.6.1";
        materialise(fx.home, fx.store, "/", &idx);
        CHECK(is_link_to(fx.cert_link(), fx.cert_bundle()));
    }

    TEST_CASE("materialise without an index, or with unsupported steps, links nothing extra") {
        PostInstallFx fx;
        fx.keg("openssl@3", "3.6.0");
        fx.want("openssl@3", "3.6.0");
        materialise(fx.home, fx.store, "/");
        CHECK_FALSE(fs::exists(fs::symlink_status(fx.cert_link())));

        PackageIndex idx;
        Package p;
        p.name = "openssl@3";
        p.version = "3.6.0";
        p.post_install_steps =
            json::array({json::parse(kOpensslSteps)[0],
                         {{"type", "run"}, {"command", {{"path", "{{bin}}/x"}}}}})
                .dump();
        idx.packages[p.name] = p;
        materialise(fx.home, fx.store, "/", &idx);
        CHECK_FALSE(fs::exists(fs::symlink_status(fx.cert_link())));
    }
}

} // namespace test
} // namespace den

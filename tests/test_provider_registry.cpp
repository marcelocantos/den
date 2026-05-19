// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Tests for the provider registry and resolve_provider helper (🎯T23.3).
// Independent of any real provider — uses minimal stub implementations
// of PackageProvider to exercise the resolution rules.

#include <doctest.h>

#include "core/error.h"
#include "env/manifest.h"
#include "provider/package_provider.h"
#include "provider/registry.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace den {
namespace test {

namespace {

// Minimal provider stub for resolution tests. Configurable name and
// accepts() behaviour; the install/uninstall/list/binary_paths methods
// are not exercised by resolve_provider and throw if called.
struct StubProvider : public PackageProvider {
    std::string name;
    bool accepts_default = false;
    std::vector<std::string> accepts_names;

    StubProvider(std::string n, bool ad) : name(std::move(n)), accepts_default(ad) {}

    std::string_view provider_name() const override { return name; }

    bool accepts(std::string_view query) const override {
        for (const auto& n : accepts_names) {
            if (n == query) {
                return true;
            }
        }
        return accepts_default;
    }

    InstallResult install(const Config&, std::string_view, std::string_view) override {
        throw std::logic_error("install not exercised in registry tests");
    }
    void uninstall(const Config&, std::string_view) override {
        throw std::logic_error("uninstall not exercised in registry tests");
    }
    std::vector<InstalledPackage> list_installed(const Config&) const override { return {}; }
    fs::path package_root(const Config&, std::string_view, std::string_view) const override {
        return {};
    }
    std::vector<fs::path> binary_paths(const Config&, const InstalledPackage&) const override {
        return {};
    }
};

} // namespace

TEST_SUITE("T23.3::resolve_provider") {

    TEST_CASE("explicit --provider returns the named provider") {
        ProviderRegistry r;
        auto brew = std::make_shared<StubProvider>("homebrew", /*accepts_default*/ true);
        auto pip = std::make_shared<StubProvider>("pip", /*accepts_default*/ false);
        r.add(brew);
        r.add(pip);
        r.set_default("homebrew");

        Manifest m;
        auto& chosen = resolve_provider(r, "requests", m, "pip");
        CHECK(chosen.provider_name() == "pip");
    }

    TEST_CASE("explicit --provider with unknown name throws UserError") {
        ProviderRegistry r;
        r.add(std::make_shared<StubProvider>("homebrew", true));

        Manifest m;
        CHECK_THROWS_AS(resolve_provider(r, "foo", m, "npm"), UserError);
    }

    TEST_CASE("manifest hint picks the provider that already owns the name") {
        ProviderRegistry r;
        auto brew = std::make_shared<StubProvider>("homebrew", true);
        auto pip = std::make_shared<StubProvider>("pip", false);
        r.add(brew);
        r.add(pip);
        r.set_default("homebrew");

        Manifest m;
        m.packages["pip"]["requests"] = "2.31.0";

        auto& chosen = resolve_provider(r, "requests", m, "");
        CHECK(chosen.provider_name() == "pip");
    }

    TEST_CASE("manifest hint dominates over name-pattern accepts()") {
        ProviderRegistry r;
        auto brew = std::make_shared<StubProvider>("homebrew", true);
        auto pip = std::make_shared<StubProvider>("pip", false);
        auto npm = std::make_shared<StubProvider>("npm", false);
        // npm "accepts" the name, but the manifest already places it under pip.
        npm->accepts_names = {"shared-tool"};
        r.add(brew);
        r.add(pip);
        r.add(npm);
        r.set_default("homebrew");

        Manifest m;
        m.packages["pip"]["shared-tool"] = "1.0";

        auto& chosen = resolve_provider(r, "shared-tool", m, "");
        CHECK(chosen.provider_name() == "pip");
    }

    TEST_CASE("name-pattern accepts() picks a non-default provider") {
        ProviderRegistry r;
        auto brew = std::make_shared<StubProvider>("homebrew", true);
        auto pip = std::make_shared<StubProvider>("pip", false);
        pip->accepts_names = {"requests"};
        r.add(brew);
        r.add(pip);
        r.set_default("homebrew");

        Manifest m;
        auto& chosen = resolve_provider(r, "requests", m, "");
        CHECK(chosen.provider_name() == "pip");
    }

    TEST_CASE("default fallback is returned when nothing else claims the name") {
        ProviderRegistry r;
        auto brew = std::make_shared<StubProvider>("homebrew", true);
        auto pip = std::make_shared<StubProvider>("pip", false);
        r.add(brew);
        r.add(pip);
        r.set_default("homebrew");

        Manifest m;
        auto& chosen = resolve_provider(r, "ffmpeg", m, "");
        CHECK(chosen.provider_name() == "homebrew");
    }

    TEST_CASE("manifest entry under an unregistered provider throws UserError") {
        ProviderRegistry r;
        r.add(std::make_shared<StubProvider>("homebrew", true));

        Manifest m;
        m.packages["ghost-provider"]["something"] = "1.0";

        CHECK_THROWS_AS(resolve_provider(r, "something", m, ""), UserError);
    }

    TEST_CASE("first registered provider becomes the implicit default") {
        ProviderRegistry r;
        r.add(std::make_shared<StubProvider>("homebrew", true));
        r.add(std::make_shared<StubProvider>("pip", false));

        CHECK(r.default_provider() != nullptr);
        CHECK(r.default_provider()->provider_name() == "homebrew");
    }

    TEST_CASE("set_default changes which provider is the fallback") {
        // Both providers accept-nothing — so resolution falls through the
        // name-pattern pass and ends at the default. The accept-everything
        // catch-all role is itself owned by whichever provider is the
        // default; non-default catch-alls would be picked up at step 3
        // and never reach the fallback.
        ProviderRegistry r;
        r.add(std::make_shared<StubProvider>("homebrew", false));
        r.add(std::make_shared<StubProvider>("pip", false));
        r.set_default("pip");

        Manifest m;
        auto& chosen = resolve_provider(r, "ffmpeg", m, "");
        CHECK(chosen.provider_name() == "pip");
    }

    TEST_CASE("set_default with unknown provider throws InternalError") {
        ProviderRegistry r;
        r.add(std::make_shared<StubProvider>("homebrew", true));
        CHECK_THROWS_AS(r.set_default("npm"), InternalError);
    }

    TEST_CASE("empty registry: resolve_provider throws InternalError") {
        ProviderRegistry r;
        Manifest m;
        CHECK_THROWS_AS(resolve_provider(r, "anything", m, ""), InternalError);
    }
}

} // namespace test
} // namespace den

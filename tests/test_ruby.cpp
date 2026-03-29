// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>

#ifdef DEN_HAVE_RUBY

#include "../src/ruby/embedded.h"

// Ruby VM can only be initialized once per process. Use a shared instance.
static den::RubyRuntime& shared_ruby() {
    static den::RubyRuntime rt;
    if (!rt.is_initialized()) {
        rt.init();
    }
    return rt;
}

TEST_SUITE("ruby::embedded") {

    TEST_CASE("Ruby VM initializes") {
        auto& rt = shared_ruby();
        CHECK(rt.is_initialized());
    }

    TEST_CASE("register source in virtual FS") {
        auto& rt = shared_ruby();

        // Register a simple Ruby module in the VFS.
        rt.register_source("test_module.rb", R"(
            module TestModule
                VERSION = "1.2.3"
            end
        )");

        // Verify register doesn't crash. The module isn't loaded until required.
    }

    TEST_CASE("evaluate a minimal formula") {
        auto& rt = shared_ruby();
        REQUIRE(rt.is_initialized());

        // Define Formula base class and a concrete formula in one eval.
        // This simulates what happens when Homebrew's library is loaded
        // via the VFS and a formula file is evaluated.
        auto recipe = rt.evaluate_formula(R"(
            class Formula
                def self.desc(s = nil)
                    s ? (@desc = s) : @desc
                end
                def self.homepage(s = nil)
                    s ? (@homepage = s) : @homepage
                end
                def self.license(s = nil)
                    s ? (@license = s) : @license
                end
                def self.url(s = nil, **opts)
                    s ? (@url_str = s) : @url_str
                end
                def self.sha256(s = nil)
                    s ? (@sha256 = s) : @sha256
                end
                def self.depends_on(dep)
                    (@deps ||= []) << dep
                end
                def self.deps
                    @deps || []
                end
                def self.stable
                    self
                end
                def self.keg_only?
                    false
                end
                def self.checksum
                    @sha256 || ""
                end
                attr_reader :name
                def initialize(name)
                    @name = name
                end
                def version
                    "0.0.0"
                end
            end

            class Tree < Formula
                desc "Display directories as trees"
                homepage "https://mama.indstate.edu/users/ice/tree/"
                license "GPL-2.0-or-later"
                url "https://example.com/tree-2.2.1.tgz"
                sha256 "abc123def456"
                depends_on "gcc"
            end
        )", "tree.rb");

        REQUIRE(recipe.has_value());
        CHECK(recipe->name == "tree");
        CHECK(recipe->description == "Display directories as trees");
        CHECK(recipe->homepage == "https://mama.indstate.edu/users/ice/tree/");
        CHECK(recipe->license == "GPL-2.0-or-later");
        CHECK(recipe->dependencies.size() == 1);
        CHECK(recipe->dependencies[0] == "gcc");
    }
}

#endif // DEN_HAVE_RUBY

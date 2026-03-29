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
        // Use a simpler extractor that reads class instance variables directly.
        auto recipe = rt.evaluate_formula(R"(
            class Formula
                class << self
                    attr_accessor :_desc, :_homepage, :_license, :_deps

                    def desc(s = nil)
                        s ? (self._desc = s) : self._desc
                    end
                    def homepage(s = nil)
                        s ? (self._homepage = s) : self._homepage
                    end
                    def license(s = nil)
                        s ? (self._license = s) : self._license
                    end
                    def depends_on(dep)
                        self._deps ||= []
                        self._deps << dep.to_s
                    end
                    def deps
                        self._deps || []
                    end
                    def stable
                        nil
                    end
                    def keg_only?
                        false
                    end
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
                depends_on "gcc"
            end
        )", "tree.rb");

        REQUIRE(recipe.has_value());
        // Name comes empty from embedded Ruby 4.0 (klass.name segfaults).
        // The C++ caller supplies the name from the package index.
        CHECK(recipe->description == "Display directories as trees");
        CHECK(recipe->homepage == "https://mama.indstate.edu/users/ice/tree/");
        CHECK(recipe->license == "GPL-2.0-or-later");
        CHECK(recipe->dependencies.size() == 1);
        if (!recipe->dependencies.empty()) {
            CHECK(recipe->dependencies[0] == "gcc");
        }
    }
}

#endif // DEN_HAVE_RUBY

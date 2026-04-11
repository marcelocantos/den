// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>

#include "build/formula_parser.h"

#include <algorithm>
#include <string>

namespace den {

namespace {

/// Build a synthetic formula around an arbitrary install body so individual
/// DSL constructs can be exercised in isolation.
std::string wrap_install(const std::string& body) {
    return R"(class Fake < Formula
  desc "fake formula"
  homepage "https://example.com"
  url "https://example.com/fake-1.0.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"

  def install
)" + body + R"(
  end
end
)";
}

bool has_marker(const ParsedFormula& p, const std::string& construct) {
    return std::any_of(p.complexity_markers.begin(), p.complexity_markers.end(),
                       [&](const ComplexityMarker& m) { return m.construct == construct; });
}

} // namespace

TEST_SUITE("formula_parser") {

    TEST_CASE("simple formula with std_configure_args is SIMPLE") {
        auto src = wrap_install(R"(    system "./configure", *std_configure_args
    system "make", "install"
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        CHECK(p.complexity == FormulaComplexity::Simple);
        CHECK(p.complexity_markers.empty());
        REQUIRE(p.build_commands.size() == 2);
        CHECK(p.build_commands[0].find("./configure") != std::string::npos);
        CHECK(p.build_commands[0].find("--prefix=/opt/den/fake/1.0") != std::string::npos);
        CHECK(p.build_commands[1].find("make") != std::string::npos);
    }

    TEST_CASE("missing install body is UNSUPPORTED") {
        std::string src = R"(class Fake < Formula
  url "https://example.com/fake-1.0.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
end
)";
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        CHECK(p.complexity == FormulaComplexity::Unsupported);
        CHECK(has_marker(p, "missing-install"));
    }

    TEST_CASE("if guard triggers COMPLEX with an `if` marker") {
        auto src = wrap_install(R"(    if OS.mac?
      system "./configure", *std_configure_args
    end
    system "make", "install"
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        CHECK(p.complexity == FormulaComplexity::Complex);
        CHECK(has_marker(p, "if"));
    }

    TEST_CASE("unless guard triggers COMPLEX") {
        auto src = wrap_install(R"(    unless build.head?
      system "./configure"
    end
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        CHECK(p.complexity == FormulaComplexity::Complex);
        CHECK(has_marker(p, "unless"));
    }

    TEST_CASE("on_macos block triggers COMPLEX") {
        auto src = wrap_install(R"(    on_macos do
      system "./configure", "--with-mac"
    end
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        CHECK(p.complexity == FormulaComplexity::Complex);
        CHECK(has_marker(p, "on_macos"));
    }

    TEST_CASE("on_linux block triggers COMPLEX") {
        auto src = wrap_install(R"(    on_linux do
      system "./configure", "--with-linux"
    end
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        CHECK(p.complexity == FormulaComplexity::Complex);
        CHECK(has_marker(p, "on_linux"));
    }

    TEST_CASE("resource block triggers COMPLEX") {
        auto src = wrap_install(R"(    resource "extra" do
      system "make"
    end
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        CHECK(p.complexity == FormulaComplexity::Complex);
        CHECK(has_marker(p, "resource"));
    }

    TEST_CASE("patch block triggers COMPLEX") {
        auto src = wrap_install(R"(    patch do
      url "https://example.com/fix.patch"
    end
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        CHECK(p.complexity == FormulaComplexity::Complex);
        CHECK(has_marker(p, "patch"));
    }

    TEST_CASE("inreplace triggers COMPLEX") {
        auto src = wrap_install(R"(    inreplace "Makefile", "old", "new"
    system "make"
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        CHECK(p.complexity == FormulaComplexity::Complex);
        CHECK(has_marker(p, "inreplace"));
    }

    TEST_CASE("generic do-block triggers COMPLEX") {
        auto src = wrap_install(R"(    Dir.chdir("subdir") do
      system "make"
    end
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        CHECK(p.complexity == FormulaComplexity::Complex);
        CHECK(has_marker(p, "do"));
    }

    TEST_CASE("unknown statement triggers COMPLEX with unknown-statement marker") {
        // `pkgshare.install ...` is neither system/ENV/mkdir_p nor a known
        // block construct. Previously the parser silently dropped lines
        // like this. Now it must be flagged.
        auto src = wrap_install(R"(    system "./configure"
    pkgshare.install "data"
    system "make", "install"
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        CHECK(p.complexity == FormulaComplexity::Complex);
        CHECK(has_marker(p, "unknown-statement"));
    }

    TEST_CASE("parser collects ALL markers, not just the first") {
        // Flat install body — no nested do-blocks — so this exercises
        // marker collection independently of extract_install_body's
        // nesting tracker. (See follow-up: install-body extractor
        // truncates early on `resource ... do` and other method-with-do
        // forms because only specific leading keywords increment depth.)
        auto src = wrap_install(R"(    if OS.mac?
      system "./configure"
    end
    inreplace "Makefile", "old", "new"
    system "make", "install"
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        CHECK(p.complexity == FormulaComplexity::Complex);
        // Every offender is recorded, so humans reviewing a COMPLEX
        // verdict can see every construct that triggered it.
        CHECK(has_marker(p, "if"));
        CHECK(has_marker(p, "inreplace"));
        // A Simple construct mixed in does not suppress the Complex
        // verdict — Complex is sticky.
        CHECK(p.complexity == FormulaComplexity::Complex);
    }

    TEST_CASE("markers carry a line number and the offending text") {
        auto src = wrap_install(R"(    system "./configure"
    if OS.mac?
      system "make"
    end
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        REQUIRE(!p.complexity_markers.empty());
        const auto& m = p.complexity_markers.front();
        CHECK(m.construct == "if");
        CHECK(m.line > 0);
        CHECK(m.detail.find("if ") != std::string::npos);
    }

    TEST_CASE("bare `iffy` identifier is not mistaken for `if`") {
        // The keyword matcher must require a word boundary, otherwise any
        // identifier starting with `if` would poison the classification.
        auto src = wrap_install(R"(    system "iffy_tool", "--help"
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        // `system "iffy_tool"` is handled; no complexity triggered.
        CHECK(p.complexity == FormulaComplexity::Simple);
        CHECK(p.complexity_markers.empty());
    }

    TEST_CASE("to_string renders each verdict") {
        CHECK(std::string(to_string(FormulaComplexity::Simple)) == "simple");
        CHECK(std::string(to_string(FormulaComplexity::Complex)) == "complex");
        CHECK(std::string(to_string(FormulaComplexity::Unsupported)) == "unsupported");
    }
}

} // namespace den

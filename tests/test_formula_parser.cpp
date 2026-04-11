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

    TEST_CASE("trailing-if modifier on system is caught, not silently absorbed") {
        // Real-world bug from the 🎯T18 oracle: the parser used to fold
        // `if build.head?` into the system call's argument list.
        auto src = wrap_install(R"(    system "autoreconf", "--force" if build.head?
    system "make", "install"
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        CHECK(p.complexity == FormulaComplexity::Complex);
        CHECK(has_marker(p, "trailing-conditional"));
    }

    TEST_CASE("trailing-unless modifier is caught") {
        auto src = wrap_install(R"(    system "./configure" unless OS.mac?
    system "make", "install"
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        CHECK(p.complexity == FormulaComplexity::Complex);
        CHECK(has_marker(p, "trailing-conditional"));
    }

    TEST_CASE(
        "`if` inside a string literal doesn't trigger a trailing-conditional false positive") {
        // The word `if` surrounded by spaces appears inside the command
        // string but the full statement has no real trailing conditional.
        auto src = wrap_install(R"(    system "echo", "check if needed"
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        CHECK(p.complexity == FormulaComplexity::Simple);
        CHECK(p.complexity_markers.empty());
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
        auto src = wrap_install(R"(    if OS.mac?
      system "./configure"
    end
    resource "extra" do
      system "make"
    end
    inreplace "Makefile", "old", "new"
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        CHECK(p.complexity == FormulaComplexity::Complex);
        // Every offender is recorded, so humans reviewing a COMPLEX
        // verdict can see every construct that triggered it. This
        // exercises both complexity-marker emission AND the install-body
        // extractor's nesting tracker — if the extractor closed the body
        // early on `resource "..." do`, the `inreplace` line after it
        // would be invisible to the loop.
        CHECK(has_marker(p, "if"));
        CHECK(has_marker(p, "resource"));
        CHECK(has_marker(p, "inreplace"));
    }

    TEST_CASE("extract_install_body tracks nesting for method-with-do forms") {
        // Regression test for 🎯T55: the install-body extractor used to
        // only increment its depth counter for specific leading keywords
        // (`def`/`do`/`if`/`unless`/`case`/`begin`), so a `resource "x"
        // do` or `Dir.chdir("x") do` line did NOT deepen the scope, but
        // the matching `end` DID shallow it — causing the install body
        // to be silently truncated at the nested block's `end`.
        // Statements past that point were lost from the parser loop.
        //
        // `configure_with_special_marker` is a unique string that can
        // only appear if the parser saw the line *after* the nested
        // block. If the extractor truncates early, the marker is
        // missing from build_commands entirely.
        auto src = wrap_install(R"(    Dir.chdir("subdir") do
      system "make"
    end
    system "./configure_with_special_marker"
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        CHECK(p.complexity == FormulaComplexity::Complex);
        CHECK(has_marker(p, "do"));
        // The post-block line must be visible to the parser loop.
        // Before the fix, the install body was truncated at the nested
        // `end` and this line was never seen.
        bool saw_marker_line = false;
        for (const auto& cmd : p.build_commands)
            if (cmd.find("./configure_with_special_marker") != std::string::npos)
                saw_marker_line = true;
        CHECK(saw_marker_line);
    }

    TEST_CASE("extract_install_body tracks nesting for each-with-block-args") {
        // `array.each do |item|` is another method-with-do form that
        // the old extractor mis-scoped. The block args `|item|` must
        // not confuse the depth tracker.
        auto src = wrap_install(R"(    ["a", "b", "c"].each do |f|
      system "touch", f
    end
    system "./post_block_marker"
)");
        auto p = parse_formula(src, "/opt/den/fake/1.0", "fake");
        CHECK(p.complexity == FormulaComplexity::Complex);
        CHECK(has_marker(p, "do"));
        // Without the fix, `./post_block_marker` would never be seen.
        bool saw_marker_line = false;
        for (const auto& cmd : p.build_commands)
            if (cmd.find("./post_block_marker") != std::string::npos)
                saw_marker_line = true;
        CHECK(saw_marker_line);
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

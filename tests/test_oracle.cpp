// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Oracle tests for the native formula parser (🎯T18).
//
// Runs `parse_formula` against real Homebrew formulae checked out via the
// `tests/corpus/homebrew-core` submodule and asserts two things:
//
//   * Formulae listed in `tests/corpus/simple_formulae.txt` classify as
//     SIMPLE and the extracted `source_url` / `source_sha256` match the
//     values in the Ruby source verbatim.
//   * Formulae listed in `tests/corpus/complex_formulae.txt` classify as
//     COMPLEX and every expected complexity marker is present.
//
// If the submodule isn't initialised (e.g. a clone without
// `--recursive`), the tests emit an informational message and pass so
// that `ctest` still succeeds for contributors who don't want the
// ~40MB corpus.
//
// This is the first slice of the oracle. A later PR will add
// field-by-field cross-validation against Ruby's `extract_formula.rb`.

#include <doctest.h>

#include "build/formula_parser.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// The CMake build passes DEN_CORPUS_DIR as a compile flag pointing at the
// homebrew-core submodule checkout. Map it to a typed constant so clangd
// can parse this file without the build flag (kCorpusDir falls back to
// empty, which the tests interpret as "corpus not available, skip").
#ifdef DEN_CORPUS_DIR
static constexpr const char* kCorpusDir = DEN_CORPUS_DIR;
#else
static constexpr const char* kCorpusDir = "";
#endif

namespace den {

namespace {

namespace fs = std::filesystem;

/// Strip leading and trailing ASCII whitespace.
std::string trim(std::string s) {
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

/// Split on a single delimiter. Empty fields are preserved.
std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

/// Read a text file, stripping `#` comments and blank lines. Each
/// returned entry is a trimmed non-empty logical line.
std::vector<std::string> read_baseline(const fs::path& path) {
    std::vector<std::string> out;
    std::ifstream in(path);
    if (!in)
        return out;
    std::string line;
    while (std::getline(in, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos)
            line = line.substr(0, hash);
        auto t = trim(line);
        if (!t.empty())
            out.push_back(t);
    }
    return out;
}

/// Root of the homebrew-core submodule checkout, or empty if not
/// initialised / not configured.
fs::path corpus_root() {
    fs::path root = kCorpusDir;
    if (root.empty())
        return {};
    if (!fs::exists(root / "Formula"))
        return {};
    return root;
}

/// Resolve a formula name (e.g. `openssl@3`) to its Ruby source path
/// inside homebrew-core. Homebrew shards formulae by first letter:
/// `Formula/o/openssl@3.rb`.
fs::path formula_path(const fs::path& root, const std::string& name) {
    if (name.empty())
        return {};
    std::string shard(1, static_cast<char>(std::tolower(name[0])));
    return root / "Formula" / shard / (name + ".rb");
}

/// Read an entire file into a string.
std::string read_file(const fs::path& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Scrape the top-level `url "..."` declaration from a formula's
/// Ruby source. Used as the ground truth for the SIMPLE oracle's
/// extraction check. Naive — matches the first occurrence of
/// `url "..."` and doesn't handle alternative `url :stable, "..."`
/// forms or the `head do` block's own `url`. Good enough for
/// hand-picked simple formulae.
std::string scrape_url(const std::string& source) {
    auto pos = source.find("\n  url \"");
    if (pos == std::string::npos)
        return "";
    pos += 8; // past `\n  url "`
    auto end = source.find('"', pos);
    if (end == std::string::npos)
        return "";
    return source.substr(pos, end - pos);
}

/// Scrape the top-level `sha256 "<hex>"` declaration. Matches only
/// hex-string SHA256s, not the `sha256 cellar: :any, arm64_sonoma:
/// "..."` bottle forms (those sit inside `bottle do` and aren't the
/// source hash).
std::string scrape_sha256(const std::string& source) {
    auto pos = source.find("\n  sha256 \"");
    if (pos == std::string::npos)
        return "";
    pos += 11; // past `\n  sha256 "`
    auto end = source.find('"', pos);
    if (end == std::string::npos)
        return "";
    return source.substr(pos, end - pos);
}

bool has_marker(const ParsedFormula& p, const std::string& construct) {
    return std::any_of(p.complexity_markers.begin(), p.complexity_markers.end(),
                       [&](const ComplexityMarker& m) { return m.construct == construct; });
}

} // namespace

TEST_SUITE("formula_parser_oracle") {

    TEST_CASE("corpus is initialised") {
        auto root = corpus_root();
        if (root.empty()) {
            MESSAGE("homebrew-core submodule not initialised at "
                    << kCorpusDir
                    << " — skipping oracle tests. Run `git submodule update --init "
                       "tests/corpus/homebrew-core` to enable them.");
            return;
        }
        CHECK(fs::exists(root / "Formula"));
    }

    TEST_CASE("simple formulae classify as SIMPLE and extract url/sha256") {
        auto root = corpus_root();
        if (root.empty())
            return;

        auto baseline_path = fs::path(kCorpusDir).parent_path() / "simple_formulae.txt";
        auto names = read_baseline(baseline_path);
        REQUIRE_MESSAGE(!names.empty(),
                        "simple_formulae.txt is empty or missing: " << baseline_path);

        for (const auto& name : names) {
            CAPTURE(name);
            auto path = formula_path(root, name);
            REQUIRE_MESSAGE(fs::exists(path), "formula source missing: " << path);

            auto source = read_file(path);
            auto expected_url = scrape_url(source);
            auto expected_sha = scrape_sha256(source);
            REQUIRE_MESSAGE(!expected_url.empty(), "could not scrape url from " << name);
            REQUIRE_MESSAGE(!expected_sha.empty(), "could not scrape sha256 from " << name);

            auto p = parse_formula(source, "/opt/den/" + name + "/0", name);

            // If the parser regressed to COMPLEX, surface every marker so
            // the diagnosis is self-contained.
            if (p.complexity != FormulaComplexity::Simple) {
                std::ostringstream diag;
                diag << name << " classified as " << to_string(p.complexity) << " — markers:";
                for (const auto& m : p.complexity_markers)
                    diag << "\n  [" << m.construct << "] line " << m.line << ": " << m.detail;
                FAIL_CHECK(diag.str());
                continue;
            }
            CHECK(p.complexity_markers.empty());
            CHECK(p.source_url == expected_url);
            CHECK(p.source_sha256 == expected_sha);
            // Extracting zero build commands would mean the parser found
            // an install body but stripped it down to nothing — either a
            // silent-drop regression or a genuinely-empty install. For
            // the SIMPLE baseline we require at least one build command.
            CHECK(!p.build_commands.empty());
        }
    }

    TEST_CASE("complex formulae classify as COMPLEX with expected markers") {
        auto root = corpus_root();
        if (root.empty())
            return;

        auto baseline_path = fs::path(kCorpusDir).parent_path() / "complex_formulae.txt";
        auto lines = read_baseline(baseline_path);
        REQUIRE_MESSAGE(!lines.empty(),
                        "complex_formulae.txt is empty or missing: " << baseline_path);

        for (const auto& entry : lines) {
            auto parts = split(entry, ':');
            REQUIRE_MESSAGE(parts.size() == 2, "malformed complex baseline entry: " << entry);
            auto name = trim(parts[0]);
            auto expected_markers = split(trim(parts[1]), ',');
            CAPTURE(name);

            auto path = formula_path(root, name);
            REQUIRE_MESSAGE(fs::exists(path), "formula source missing: " << path);

            auto source = read_file(path);
            auto p = parse_formula(source, "/opt/den/" + name + "/0", name);

            CHECK_MESSAGE(p.complexity == FormulaComplexity::Complex,
                          name << " did not classify as COMPLEX");
            for (const auto& m : expected_markers) {
                auto marker = trim(m);
                CHECK_MESSAGE(has_marker(p, marker),
                              name << " missing expected marker: " << marker);
            }
        }
    }

} // TEST_SUITE

} // namespace den

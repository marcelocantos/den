// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Verification harness for 🎯T63: SAT solver handles real-world dependency
// conflicts.
//
// PURPOSE
// -------
// This file is the acceptance gate for the SAT-based dependency solver
// (🎯T29) and its stability-preference layer (🎯T30).  It does NOT
// implement the solver — that is upstream work.  What it does:
//
//   1. Defines a small in-memory scenario model (packages, versions,
//      dependency constraints, conflicts, soft deps) independent of any
//      real Homebrew index or filesystem.
//
//   2. Loads golden scenario data from tests/corpus/dep_scenarios.yaml
//      (via a minimal hand-rolled YAML reader sufficient for the schema used
//      in that file — no external YAML library is vendored yet).
//
//   3. For each scenario, asserts that the solver's output matches the
//      expected resolution defined in the YAML file.
//
//   4. All tests are marked `* doctest::skip()` until 🎯T29 lands and
//      exposes a public solver API at `src/index/sat_solver.h`.  Once that
//      header exists, remove the skip decorator and wire up the real call.
//
// UPSTREAM GAPS
// -------------
//   🎯T29 — SAT-based solver: no implementation exists yet.  The tests
//            exercise the expected API surface and will fail to compile
//            once the skip is removed and the header is absent.
//   🎯T30 — Stability ratings: the solver's preference function must bias
//            toward stable versions and warn when forced onto an unstable
//            candidate.  Scenario 8 ("stability_forced_unstable_warns")
//            captures this requirement.
//
// HOW TO ACTIVATE
// ---------------
//   1. Implement the SAT solver at src/index/sat_solver.h (see API stub
//      below for the expected interface).
//   2. Delete the `* doctest::skip()` decorator from every TEST_CASE.
//   3. Wire up the real solver call in run_scenario() below.
//   4. Build + ctest — all 14 golden scenarios must pass.
//   5. Remove the greedy DFS resolver from src/cli/install.cpp once all
//      scenarios green (🎯T63 acceptance: no fallback path remains).

#include <doctest.h>

#include "index/sat_solver.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace den {
namespace sat_test {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Scenario model
// ---------------------------------------------------------------------------

/// A version-constraint in the form "pkg op ver", e.g. "openssl >= 3.0.0".
struct VersionConstraint {
    std::string package;
    std::string op; // ">=", "<=", "==", "!="
    std::string version;
};

/// Stability level, ordered from most preferred to least.
enum class Stability { Stable, Testing, Developer, Buggy, Insecure };

Stability parse_stability(const std::string& s) {
    if (s == "stable")
        return Stability::Stable;
    if (s == "testing")
        return Stability::Testing;
    if (s == "developer")
        return Stability::Developer;
    if (s == "buggy")
        return Stability::Buggy;
    if (s == "insecure")
        return Stability::Insecure;
    return Stability::Stable; // default
}

/// A single candidate version of a package.
struct CandidateVersion {
    std::string version;
    Stability stability = Stability::Stable;
    std::vector<VersionConstraint> depends_on;
    std::vector<std::string> conflicts;
    std::vector<VersionConstraint> optional_deps;
    std::vector<VersionConstraint> recommended_deps;
};

/// All candidates for one package name.
struct PackageSpec {
    std::string name;
    std::vector<CandidateVersion> candidates;
};

/// Solver result for one scenario.
struct SolverResult {
    bool satisfiable = true;
    /// Chosen version per package (only populated when satisfiable).
    std::map<std::string, std::string> assignment;
    /// Human-readable warnings (e.g. "unstable: libz 1.4.0-rc1 is testing").
    std::vector<std::string> warnings;
};

/// A complete test scenario loaded from dep_scenarios.yaml.
struct Scenario {
    std::string name;
    std::map<std::string, PackageSpec> packages;
    std::vector<VersionConstraint> request;

    // Expected outcomes.
    std::map<std::string, std::string> expected_assignment;
    std::vector<std::string> expect_warn;
    bool expect_unsat = false;
};

// ---------------------------------------------------------------------------
// Adapter: translate this harness's scenario model into the production solver
// types (den::sat) and back.  The harness keeps its own structs so the corpus
// schema stays decoupled from the solver's API; this thin bridge connects the
// two.
// ---------------------------------------------------------------------------

namespace {

den::sat::VersionConstraint to_sat(const VersionConstraint& vc) {
    return {vc.package, vc.op, vc.version};
}

den::sat::Stability to_sat(Stability s) {
    switch (s) {
    case Stability::Stable:
        return den::sat::Stability::Stable;
    case Stability::Testing:
        return den::sat::Stability::Testing;
    case Stability::Developer:
        return den::sat::Stability::Developer;
    case Stability::Buggy:
        return den::sat::Stability::Buggy;
    case Stability::Insecure:
        return den::sat::Stability::Insecure;
    }
    return den::sat::Stability::Stable;
}

den::sat::SolverResult sat_solve(const std::map<std::string, PackageSpec>& packages,
                                 const std::vector<VersionConstraint>& request) {
    std::map<std::string, den::sat::PackageSpec> sat_packages;
    for (const auto& [name, spec] : packages) {
        den::sat::PackageSpec sp;
        sp.name = spec.name;
        for (const auto& cand : spec.candidates) {
            den::sat::CandidateVersion c;
            c.version = cand.version;
            c.stability = to_sat(cand.stability);
            for (const auto& d : cand.depends_on)
                c.depends_on.push_back(to_sat(d));
            c.conflicts = cand.conflicts;
            for (const auto& d : cand.optional_deps)
                c.optional_deps.push_back(to_sat(d));
            for (const auto& d : cand.recommended_deps)
                c.recommended_deps.push_back(to_sat(d));
            sp.candidates.push_back(std::move(c));
        }
        sat_packages[name] = std::move(sp);
    }
    std::vector<den::sat::VersionConstraint> sat_request;
    for (const auto& r : request)
        sat_request.push_back(to_sat(r));
    return den::sat::solve(sat_packages, sat_request);
}

} // namespace

// ---------------------------------------------------------------------------
// Minimal YAML parser (covers dep_scenarios.yaml schema only)
// ---------------------------------------------------------------------------

namespace yaml {

/// Strip leading/trailing ASCII whitespace.
static std::string trim(std::string s) {
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

/// Count leading spaces.
static size_t indent_of(const std::string& line) {
    size_t n = 0;
    while (n < line.size() && line[n] == ' ')
        ++n;
    return n;
}

/// Remove surrounding quotes from a YAML scalar.
static std::string unquote(const std::string& s) {
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
        return s.substr(1, s.size() - 2);
    return s;
}

/// Parse a scalar value (quoted or bare, strip trailing inline comment).
static std::string parse_value(const std::string& raw) {
    std::string s = trim(raw);
    if (!s.empty() && s[0] != '"' && s[0] != '\'') {
        auto h = s.find(" #");
        if (h != std::string::npos)
            s = trim(s.substr(0, h));
    }
    return unquote(s);
}

/// Parse "pkg [op ver]" into a VersionConstraint.
static VersionConstraint parse_constraint(const std::string& raw) {
    VersionConstraint vc;
    std::istringstream ss(raw);
    ss >> vc.package;
    if (!(ss >> vc.op)) {
        vc.op = "";
        vc.version = "";
        return vc;
    }
    if (!(ss >> vc.version)) {
        vc.op = "";
        vc.version = "";
    }
    return vc;
}

// ---------------------------------------------------------------------------
// Index-based line walker for dep_scenarios.yaml.
//
// Uses an index `i` that advances through the pre-loaded line array.
// Helper lambdas consume lines at a given minimum indent, making it easy
// to reason about scope without a complex state machine.
// ---------------------------------------------------------------------------

std::vector<Scenario> load_scenarios(const fs::path& yaml_path) {
    std::ifstream f(yaml_path);
    if (!f.is_open())
        throw std::runtime_error("cannot open: " + yaml_path.string());

    // Pre-load and filter: strip blank lines and full-line comments.
    struct Line {
        size_t ind;
        std::string raw;
        std::string text;
    };
    std::vector<Line> lines;
    {
        std::string raw;
        while (std::getline(f, raw)) {
            std::string t = trim(raw);
            if (t.empty() || t[0] == '#')
                continue;
            lines.push_back({indent_of(raw), raw, t});
        }
    }

    size_t i = 0;
    std::vector<Scenario> scenarios;

    // Returns true and advances i if current line has exactly `ind` indent
    // and text starts with `prefix`.
    auto peek_at = [&](size_t ind, const std::string& prefix) -> bool {
        return i < lines.size() && lines[i].ind == ind &&
               lines[i].text.substr(0, prefix.size()) == prefix;
    };

    // Parse "key: value" at given indent; returns {key, value} or nullopt.
    auto parse_kv = [&](size_t ind) -> std::optional<std::pair<std::string, std::string>> {
        if (i >= lines.size() || lines[i].ind != ind)
            return std::nullopt;
        auto colon = lines[i].text.find(':');
        if (colon == std::string::npos)
            return std::nullopt;
        auto key = trim(lines[i].text.substr(0, colon));
        auto val = parse_value(lines[i].text.substr(colon + 1));
        ++i;
        return std::make_pair(key, val);
    };

    // Parse a list of "- value" items at given indent.
    auto parse_str_list = [&](size_t ind) -> std::vector<std::string> {
        std::vector<std::string> result;
        while (i < lines.size() && lines[i].ind == ind && lines[i].text.size() >= 2 &&
               lines[i].text[0] == '-' && lines[i].text[1] == ' ') {
            result.push_back(parse_value(lines[i].text.substr(2)));
            ++i;
        }
        return result;
    };

    // Parse a candidate version block starting at ind (the "- version:" line).
    auto parse_candidate = [&](size_t ind) -> CandidateVersion {
        CandidateVersion cand;
        // Current line is "- version: X" at ind.
        auto rest = trim(lines[i].text.substr(2)); // strip "- "
        if (rest.find("version:") == 0)
            cand.version = parse_value(rest.substr(8));
        ++i;
        // Now parse sub-fields at ind+2.
        size_t sub = ind + 2;
        while (i < lines.size() && lines[i].ind >= sub) {
            if (lines[i].ind != sub) {
                ++i;
                continue;
            }
            auto colon = lines[i].text.find(':');
            if (colon == std::string::npos) {
                ++i;
                continue;
            }
            auto key = trim(lines[i].text.substr(0, colon));
            auto val = parse_value(lines[i].text.substr(colon + 1));
            ++i;
            if (key == "version") {
                cand.version = val;
            } else if (key == "stability") {
                cand.stability = parse_stability(val);
            } else if (key == "depends_on") {
                for (auto& s : parse_str_list(sub + 2))
                    cand.depends_on.push_back(parse_constraint(s));
            } else if (key == "conflicts") {
                for (auto& s : parse_str_list(sub + 2))
                    cand.conflicts.push_back(s);
            } else if (key == "optional") {
                for (auto& s : parse_str_list(sub + 2))
                    cand.optional_deps.push_back(parse_constraint(s));
            } else if (key == "recommended") {
                for (auto& s : parse_str_list(sub + 2))
                    cand.recommended_deps.push_back(parse_constraint(s));
            }
        }
        return cand;
    };

    // Advance past "scenarios:" header.
    while (i < lines.size() && lines[i].text != "scenarios:")
        ++i;
    ++i; // consume "scenarios:"

    // Each scenario starts with "  - name: ..." at indent 2.
    while (i < lines.size()) {
        if (lines[i].ind != 2 || lines[i].text.size() < 2 || lines[i].text[0] != '-' ||
            lines[i].text[1] != ' ') {
            ++i;
            continue;
        }
        Scenario sc;
        // Line is "- name: foo" at indent 2.
        auto rest = trim(lines[i].text.substr(2));
        if (rest.find("name:") == 0)
            sc.name = parse_value(rest.substr(5));
        ++i;

        // Parse scenario fields at indent 4.
        while (i < lines.size() && lines[i].ind >= 4) {
            if (lines[i].ind != 4) {
                ++i;
                continue;
            }
            auto colon = lines[i].text.find(':');
            if (colon == std::string::npos) {
                ++i;
                continue;
            }
            auto key = trim(lines[i].text.substr(0, colon));
            auto val = parse_value(lines[i].text.substr(colon + 1));
            ++i;

            if (key == "name") {
                sc.name = val;
            } else if (key == "packages") {
                // Parse package map at indent 6.
                while (i < lines.size() && lines[i].ind >= 6) {
                    if (lines[i].ind != 6) {
                        ++i;
                        continue;
                    }
                    auto pcolon = lines[i].text.find(':');
                    if (pcolon == std::string::npos) {
                        ++i;
                        continue;
                    }
                    std::string pkg_name = trim(lines[i].text.substr(0, pcolon));
                    sc.packages[pkg_name].name = pkg_name;
                    ++i;
                    // Candidates at indent 8 as "- version: X".
                    while (i < lines.size() && lines[i].ind >= 8) {
                        if (lines[i].ind == 8 && lines[i].text.size() >= 2 &&
                            lines[i].text[0] == '-' && lines[i].text[1] == ' ') {
                            sc.packages[pkg_name].candidates.push_back(parse_candidate(8));
                        } else {
                            ++i;
                        }
                    }
                }
            } else if (key == "request") {
                for (auto& s : parse_str_list(6))
                    sc.request.push_back(parse_constraint(s));
            } else if (key == "expected") {
                while (i < lines.size() && lines[i].ind == 6) {
                    auto kv = parse_kv(6);
                    if (kv)
                        sc.expected_assignment[kv->first] = kv->second;
                }
            } else if (key == "expect_warn") {
                for (auto& s : parse_str_list(6))
                    sc.expect_warn.push_back(s);
            } else if (key == "expect_unsat") {
                sc.expect_unsat = (val == "true");
            }
        }

        scenarios.push_back(std::move(sc));
    }

    return scenarios;
}

} // namespace yaml

// ---------------------------------------------------------------------------
// Locate the corpus YAML file relative to this source file's directory.
// The CMake build passes DEN_CORPUS_DIR (pointing at tests/corpus/homebrew-core);
// we derive the dep_scenarios.yaml path from it.
// ---------------------------------------------------------------------------

#ifdef DEN_CORPUS_DIR
static constexpr const char* kCorpusDir = DEN_CORPUS_DIR;
#else
static constexpr const char* kCorpusDir = "";
#endif

static fs::path scenarios_path() {
    // DEN_CORPUS_DIR = .../tests/corpus/homebrew-core
    // dep_scenarios.yaml lives at .../tests/corpus/dep_scenarios.yaml
    if (std::string(kCorpusDir).empty())
        return {};
    return fs::path(kCorpusDir).parent_path() / "dep_scenarios.yaml";
}

// ---------------------------------------------------------------------------
// Scenario runner — drives the production SAT solver (🎯T29 / 🎯T30).
// ---------------------------------------------------------------------------

static SolverResult run_scenario(const Scenario& s) {
    auto sat = sat_solve(s.packages, s.request);
    SolverResult r;
    r.satisfiable = sat.satisfiable;
    r.assignment = sat.assignment;
    r.warnings = sat.warnings;
    return r;
}

// ---------------------------------------------------------------------------
// Assertion helpers
// ---------------------------------------------------------------------------

static bool warn_contains(const std::vector<std::string>& warnings, const std::string& needle) {
    return std::any_of(warnings.begin(), warnings.end(),
                       [&](const std::string& w) { return w.find(needle) != std::string::npos; });
}

// ---------------------------------------------------------------------------
// Tests — all skipped until 🎯T29 solver exists.
//
// To activate: delete `* doctest::skip()` from each TEST_CASE once the
// solver header is available at src/index/sat_solver.h.
// ---------------------------------------------------------------------------

TEST_SUITE("sat_solver::corpus") {

    // -----------------------------------------------------------------------
    // Meta-test: verify the YAML corpus can be loaded and contains all
    // expected scenario names.  This test is NOT skipped — it exercises the
    // parser alone and must always pass.
    // -----------------------------------------------------------------------
    TEST_CASE("corpus YAML loads without error") {
        auto path = scenarios_path();
        if (path.empty() || !fs::exists(path)) {
            MESSAGE("dep_scenarios.yaml not found at "
                    << path.string() << " — re-run cmake to regenerate DEN_CORPUS_DIR");
            return;
        }
        auto scenarios = yaml::load_scenarios(path);
        CHECK(!scenarios.empty());

        // Verify all 14 golden scenarios are present.
        static const std::vector<std::string> kExpectedNames{
            "trivial_single",
            "linear_dep_chain",
            "multi_version_coinstall",
            "version_conflict_unsat",
            "constraint_resolution",
            "direct_conflict_unsat",
            "stability_prefer_stable",
            "stability_forced_unstable_warns",
            "diamond_shared_dep",
            "optional_dep_included_when_available",
            "recommended_dep_preferred",
            "exact_version_pin",
            "transitive_backtrack",
            "provider_choice",
        };

        std::set<std::string> loaded_names;
        for (const auto& s : scenarios)
            loaded_names.insert(s.name);

        for (const auto& expected : kExpectedNames) {
            CHECK_MESSAGE(loaded_names.count(expected), "missing scenario: " << expected);
        }

        // Spot-check a few structural invariants.
        for (const auto& s : scenarios) {
            CHECK_MESSAGE(!s.name.empty(), "scenario has empty name");
            CHECK_MESSAGE(!s.request.empty(), "scenario '" << s.name << "' has no request");
            // Either an expected assignment or expect_unsat, never neither.
            bool has_expected = !s.expected_assignment.empty();
            bool has_unsat = s.expect_unsat;
            bool has_outcome = has_expected || has_unsat;
            CHECK_MESSAGE(has_outcome,
                          "scenario '" << s.name << "' has neither expected nor expect_unsat");
        }
    }

    // -----------------------------------------------------------------------
    // 1. Trivial: single package, single version.
    // -----------------------------------------------------------------------
    TEST_CASE("trivial_single") {
        auto path = scenarios_path();
        if (path.empty() || !fs::exists(path))
            return;
        auto all = yaml::load_scenarios(path);
        auto it = std::find_if(all.begin(), all.end(),
                               [](const Scenario& s) { return s.name == "trivial_single"; });
        REQUIRE(it != all.end());
        auto result = run_scenario(*it);
        REQUIRE(result.satisfiable);
        CHECK(result.assignment.at("curl") == "8.6.0");
    }

    // -----------------------------------------------------------------------
    // 2. Linear dependency chain.
    // -----------------------------------------------------------------------
    TEST_CASE("linear_dep_chain") {
        auto path = scenarios_path();
        if (path.empty() || !fs::exists(path))
            return;
        auto all = yaml::load_scenarios(path);
        auto it = std::find_if(all.begin(), all.end(),
                               [](const Scenario& s) { return s.name == "linear_dep_chain"; });
        REQUIRE(it != all.end());
        auto result = run_scenario(*it);
        REQUIRE(result.satisfiable);
        CHECK(result.assignment.at("curl") == "8.6.0");
        CHECK(result.assignment.at("libssh") == "0.10.6");
        CHECK(result.assignment.at("openssl") == "3.3.0");
    }

    // -----------------------------------------------------------------------
    // 3. Multi-version coinstallation.
    // -----------------------------------------------------------------------
    TEST_CASE("multi_version_coinstall") {
        auto path = scenarios_path();
        if (path.empty() || !fs::exists(path))
            return;
        auto all = yaml::load_scenarios(path);
        auto it = std::find_if(all.begin(), all.end(), [](const Scenario& s) {
            return s.name == "multi_version_coinstall";
        });
        REQUIRE(it != all.end());
        auto result = run_scenario(*it);
        REQUIRE(result.satisfiable);
        CHECK(result.assignment.at("myapp") == "1.0.0");
        CHECK(result.assignment.at("mylib") == "2.0.0");
        CHECK(result.assignment.at("python@3.11") == "3.11.9");
        CHECK(result.assignment.at("python@3.12") == "3.12.3");
    }

    // -----------------------------------------------------------------------
    // 4. Version-constrained conflict → UNSAT.
    // -----------------------------------------------------------------------
    TEST_CASE("version_conflict_unsat") {
        auto path = scenarios_path();
        if (path.empty() || !fs::exists(path))
            return;
        auto all = yaml::load_scenarios(path);
        auto it = std::find_if(all.begin(), all.end(), [](const Scenario& s) {
            return s.name == "version_conflict_unsat";
        });
        REQUIRE(it != all.end());
        auto result = run_scenario(*it);
        CHECK(!result.satisfiable);
    }

    // -----------------------------------------------------------------------
    // 5. Constraint resolved by choosing the right version.
    // -----------------------------------------------------------------------
    TEST_CASE("constraint_resolution") {
        auto path = scenarios_path();
        if (path.empty() || !fs::exists(path))
            return;
        auto all = yaml::load_scenarios(path);
        auto it = std::find_if(all.begin(), all.end(),
                               [](const Scenario& s) { return s.name == "constraint_resolution"; });
        REQUIRE(it != all.end());
        auto result = run_scenario(*it);
        REQUIRE(result.satisfiable);
        CHECK(result.assignment.at("libbar") == "2.0.0");
        CHECK(result.assignment.at("pkgX") == "1.0.0");
        CHECK(result.assignment.at("pkgY") == "1.0.0");
    }

    // -----------------------------------------------------------------------
    // 6. Direct conflicts_with → UNSAT.
    // -----------------------------------------------------------------------
    TEST_CASE("direct_conflict_unsat") {
        auto path = scenarios_path();
        if (path.empty() || !fs::exists(path))
            return;
        auto all = yaml::load_scenarios(path);
        auto it = std::find_if(all.begin(), all.end(),
                               [](const Scenario& s) { return s.name == "direct_conflict_unsat"; });
        REQUIRE(it != all.end());
        auto result = run_scenario(*it);
        CHECK(!result.satisfiable);
    }

    // -----------------------------------------------------------------------
    // 7. Stability bias: solver picks stable over testing.
    // -----------------------------------------------------------------------
    TEST_CASE("stability_prefer_stable") {
        auto path = scenarios_path();
        if (path.empty() || !fs::exists(path))
            return;
        auto all = yaml::load_scenarios(path);
        auto it = std::find_if(all.begin(), all.end(), [](const Scenario& s) {
            return s.name == "stability_prefer_stable";
        });
        REQUIRE(it != all.end());
        auto result = run_scenario(*it);
        REQUIRE(result.satisfiable);
        // Must have chosen the stable candidate, not the rc.
        CHECK(result.assignment.at("libz") == "1.3.1");
        // No unstable warning — solver found a stable solution.
        bool warned = warn_contains(result.warnings, "unstable");
        CHECK(!warned);
    }

    // -----------------------------------------------------------------------
    // 8. Stability forced onto unstable candidate → warning emitted.
    //    (🎯T30 acceptance criterion)
    // -----------------------------------------------------------------------
    TEST_CASE("stability_forced_unstable_warns") {
        auto path = scenarios_path();
        if (path.empty() || !fs::exists(path))
            return;
        auto all = yaml::load_scenarios(path);
        auto it = std::find_if(all.begin(), all.end(), [](const Scenario& s) {
            return s.name == "stability_forced_unstable_warns";
        });
        REQUIRE(it != all.end());
        auto result = run_scenario(*it);
        REQUIRE(result.satisfiable);
        CHECK(result.assignment.at("libz") == "1.4.0-rc1");
        // Must warn about instability (🎯T30 requirement).
        CHECK_MESSAGE(
            warn_contains(result.warnings, "unstable"),
            "solver must emit an 'unstable' warning when forced onto a non-stable candidate");
    }

    // -----------------------------------------------------------------------
    // 9. Diamond dependency resolved to compatible shared version.
    // -----------------------------------------------------------------------
    TEST_CASE("diamond_shared_dep") {
        auto path = scenarios_path();
        if (path.empty() || !fs::exists(path))
            return;
        auto all = yaml::load_scenarios(path);
        auto it = std::find_if(all.begin(), all.end(),
                               [](const Scenario& s) { return s.name == "diamond_shared_dep"; });
        REQUIRE(it != all.end());
        auto result = run_scenario(*it);
        REQUIRE(result.satisfiable);
        CHECK(result.assignment.at("appA") == "1.0.0");
        CHECK(result.assignment.at("libB") == "1.0.0");
        CHECK(result.assignment.at("libC") == "1.0.0");
        // libD must be >= 1.1.0 (libC's lower bound); solver should pick newest.
        CHECK(result.assignment.at("libD") == "1.2.0");
    }

    // -----------------------------------------------------------------------
    // 10. Optional dep pulled in when available in index.
    // -----------------------------------------------------------------------
    TEST_CASE("optional_dep_included_when_available") {
        auto path = scenarios_path();
        if (path.empty() || !fs::exists(path))
            return;
        auto all = yaml::load_scenarios(path);
        auto it = std::find_if(all.begin(), all.end(), [](const Scenario& s) {
            return s.name == "optional_dep_included_when_available";
        });
        REQUIRE(it != all.end());
        auto result = run_scenario(*it);
        REQUIRE(result.satisfiable);
        CHECK(result.assignment.at("imagemagick") == "7.1.1");
        CHECK(result.assignment.at("giflib") == "5.2.2");
    }

    // -----------------------------------------------------------------------
    // 11. Recommended dep preferred over optional.
    // -----------------------------------------------------------------------
    TEST_CASE("recommended_dep_preferred") {
        auto path = scenarios_path();
        if (path.empty() || !fs::exists(path))
            return;
        auto all = yaml::load_scenarios(path);
        auto it = std::find_if(all.begin(), all.end(), [](const Scenario& s) {
            return s.name == "recommended_dep_preferred";
        });
        REQUIRE(it != all.end());
        auto result = run_scenario(*it);
        REQUIRE(result.satisfiable);
        CHECK(result.assignment.at("grep") == "3.11");
        // pcre2 (recommended) must be pulled in.
        CHECK(result.assignment.count("pcre2") == 1);
        CHECK(result.assignment.at("pcre2") == "10.43");
    }

    // -----------------------------------------------------------------------
    // 12. Exact-version pinning honoured.
    // -----------------------------------------------------------------------
    TEST_CASE("exact_version_pin") {
        auto path = scenarios_path();
        if (path.empty() || !fs::exists(path))
            return;
        auto all = yaml::load_scenarios(path);
        auto it = std::find_if(all.begin(), all.end(),
                               [](const Scenario& s) { return s.name == "exact_version_pin"; });
        REQUIRE(it != all.end());
        auto result = run_scenario(*it);
        REQUIRE(result.satisfiable);
        CHECK(result.assignment.at("python@3.12") == "3.12.2");
    }

    // -----------------------------------------------------------------------
    // 13. Transitive conflict requires backtracking to older dep version.
    // -----------------------------------------------------------------------
    TEST_CASE("transitive_backtrack") {
        auto path = scenarios_path();
        if (path.empty() || !fs::exists(path))
            return;
        auto all = yaml::load_scenarios(path);
        auto it = std::find_if(all.begin(), all.end(),
                               [](const Scenario& s) { return s.name == "transitive_backtrack"; });
        REQUIRE(it != all.end());
        auto result = run_scenario(*it);
        REQUIRE(result.satisfiable);
        CHECK(result.assignment.at("pkgM") == "1.0.0");
        CHECK(result.assignment.at("pkgN") == "1.0.0");
        // pkgN requires libssl <= 1.1.1 — solver must backtrack.
        CHECK(result.assignment.at("libssl") == "1.1.1");
    }

    // -----------------------------------------------------------------------
    // 14. Cross-provider: explicit provider wins.
    // -----------------------------------------------------------------------
    TEST_CASE("provider_choice") {
        auto path = scenarios_path();
        if (path.empty() || !fs::exists(path))
            return;
        auto all = yaml::load_scenarios(path);
        auto it = std::find_if(all.begin(), all.end(),
                               [](const Scenario& s) { return s.name == "provider_choice"; });
        REQUIRE(it != all.end());
        auto result = run_scenario(*it);
        REQUIRE(result.satisfiable);
        CHECK(result.assignment.at("pkgA") == "1.0.0");
        CHECK(result.assignment.at("openssl") == "3.3.0");
        // libressl was too old and not requested.
        CHECK(result.assignment.count("libressl") == 0);
    }

    // -----------------------------------------------------------------------
    // Data-driven sweep: for every scenario in the corpus, verify the basic
    // satisfiability and assignment invariants without hard-coding per-field
    // checks.  This catches regressions when new scenarios are added to the
    // YAML without adding matching individual test cases above.
    // -----------------------------------------------------------------------
    TEST_CASE("all_scenarios_data_driven") {
        auto path = scenarios_path();
        if (path.empty() || !fs::exists(path))
            return;
        auto all = yaml::load_scenarios(path);

        for (const auto& s : all) {
            INFO("scenario: " << s.name);
            auto result = run_scenario(s);

            if (s.expect_unsat) {
                CHECK_MESSAGE(!result.satisfiable,
                              "scenario '" << s.name << "' expected UNSAT but got SAT");
                continue;
            }

            CHECK_MESSAGE(result.satisfiable,
                          "scenario '" << s.name << "' expected SAT but got UNSAT");
            if (!result.satisfiable)
                continue;

            // Check each expected package/version pair.
            for (const auto& [pkg, ver] : s.expected_assignment) {
                CHECK_MESSAGE(result.assignment.count(pkg) > 0,
                              "package '" << pkg << "' missing from assignment in '" << s.name
                                          << "'");
                if (result.assignment.count(pkg)) {
                    CHECK_MESSAGE(result.assignment.at(pkg) == ver,
                                  "package '" << pkg << "': expected " << ver << " but got "
                                              << result.assignment.at(pkg) << " in '" << s.name
                                              << "'");
                }
            }

            // Check expected warnings.
            for (const auto& needle : s.expect_warn) {
                CHECK_MESSAGE(warn_contains(result.warnings, needle),
                              "scenario '" << s.name << "' expected warning containing '" << needle
                                           << "' but none found");
            }
        }
    }

} // TEST_SUITE sat_solver::corpus

} // namespace sat_test
} // namespace den

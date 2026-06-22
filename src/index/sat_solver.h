// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// SAT-based dependency solver (🎯T29) with stability-preference biasing
// (🎯T30).
//
// The solver models each (package, version) candidate as a boolean variable
// and finds a satisfying assignment that maximises a preference function:
//
//   stable > testing > developer > buggy > insecure   (stability, 🎯T30)
//   newer version over older                          (tie-break)
//   already-installed version                         (upgrade stability)
//
// The search is a DPLL-style backtracking procedure whose decision heuristic
// visits candidates in preference order, so the first complete assignment it
// finds is the most-preferred one.  It is strictly bounded by a decision
// budget to guarantee termination even on adversarial input.

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace den::sat {

/// A version constraint in the form "pkg op ver", e.g. "openssl >= 3.0.0".
/// An empty `op`/`version` means "any version of `package`".
struct VersionConstraint {
    std::string package;
    std::string op; // ">=", "<=", "==", "!="  (empty == any)
    std::string version;
};

/// Stability level, ordered from most preferred (Stable) to least (Insecure).
enum class Stability { Stable, Testing, Developer, Buggy, Insecure };

/// True when `s` is anything other than Stable (i.e. warrants a warning).
bool is_unstable(Stability s);

/// Human-readable stability label ("stable", "testing", ...).
std::string stability_label(Stability s);

/// Parse a stability label; unknown/empty strings default to Stable.
Stability parse_stability(const std::string& s);

/// A single candidate version of a package.
struct CandidateVersion {
    std::string version;
    Stability stability = Stability::Stable;
    bool installed = false; // already present in the active environment
    std::vector<VersionConstraint> depends_on;
    std::vector<std::string> conflicts;
    std::vector<VersionConstraint> optional_deps;
    std::vector<VersionConstraint> recommended_deps;
};

/// All candidate versions for one package name.
struct PackageSpec {
    std::string name;
    std::vector<CandidateVersion> candidates;
};

/// A line of solver reasoning surfaced by `den deps --explain`.
struct ExplainEntry {
    enum class Kind { Clause, Preference, Conflict, Warning };
    Kind kind = Kind::Clause;
    std::string message;
};

/// Result of solving one request.
struct SolverResult {
    bool satisfiable = true;
    /// Chosen version per package (populated only when satisfiable).
    std::map<std::string, std::string> assignment;
    /// Human-readable warnings (each unstable pick contains "unstable").
    std::vector<std::string> warnings;
    /// Structured reasoning for `den deps --explain`.
    std::vector<ExplainEntry> explanation;
};

/// Compare two version strings.  Returns <0, 0, >0 like strcmp.
/// Numeric components compare numerically; pre-release suffixes (e.g.
/// "-rc1") sort *before* the corresponding release ("1.4.0-rc1" < "1.4.0").
int version_compare(const std::string& a, const std::string& b);

/// True when `version` satisfies `constraint` (op applied to version).
bool constraint_matches(const VersionConstraint& constraint, const std::string& version);

/// Solve a dependency request over the given package universe.
///
/// `packages` maps package name -> its candidate versions.  `request` is the
/// list of top-level install constraints.  The returned SolverResult records
/// the chosen assignment (or satisfiable=false), any stability warnings, and
/// a structured explanation trace.
SolverResult solve(const std::map<std::string, PackageSpec>& packages,
                   const std::vector<VersionConstraint>& request);

} // namespace den::sat

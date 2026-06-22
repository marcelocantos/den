// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "sat_solver.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <sstream>

namespace den::sat {

namespace {

// Hard upper bound on solver decisions.  A complete backtracking search over
// the dependency universe is finite, but adversarial or buggy input could
// blow up combinatorially; this guarantees termination (defensive-coding:
// bounded loops).  Real-world requests resolve in well under a thousand
// decisions; the budget is generous so legitimate requests never hit it.
constexpr std::uint64_t kMaxDecisions = 5'000'000;

/// Split a version into its dotted numeric components, ignoring any
/// pre-release/build suffix introduced by '-' or '+'.  "1.4.0-rc1" -> {1,4,0}.
std::vector<long long> numeric_components(const std::string& version) {
    std::vector<long long> out;
    std::string base = version;
    auto cut = base.find_first_of("-+");
    if (cut != std::string::npos)
        base = base.substr(0, cut);

    std::string token;
    std::istringstream ss(base);
    while (std::getline(ss, token, '.')) {
        long long value = 0;
        bool any_digit = false;
        for (char c : token) {
            if (c >= '0' && c <= '9') {
                value = value * 10 + (c - '0');
                any_digit = true;
            } else {
                break; // stop at first non-digit (e.g. "3a" -> 3)
            }
        }
        out.push_back(any_digit ? value : 0);
    }
    return out;
}

} // namespace

bool is_unstable(Stability s) {
    return s != Stability::Stable;
}

std::string stability_label(Stability s) {
    switch (s) {
    case Stability::Stable:
        return "stable";
    case Stability::Testing:
        return "testing";
    case Stability::Developer:
        return "developer";
    case Stability::Buggy:
        return "buggy";
    case Stability::Insecure:
        return "insecure";
    }
    return "stable";
}

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
    return Stability::Stable;
}

int version_compare(const std::string& a, const std::string& b) {
    auto ca = numeric_components(a);
    auto cb = numeric_components(b);
    size_t n = std::max(ca.size(), cb.size());
    for (size_t i = 0; i < n; ++i) {
        long long va = i < ca.size() ? ca[i] : 0;
        long long vb = i < cb.size() ? cb[i] : 0;
        if (va < vb)
            return -1;
        if (va > vb)
            return 1;
    }
    return 0;
}

bool constraint_matches(const VersionConstraint& constraint, const std::string& version) {
    if (constraint.op.empty() || constraint.version.empty())
        return true; // bare "pkg" — any version satisfies
    int cmp = version_compare(version, constraint.version);
    if (constraint.op == ">=")
        return cmp >= 0;
    if (constraint.op == "<=")
        return cmp <= 0;
    if (constraint.op == "==")
        return cmp == 0;
    if (constraint.op == "!=")
        return cmp != 0;
    // Unknown operator: treat conservatively as "any" rather than rejecting,
    // so a malformed constraint never silently makes everything UNSAT.
    return true;
}

namespace {

// ---------------------------------------------------------------------------
// Backtracking solver.
//
// Each package decision tries its candidates in preference order; the first
// complete, conflict-free extension found is therefore the most preferred.
// ---------------------------------------------------------------------------

/// Preference ordering for a package's candidates: most preferred first.
///   1. lower stability tier (Stable < Testing < ...)
///   2. already installed
///   3. newer version
std::vector<const CandidateVersion*> ordered_candidates(const PackageSpec& spec) {
    std::vector<const CandidateVersion*> out;
    out.reserve(spec.candidates.size());
    for (const auto& c : spec.candidates)
        out.push_back(&c);
    std::stable_sort(out.begin(), out.end(),
                     [](const CandidateVersion* x, const CandidateVersion* y) {
                         if (x->stability != y->stability)
                             return static_cast<int>(x->stability) < static_cast<int>(y->stability);
                         if (x->installed != y->installed)
                             return x->installed;                            // installed first
                         return version_compare(x->version, y->version) > 0; // newer first
                     });
    return out;
}

struct Solver {
    const std::map<std::string, PackageSpec>& packages;
    std::uint64_t decisions = 0;
    bool budget_exhausted = false;

    // Current partial assignment: package -> chosen candidate.
    std::map<std::string, const CandidateVersion*> chosen;

    explicit Solver(const std::map<std::string, PackageSpec>& pkgs) : packages(pkgs) {}

    const PackageSpec* find(const std::string& name) const {
        auto it = packages.find(name);
        return it != packages.end() ? &it->second : nullptr;
    }

    /// Does the current (partial) assignment violate any conflict or
    /// dependency constraint that involves a *fully decided* package?
    /// Returns the offending reason in `reason` for explanation purposes.
    bool consistent(std::string& reason) const {
        // Conflicts are symmetric: if either side declares the other and both
        // are chosen, that's a violation.
        for (const auto& [name_a, cand_a] : chosen) {
            for (const auto& conflict : cand_a->conflicts) {
                auto it = chosen.find(conflict);
                if (it != chosen.end()) {
                    reason = name_a + " conflicts with " + conflict;
                    return false;
                }
            }
        }
        // Dependency constraints: every chosen candidate's hard deps must be
        // satisfiable by some candidate of the depended package, and if that
        // package is already chosen, the chosen version must match.
        for (const auto& [name, cand] : chosen) {
            for (const auto& dep : cand->depends_on) {
                auto it = chosen.find(dep.package);
                if (it != chosen.end()) {
                    if (!constraint_matches(dep, it->second->version)) {
                        reason = name + " requires " + dep.package + " " + dep.op + " " +
                                 dep.version + " but " + dep.package + " " + it->second->version +
                                 " is selected";
                        return false;
                    }
                } else {
                    // Not yet decided — ensure at least one candidate could
                    // satisfy it, else this branch is hopeless.
                    const PackageSpec* spec = find(dep.package);
                    if (!spec) {
                        reason = name + " requires missing package " + dep.package;
                        return false;
                    }
                    bool any = std::any_of(spec->candidates.begin(), spec->candidates.end(),
                                           [&](const CandidateVersion& c) {
                                               return constraint_matches(dep, c.version);
                                           });
                    if (!any) {
                        reason = name + " requires " + dep.package + " " + dep.op + " " +
                                 dep.version + " but no such version exists";
                        return false;
                    }
                }
            }
        }
        return true;
    }

    /// Collect the set of packages that must be decided: the request targets
    /// plus the transitive closure of their hard dependencies and any soft
    /// (optional/recommended) deps that are available in the universe.
    std::set<std::string> required_packages(const std::vector<VersionConstraint>& request) const {
        std::set<std::string> needed;
        std::vector<std::string> stack;
        auto add = [&](const std::string& n) {
            if (find(n) && !needed.count(n)) {
                needed.insert(n);
                stack.push_back(n);
            }
        };
        for (const auto& r : request)
            add(r.package);
        // Bounded by the size of the universe (each package added at most once).
        while (!stack.empty()) {
            std::string name = stack.back();
            stack.pop_back();
            const PackageSpec* spec = find(name);
            if (!spec)
                continue;
            for (const auto& cand : spec->candidates) {
                for (const auto& d : cand.depends_on)
                    add(d.package);
                for (const auto& d : cand.recommended_deps)
                    add(d.package); // recommended: pull in when available
                for (const auto& d : cand.optional_deps)
                    add(d.package); // optional: pull in when available
            }
        }
        return needed;
    }

    /// Recursive assignment over `order[idx..]`.  Returns true once a complete
    /// consistent assignment is found (left in `chosen`).
    bool assign(const std::vector<std::string>& order, size_t idx,
                const std::map<std::string, std::vector<VersionConstraint>>& request_constraints) {
        if (budget_exhausted)
            return false;
        if (idx == order.size())
            return true;

        const std::string& name = order[idx];
        const PackageSpec* spec = find(name);
        if (!spec)
            return false;

        for (const CandidateVersion* cand : ordered_candidates(*spec)) {
            if (++decisions > kMaxDecisions) {
                budget_exhausted = true;
                return false;
            }
            // Honour top-level request constraints on this package.
            auto rc = request_constraints.find(name);
            if (rc != request_constraints.end()) {
                bool ok = std::all_of(rc->second.begin(), rc->second.end(),
                                      [&](const VersionConstraint& c) {
                                          return constraint_matches(c, cand->version);
                                      });
                if (!ok)
                    continue;
            }

            chosen[name] = cand;
            std::string reason;
            if (consistent(reason) && assign(order, idx + 1, request_constraints))
                return true;
            chosen.erase(name);
        }
        return false;
    }
};

/// Render the structured explanation for a satisfiable result.
void explain_satisfiable(SolverResult& result, const std::map<std::string, PackageSpec>& packages,
                         const std::map<std::string, const CandidateVersion*>& chosen,
                         const std::map<std::string, std::vector<VersionConstraint>>& req_c) {
    for (const auto& [name, cand] : chosen) {
        // Clause evidence: top-level request and each dependency that drove
        // this selection.
        auto rc = req_c.find(name);
        if (rc != req_c.end()) {
            for (const auto& c : rc->second) {
                std::string op = c.op.empty() ? "(any)" : (c.op + " " + c.version);
                result.explanation.push_back(
                    {ExplainEntry::Kind::Clause, "clause: request " + name + " " + op + " -> " +
                                                     name + "@" + cand->version + " selected"});
            }
        }
        for (const auto& dep : cand->depends_on) {
            auto it = chosen.find(dep.package);
            if (it != chosen.end()) {
                std::string op = dep.op.empty() ? "(any)" : (dep.op + " " + dep.version);
                result.explanation.push_back(
                    {ExplainEntry::Kind::Clause,
                     "clause: " + name + "@" + cand->version + " depends_on " + dep.package + " " +
                         op + " -> " + dep.package + "@" + it->second->version + " selected"});
            }
        }

        // Preference evidence: why this candidate over the alternatives.
        const PackageSpec& spec = packages.at(name);
        if (spec.candidates.size() > 1) {
            if (is_unstable(cand->stability)) {
                result.explanation.push_back(
                    {ExplainEntry::Kind::Preference,
                     "preference: no stable candidate satisfied the constraints; chose " + name +
                         "@" + cand->version + " (" + stability_label(cand->stability) + ")"});
            } else {
                result.explanation.push_back(
                    {ExplainEntry::Kind::Preference, "preference: prefer stable " + name + "@" +
                                                         cand->version + " over other candidates"});
            }
        }

        // Stability warning (🎯T30): emit when forced onto a non-stable pick.
        if (is_unstable(cand->stability)) {
            std::string w = "unstable: " + name + " " + cand->version + " is " +
                            stability_label(cand->stability) +
                            " (no stable version satisfied the constraints)";
            result.warnings.push_back(w);
            result.explanation.push_back({ExplainEntry::Kind::Warning, w});
        }
    }
}

} // namespace

SolverResult solve(const std::map<std::string, PackageSpec>& packages,
                   const std::vector<VersionConstraint>& request) {
    SolverResult result;

    // Group top-level request constraints by package (a package may appear in
    // the request more than once with different bounds).
    std::map<std::string, std::vector<VersionConstraint>> req_constraints;
    for (const auto& r : request) {
        if (!packages.count(r.package)) {
            result.satisfiable = false;
            result.explanation.push_back(
                {ExplainEntry::Kind::Conflict,
                 "cannot resolve: requested package '" + r.package + "' is not available"});
            return result;
        }
        req_constraints[r.package].push_back(r);
    }

    Solver solver(packages);
    std::set<std::string> needed = solver.required_packages(request);
    std::vector<std::string> order(needed.begin(), needed.end());

    bool ok = solver.assign(order, 0, req_constraints);

    if (!ok) {
        result.satisfiable = false;
        if (solver.budget_exhausted) {
            result.explanation.push_back(
                {ExplainEntry::Kind::Conflict,
                 "cannot resolve: search budget exhausted before finding an assignment"});
        } else {
            result.explanation.push_back(
                {ExplainEntry::Kind::Conflict,
                 "unsatisfiable: no version assignment satisfies all constraints; the requested "
                 "packages have incompatible dependency or conflict requirements"});
        }
        return result;
    }

    result.satisfiable = true;
    for (const auto& [name, cand] : solver.chosen)
        result.assignment[name] = cand->version;

    explain_satisfiable(result, packages, solver.chosen, req_constraints);
    return result;
}

} // namespace den::sat

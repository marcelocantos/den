// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "dep_resolve.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <functional>
#include <set>
#include <vector>

namespace den {

namespace {

/// Build the solver universe: the transitive closure of the requested
/// packages over their dependencies, plus any package named in a
/// conflicts_with edge so conflict clauses can be encoded.  Bounded by the
/// index size — each package is visited at most once (visited set).
void collect_universe(const PackageIndex& idx, const std::vector<std::string>& requested,
                      std::set<std::string>& universe) {
    std::vector<std::string> stack(requested.begin(), requested.end());
    while (!stack.empty()) {
        std::string name = stack.back();
        stack.pop_back();
        if (universe.count(name))
            continue;
        const Package* pkg = idx.find(name);
        if (!pkg) {
            // Missing dependency: record the name so the solver reports it as
            // unsatisfiable rather than silently dropping it.
            universe.insert(name);
            continue;
        }
        universe.insert(name);
        for (const auto& dep : pkg->dependencies)
            stack.push_back(dep);
        for (const auto& c : pkg->conflicts_with)
            stack.push_back(c);
    }
}

} // namespace

ResolveResult resolve_request(const PackageIndex& idx, const std::vector<std::string>& requested,
                              const std::vector<std::string>& installed) {
    ResolveResult out;

    std::set<std::string> installed_set(installed.begin(), installed.end());

    std::set<std::string> universe;
    collect_universe(idx, requested, universe);

    // Translate the universe into a solver model. The index is single-version,
    // so each present package contributes exactly one candidate.
    std::map<std::string, sat::PackageSpec> packages;
    for (const auto& name : universe) {
        const Package* pkg = idx.find(name);
        if (!pkg)
            continue; // missing — leave absent so the solver flags UNSAT
        sat::PackageSpec spec;
        spec.name = name;
        sat::CandidateVersion cand;
        cand.version = pkg->version;
        cand.stability = pkg->stability;
        cand.installed = installed_set.count(name) > 0;
        cand.conflicts = pkg->conflicts_with;
        for (const auto& dep : pkg->dependencies)
            cand.depends_on.push_back({dep, "", ""}); // bare name -> any version
        spec.candidates.push_back(std::move(cand));
        packages[name] = std::move(spec);
    }

    std::vector<sat::VersionConstraint> request;
    request.reserve(requested.size());
    for (const auto& name : requested)
        request.push_back({name, "", ""});

    sat::SolverResult solved = sat::solve(packages, request);
    out.satisfiable = solved.satisfiable;
    out.warnings = solved.warnings;
    out.explanation = solved.explanation;

    if (!solved.satisfiable)
        return out;

    // Topologically order the chosen packages: dependencies before dependents.
    // Cycle-guarded DFS (defensive-coding: graph traversal needs a guard).
    std::set<std::string> chosen;
    for (const auto& [name, _] : solved.assignment)
        chosen.insert(name);

    std::set<std::string> visited;
    std::set<std::string> in_stack;
    std::vector<const Package*> order;

    std::function<void(const std::string&)> visit = [&](const std::string& name) {
        if (visited.count(name))
            return;
        if (in_stack.count(name)) {
            SPDLOG_WARN("dependency cycle detected at '{}'", name);
            return;
        }
        const Package* pkg = idx.find(name);
        if (!pkg)
            return;
        in_stack.insert(name);
        for (const auto& dep : pkg->dependencies) {
            if (chosen.count(dep))
                visit(dep);
        }
        in_stack.erase(name);
        visited.insert(name);
        order.push_back(pkg);
    };

    // Visit requested roots first for deterministic ordering, then any
    // remaining chosen packages (e.g. soft deps with no requesting root yet).
    for (const auto& name : requested)
        if (chosen.count(name))
            visit(name);
    for (const auto& name : chosen)
        visit(name);

    out.install_order = std::move(order);
    return out;
}

} // namespace den

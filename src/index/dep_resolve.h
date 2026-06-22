// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Bridge between den's PackageIndex and the SAT-based dependency solver
// (🎯T29 / 🎯T63).  Builds a solver universe from the index, resolves a
// request, and returns the packages to install in dependency order.

#include "package.h"
#include "sat_solver.h"

#include <string>
#include <vector>

namespace den {

/// Outcome of resolving an install request through the SAT solver.
struct ResolveResult {
    bool satisfiable = true;
    /// Packages to install, dependencies before dependents.
    std::vector<const Package*> install_order;
    /// Stability and other warnings surfaced to the user.
    std::vector<std::string> warnings;
    /// Structured solver reasoning (for `den deps --explain`).
    std::vector<sat::ExplainEntry> explanation;
};

/// Resolve a set of requested package names against the index using the SAT
/// solver.  The index is single-version-per-package, so each indexed package
/// contributes one candidate; the solver still enforces version constraints,
/// conflicts, and stability preferences across the transitive closure.
///
/// `installed` lists package names already present in the active environment;
/// the solver treats their candidate as the preferred (installed) one.
ResolveResult resolve_request(const PackageIndex& idx, const std::vector<std::string>& requested,
                              const std::vector<std::string>& installed = {});

} // namespace den

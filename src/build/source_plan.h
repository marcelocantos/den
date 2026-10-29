// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Dependency plans for source builds (G3) and the routing of kegs that can
// only live in Homebrew's Cellar to a source build (G9).
//
// A source build needs its dependencies in den's Cellar before it starts:
// runtime dependencies (`depends_on "x"`) and, for a keg den builds, its
// build-only dependencies (`depends_on "x" => :build`). Each one is
//   - already installed: nothing to do;
//   - poured, when it has a bottle that pours into den's Cellar (:any or
//     :any_skip_relocation, or a fixed Cellar that is den's own);
//   - built from source, when its only bottle is for a fixed Cellar that is
//     not den's (e.g. /opt/homebrew/Cellar) and the formula has a source.
// The plan lists them dependencies first, so git's chain comes out as
// openssl@4 (built) before curl (poured, links opt/openssl@4) before git, and
// pkgconf is built as a build-only dependency.
//
// Every source build in a plan goes through the same trust gate as
// `den install -s` (trust/unattended.h): interactive runs build; unattended
// runs build only allowlisted formulae whose source hash cross-checks, and
// defer the rest to the ledger. A deferred step defers every step that needs
// it, so a chain is never half built.

#include "../core/config.h"
#include "../index/package.h"
#include "../trust/unattended.h"

#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace den {

enum class SourceStepAction {
    Installed, // already in den's Cellar at the indexed version
    Pour,      // bottle pours into den's Cellar
    Build,     // built from source
};

struct SourceStep {
    std::string name;    // index name
    std::string version; // indexed pkg_version
    SourceStepAction action = SourceStepAction::Installed;
    /// Needed only to build something else in the plan, never at run time
    /// by a requested package. Such kegs are installed but not recorded in
    /// the environment.
    bool build_only = false;
    /// Plan steps this one needs before it can be poured or built (its
    /// runtime dependencies, plus its build dependencies when it is built).
    std::vector<std::string> needs;
    /// Why it is built from source ("requested", or the fixed Cellar its
    /// bottle needs). Empty for other actions.
    std::string why;
};

struct SourcePlan {
    std::vector<SourceStep> steps; // dependencies before dependents
    const SourceStep* find(const std::string& name) const;
};

struct SourcePlanOptions {
    /// Build these from source even when a bottle would pour (the targets of
    /// `den install -s`). Their dependencies still pour when they can.
    std::set<std::string> force_source;
};

/// True when `pkg` has a bottle for this platform that pours into den's
/// Cellar. Otherwise `why`, when given, says why not (no bottle for the
/// platform, or the fixed Cellar the bottle needs).
bool bottle_pours_here(const Config& config, const Package& pkg, std::string* why = nullptr);

/// The fixed Cellar `pkg`'s bottle for this platform was built for, when
/// that is not den's Cellar (the bottle cannot be poured here); "" when the
/// bottle pours, or there is no bottle for this platform.
std::string foreign_fixed_cellar(const Config& config, const Package& pkg);

/// True when den can build `pkg` from its Homebrew formula.
bool has_source_build(const Package& pkg);

/// Plan how to get `targets` (and everything they need) into den's Cellar.
/// Throws UserError for a package missing from the index, a dependency
/// cycle, or a package that neither pours nor has a source build.
SourcePlan plan_source_build(const Config& config, const PackageIndex& idx,
                             const std::vector<std::string>& targets,
                             const SourcePlanOptions& opts = {});

/// What the trust gate decided for one source build.
enum class SourceGate { Build, Deferred };

/// The trust gate for building `pkg` from source in this run. Returns Build
/// or Deferred (after recording the deferral in the ledger and telling the
/// user); throws UserError when the source hashes disagree.
SourceGate gate_source_build(const Config& config, const TrustPolicy& policy, const Package& pkg);

/// How a plan's steps are carried out. Tests replace these; the defaults pour
/// with pour_formula_keg and build with build_from_source.
struct SourcePlanRunner {
    std::function<void(const Config&, const Package&)> pour;
    std::function<std::filesystem::path(const Config&, const PackageIndex&, const std::string&,
                                        const std::string&)>
        build;
};

SourcePlanRunner default_source_plan_runner();

/// Replace the runner the provider's fixed-Cellar routing uses (tests only);
/// nullptr restores the default.
void set_source_plan_runner_for_testing(const SourcePlanRunner* runner);
const SourcePlanRunner& active_source_plan_runner();

/// G9: get `pkg`, whose bottle needs `foreign_fixed_cellar(config, pkg)`,
/// into den's Cellar by a source build instead (with any build dependencies
/// it needs), through the trust gate. Throws TrustHeldBack when an
/// unattended run defers it (already in the ledger).
void route_to_source_build(const Config& config, const PackageIndex& idx, const Package& pkg);

struct SourcePlanResult {
    std::vector<std::string> poured;
    std::vector<std::string> built;
    /// Deferred by the trust gate, or because a step they need was.
    std::vector<std::string> deferred;
    bool is_deferred(const std::string& name) const;
};

/// Carry out `plan` in order. Bottles go through the bottle trust check as
/// usual (an unattended hold counts as a deferral); source builds go through
/// gate_source_build. A step whose needs include a deferred step is deferred
/// too and recorded in the ledger as "depends on <step>: deferred".
SourcePlanResult run_source_plan(const Config& config, const PackageIndex& idx,
                                 const SourcePlan& plan, const TrustPolicy& policy,
                                 const SourcePlanRunner& runner);

/// One line per step, for `--dry-run` style output and logs:
/// "build openssl@4 4.1.0 (bottle needs /opt/homebrew/Cellar)".
std::string describe_source_plan(const SourcePlan& plan);

} // namespace den

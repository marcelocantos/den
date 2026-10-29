// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "source_plan.h"

#include "../core/error.h"
#include "../index/bottle_cellar.h"
#include "../platform/platform.h"
#include "../provider/homebrew_provider.h"
#include "../store/link.h"
#include "../store/store.h"
#include "../trust/degraded_installs.h"
#include "../trust/trust_model.h"
#include "source_build.h"

#include <spdlog/spdlog.h>

#include <iostream>
#include <map>
#include <sstream>

namespace den {

const SourceStep* SourcePlan::find(const std::string& name) const {
    for (const auto& s : steps)
        if (s.name == name)
            return &s;
    return nullptr;
}

bool SourcePlanResult::is_deferred(const std::string& name) const {
    for (const auto& d : deferred)
        if (d == name)
            return true;
    return false;
}

bool has_source_build(const Package& pkg) {
    return pkg.source_url.has_value() || pkg.ruby_source_path.has_value();
}

bool bottle_pours_here(const Config& config, const Package& pkg, std::string* why) {
    // Tap formulae that install a prebuilt download pour through
    // install_one's url_install path.
    if (pkg.archives.empty() && pkg.url_install)
        return true;
    std::vector<std::string> tags;
    for (const auto& [tag, _] : pkg.archives)
        tags.push_back(tag);
    auto best = best_archive_tag(config.arch, config.macos_version, tags);
    if (!best) {
        if (why)
            *why = "no bottle for this platform";
        return false;
    }
    const auto& archive = pkg.archives.at(*best);
    if (bottle_pours_into(*best, archive, config.store))
        return true;
    if (why)
        *why = "bottle needs " + required_bottle_cellar(*best, archive);
    return false;
}

std::string foreign_fixed_cellar(const Config& config, const Package& pkg) {
    std::vector<std::string> tags;
    for (const auto& [tag, _] : pkg.archives)
        tags.push_back(tag);
    auto best = best_archive_tag(config.arch, config.macos_version, tags);
    if (!best)
        return {};
    const auto& archive = pkg.archives.at(*best);
    if (bottle_pours_into(*best, archive, config.store))
        return {};
    return required_bottle_cellar(*best, archive);
}

namespace {

struct Planner {
    const Config& config;
    const PackageIndex& idx;
    const SourcePlanOptions& opts;
    SourcePlan plan;
    std::map<std::string, int> state; // 1 = visiting, 2 = done
    std::vector<std::string> stack;

    const Package& lookup(const std::string& name, const std::string& wanted_by) {
        const Package* pkg = idx.find(name);
        if (!pkg) {
            throw UserError("'" + name + "' is not in the package index" +
                            (wanted_by.empty() ? "" : " (needed by " + wanted_by + ")") +
                            ". Run `den update` first.");
        }
        if (pkg->artifact_type == ArtifactType::App || !pkg->cask_artifacts.empty())
            throw UserError("'" + name + "' is a cask and cannot be part of a source build");
        return *pkg;
    }

    void visit(const std::string& name, const std::string& wanted_by) {
        auto it = state.find(name);
        if (it != state.end()) {
            if (it->second == 1) {
                std::string cycle;
                bool on = false;
                for (const auto& s : stack) {
                    on = on || s == name;
                    if (on)
                        cycle += s + " -> ";
                }
                throw UserError("dependency cycle: " + cycle + name);
            }
            return;
        }
        state[name] = 1;
        stack.push_back(name);
        const Package& pkg = lookup(name, wanted_by);

        SourceStep step;
        step.name = name;
        step.version = pkg.version;
        std::string why;
        // A requested source build happens even over an installed keg, as
        // `den install -s` always has.
        if (opts.force_source.count(name)) {
            if (!has_source_build(pkg))
                throw UserError("no source build is available for " + name);
            step.action = SourceStepAction::Build;
            step.why = "requested";
        } else if (is_installed(config.store, keg_name(name), pkg.version)) {
            step.action = SourceStepAction::Installed;
        } else if (bottle_pours_here(config, pkg, &why)) {
            step.action = SourceStepAction::Pour;
        } else if (has_source_build(pkg)) {
            step.action = SourceStepAction::Build;
            step.why = why;
        } else {
            throw UserError("cannot install " + name + " " + pkg.version + ": " + why +
                            " and no source build is available" +
                            (wanted_by.empty() ? "" : " (needed by " + wanted_by + ")"));
        }

        for (const auto& dep : pkg.dependencies) {
            visit(dep, name);
            step.needs.push_back(dep);
        }
        // Build dependencies matter only for a keg den builds itself.
        if (step.action == SourceStepAction::Build) {
            for (const auto& dep : pkg.build_dependencies) {
                visit(dep, name);
                step.needs.push_back(dep);
            }
        }
        plan.steps.push_back(std::move(step));
        stack.pop_back();
        state[name] = 2;
    }
};

} // namespace

SourcePlan plan_source_build(const Config& config, const PackageIndex& idx,
                             const std::vector<std::string>& targets,
                             const SourcePlanOptions& opts) {
    Planner p{config, idx, opts, {}, {}, {}};
    for (const auto& t : targets)
        p.visit(t, "");

    // Whatever a target reaches through runtime dependencies is needed at
    // run time; everything else in the plan is build-only.
    std::set<std::string> runtime;
    std::vector<std::string> todo(targets.begin(), targets.end());
    while (!todo.empty()) {
        auto n = todo.back();
        todo.pop_back();
        if (!runtime.insert(n).second)
            continue;
        if (const Package* pkg = idx.find(n))
            for (const auto& d : pkg->dependencies)
                todo.push_back(d);
    }
    for (auto& s : p.plan.steps)
        s.build_only = !runtime.count(s.name);
    return std::move(p.plan);
}

SourceGate gate_source_build(const Config& config, const TrustPolicy& policy, const Package& pkg) {
    if (!policy.unattended)
        return SourceGate::Build;
    MaybeHash source_hash = pkg.source_sha256 ? MaybeHash(*pkg.source_sha256) : std::nullopt;
    ReplicaAnswer replica =
        lookup_local_replica(config, pkg.name, pkg.upstream_version(), "source");
    TrustCheckResult check = cross_check_hashes(source_hash, replica);
    bool accepted = is_install_accepted(config, pkg.name, pkg.version);
    switch (source_build_action(policy, pkg.name, check.decision, accepted)) {
    case SourceBuildAction::Build:
        return SourceGate::Build;
    case SourceBuildAction::Refuse:
        throw UserError("refusing to build '" + pkg.name + " " + pkg.version +
                        "' from source: trust check failed — " + check.message);
    case SourceBuildAction::Defer:
        break;
    }
    bool listed = policy.source_builds && policy.source_build_allowlist.count(pkg.name);
    DegradedInstall d{pkg.name, pkg.version, "unattended-source-build", "source", {}};
    d.status = !policy.source_builds ? "trust.unattended_source_builds=off"
               : !listed             ? "not in trust.source_build_allowlist"
                                     : replica_status_tag(replica.status);
    d.bottle_sha = pkg.source_sha256.value_or("");
    record_refused_install(config, d);
    std::cerr << "deferred: source build of " << pkg.name << " " << pkg.version << " ("
              << *policy.unattended << ": " << d.status << "); review with `den trust review`\n";
    return SourceGate::Deferred;
}

SourcePlanRunner default_source_plan_runner() {
    SourcePlanRunner r;
    r.pour = [](const Config& c, const Package& p) { pour_formula_keg(c, p); };
    r.build = [](const Config& c, const PackageIndex& i, const std::string& n,
                 const std::string& v) { return build_from_source(c, i, n, v); };
    return r;
}

namespace {
const SourcePlanRunner* g_test_runner = nullptr;
}

void set_source_plan_runner_for_testing(const SourcePlanRunner* runner) {
    g_test_runner = runner;
}

const SourcePlanRunner& active_source_plan_runner() {
    static const SourcePlanRunner def = default_source_plan_runner();
    return g_test_runner ? *g_test_runner : def;
}

SourcePlanResult run_source_plan(const Config& config, const PackageIndex& idx,
                                 const SourcePlan& plan, const TrustPolicy& policy,
                                 const SourcePlanRunner& runner) {
    SourcePlanResult out;
    // Every keg in the plan needs <prefix>/opt/<name> before the next build
    // starts, not only at the final materialise: kegs built in opt-prefix
    // mode (and poured ones) load their dependencies' libraries through
    // opt/, and a later build in the same plan runs them (the bundled Ruby
    // fetches through the poured curl, git's build runs gettext's msgfmt).
    // Missing or dangling links are created; links den doesn't own are left.
    auto opt_link = [&](const std::string& name, const std::filesystem::path& keg) {
        std::error_code ec;
        if (std::filesystem::is_directory(keg, ec))
            ensure_prefix_opt_link(config.store, keg_name(name), keg);
    };
    for (const auto& step : plan.steps) {
        if (step.action == SourceStepAction::Installed) {
            opt_link(step.name, config.store / keg_name(step.name) / step.version);
            continue;
        }
        const Package* pkg = idx.find(step.name);
        if (!pkg)
            throw UserError("'" + step.name + "' is not in the package index");

        std::string blocked_by;
        for (const auto& n : step.needs)
            if (out.is_deferred(n)) {
                blocked_by = n;
                break;
            }
        if (!blocked_by.empty()) {
            // Half a chain is worse than none: hold this one back too, and
            // say why in the ledger.
            DegradedInstall d{pkg->name,
                              pkg->version,
                              "depends on " + blocked_by + ": deferred",
                              step.action == SourceStepAction::Build ? "source" : "bottle",
                              {}};
            d.status = "dependency-deferred";
            record_refused_install(config, d);
            std::cerr << "deferred: " << step.name << " " << step.version << " (" << d.reason
                      << ")\n";
            out.deferred.push_back(step.name);
            continue;
        }

        if (step.action == SourceStepAction::Pour) {
            try {
                runner.pour(config, *pkg);
            } catch (const TrustHeldBack&) {
                // Already in the ledger (install_one records the hold).
                out.deferred.push_back(step.name);
                continue;
            }
            clear_refused_install(config, step.name, step.version);
            opt_link(step.name, config.store / keg_name(step.name) / step.version);
            out.poured.push_back(step.name);
            continue;
        }

        if (gate_source_build(config, policy, *pkg) == SourceGate::Deferred) {
            out.deferred.push_back(step.name);
            continue;
        }
        if (!step.why.empty() && step.why != "requested")
            std::cout << "==> " << step.name << " " << step.version << ": " << step.why
                      << "; building from source\n";
        auto keg = runner.build(config, idx, step.name, step.version);
        clear_refused_install(config, step.name, step.version);
        opt_link(step.name, keg);
        out.built.push_back(step.name);
    }
    return out;
}

void route_to_source_build(const Config& config, const PackageIndex& idx, const Package& pkg) {
    auto plan = plan_source_build(config, idx, {pkg.name});
    SPDLOG_DEBUG("source plan for {}:\n{}", pkg.name, describe_source_plan(plan));
    auto res = run_source_plan(config, idx, plan, current_trust_policy(config),
                               active_source_plan_runner());
    if (res.is_deferred(pkg.name)) {
        throw TrustHeldBack(pkg.name, pkg.version,
                            "deferred '" + pkg.name + " " + pkg.version + "': its bottle needs " +
                                foreign_fixed_cellar(config, pkg) +
                                " and the source build was deferred; review with `den trust "
                                "review`");
    }
}

std::string describe_source_plan(const SourcePlan& plan) {
    std::ostringstream os;
    for (const auto& s : plan.steps) {
        switch (s.action) {
        case SourceStepAction::Installed:
            os << "installed ";
            break;
        case SourceStepAction::Pour:
            os << "pour ";
            break;
        case SourceStepAction::Build:
            os << "build ";
            break;
        }
        os << s.name << " " << s.version;
        if (s.build_only)
            os << " [build-only]";
        if (!s.why.empty())
            os << " (" << s.why << ")";
        os << "\n";
    }
    return os.str();
}

} // namespace den

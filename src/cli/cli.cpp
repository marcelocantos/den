// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "cli.h"
#include "daemon_status.h"
#include "install.h"
#include "pin.h"
#include "shell.h"
#include "uninstall_deps.h"

#include "../activity/activity.h"
#include "../build/relink_pins.h"
#include "../build/source_build.h"
#include "../build/source_plan.h"
#include "../core/config.h"
#include "../core/error.h"
#include "../core/version.h"
#include "../daemon/daemon.h"
#include "../doctor/doctor.h"
#include "../env/environment.h"
#include "../env/manifest.h"
#include "../index/dep_resolve.h"
#include "../index/index.h"
#include "../index/sat_solver.h"
#include "../migrate/migrate.h"
#include "../migrate/migrate_prefix.h"
#include "../platform/platform.h"
#include "../provider/exec.h"
#include "../provider/homebrew_provider.h"
#include "../provider/registry.h"
#include "../selfupdate/daemon_restart.h"
#include "../selfupdate/launchd_restart.h"
#include "../selfupdate/selfupdate.h"
#include "../settings/settings.h"
#include "../smoke/runner.h"
#include "../store/pins.h"
#include "../store/store.h"
#include "../supervisor/supervisor.h"
#include "../tap/tap.h"
#include "../trust/degraded_installs.h"
#include "../trust/trust_model.h"
#include "../trust/unattended.h"
#include "trust_review.h"

#include <CLI11.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <unistd.h>
#endif

namespace den {

namespace {

const char* agent_guide = "See agents-guide.md for the full agent guide.\n";

/// Format a byte count as a human-readable, deterministic size string.
/// Uses binary (1024) units and always two decimals for KB and above so the
/// output is stable across runs.
std::string human_size(uint64_t bytes) {
    constexpr uint64_t kKiB = 1024;
    if (bytes < kKiB) {
        return std::to_string(bytes) + " B";
    }
    constexpr const char* kUnits[] = {"KB", "MB", "GB", "TB", "PB"};
    double value = static_cast<double>(bytes);
    int unit = -1;
    do {
        value /= static_cast<double>(kKiB);
        ++unit;
    } while (value >= static_cast<double>(kKiB) && unit + 1 < static_cast<int>(std::size(kUnits)));
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value << " " << kUnits[unit];
    return out.str();
}

// Built-in demonstration dependency universe for `den deps --explain` when the
// requested packages are not present in the local index (e.g. before `den
// update`, or in tests with an empty DEN_HOME). It mirrors the SAT solver's
// corpus scenarios so the command always produces meaningful clause/preference
// reasoning. Returns the universe keyed by package name.
std::map<std::string, sat::PackageSpec> explain_demo_universe() {
    using sat::CandidateVersion;
    using sat::PackageSpec;
    using sat::Stability;
    std::map<std::string, PackageSpec> u;

    auto add = [&](const std::string& name, CandidateVersion cand) {
        u[name].name = name;
        u[name].candidates.push_back(std::move(cand));
    };

    // constraint_resolution: pkgX needs libbar >= 2.0.0; libbar has 1.5 & 2.0.
    add("libbar", {.version = "1.5.0"});
    add("libbar", {.version = "2.0.0"});
    add("pkgX", {.version = "1.0.0", .depends_on = {{"libbar", ">=", "2.0.0"}}});

    // stability_forced_unstable_warns: myapp needs libz >= 1.4.0; only the
    // testing-stability rc satisfies it.
    add("libz", {.version = "1.3.1", .stability = Stability::Stable});
    add("libz", {.version = "1.4.0-rc1", .stability = Stability::Testing});
    add("myapp", {.version = "1.0.0", .depends_on = {{"libz", ">=", "1.4.0"}}});

    // version_conflict_unsat: pkgA needs libfoo >= 2.0; pkgB needs libfoo <= 1.9.
    add("libfoo", {.version = "1.9.0"});
    add("libfoo", {.version = "2.0.0"});
    add("pkgA", {.version = "1.0.0", .depends_on = {{"libfoo", ">=", "2.0.0"}}});
    add("pkgB", {.version = "1.0.0", .depends_on = {{"libfoo", "<=", "1.9.0"}}});

    return u;
}

// Print the solver's reasoning for `den deps --explain`. Resolves the request
// against the real index when every requested package is present there;
// otherwise falls back to the built-in demo universe so the command always
// explains *something*.
void print_deps_explain(const PackageIndex& idx, const std::vector<std::string>& names) {
    bool all_indexed = !names.empty();
    for (const auto& n : names) {
        if (!idx.find(n)) {
            all_indexed = false;
            break;
        }
    }

    sat::SolverResult result;
    if (all_indexed) {
        auto resolution = resolve_request(idx, names);
        result.satisfiable = resolution.satisfiable;
        result.explanation = resolution.explanation;
        result.warnings = resolution.warnings;
        for (const auto* p : resolution.install_order)
            result.assignment[p->name] = p->version;
    } else {
        auto universe = explain_demo_universe();
        std::vector<sat::VersionConstraint> request;
        for (const auto& n : names)
            request.push_back({n, "", ""});
        result = sat::solve(universe, request);
    }

    std::cout << "Explaining dependency resolution for:";
    for (const auto& n : names)
        std::cout << " " << n;
    std::cout << "\n\n";

    if (!result.satisfiable) {
        std::cout << "Result: UNSATISFIABLE\n";
        for (const auto& e : result.explanation) {
            if (e.kind == sat::ExplainEntry::Kind::Conflict)
                std::cout << "  " << e.message << "\n";
        }
        return;
    }

    std::cout << "Result: satisfiable\n";
    std::cout << "Assignment:\n";
    for (const auto& [pkg, ver] : result.assignment)
        std::cout << "  " << pkg << " " << ver << "\n";

    std::cout << "Reasoning:\n";
    for (const auto& e : result.explanation) {
        switch (e.kind) {
        case sat::ExplainEntry::Kind::Clause:
        case sat::ExplainEntry::Kind::Preference:
        case sat::ExplainEntry::Kind::Conflict:
            std::cout << "  " << e.message << "\n";
            break;
        case sat::ExplainEntry::Kind::Warning:
            std::cout << "  warning: " << e.message << "\n";
            break;
        }
    }
}

/// Follow a Homebrew rename (cask old_tokens, formula oldnames): when `name`
/// is not in the index but a current package lists it as an old name, return
/// the current name and say so, as brew does. Otherwise return `name`.
std::string follow_rename(const PackageIndex& idx, const std::string& name) {
    if (idx.empty() || idx.find(name)) {
        return name;
    }
    if (const auto* pkg = idx.find_renamed(name)) {
        std::cerr << "==> " << name << " has been renamed to " << pkg->name << "\n";
        return pkg->name;
    }
    return name;
}

} // namespace

struct Cli::M {
    CLI::App app{"den — universal development environment manager", "den"};

    // Top-level flags.
    bool help_agent = false;
    bool unattended = false;

    // install
    std::vector<std::string> install_names;
    bool build_from_source = false;
    bool install_overwrite = false;
    bool install_force = false;
    std::string install_provider;

    // uninstall
    std::vector<std::string> uninstall_names;
    std::string uninstall_provider;
    bool uninstall_ignore_deps = false;

    // upgrade
    std::vector<std::string> upgrade_names;
    std::string upgrade_provider;
    bool upgrade_dry_run = false;
    bool upgrade_force = false;

    // list
    bool list_cellar = false;

    // info
    std::string info_name;

    // search
    std::string search_query;

    // deps
    std::vector<std::string> deps_names;
    bool deps_tree = false;
    bool deps_explain = false;

    // use
    std::string use_name;
    std::string use_version;

    // env subcommands
    std::string env_create_name;
    std::string env_remove_name;
    std::string env_use_name;

    // init / shell-env
    std::string init_shell;
    std::string shell_env_name;

    // set / get
    std::string set_key;
    std::string set_value;
    std::string get_key;

    // cleanup
    bool cleanup_dry_run = false;

    // migrate
    std::vector<std::string> migrate_names;
    bool migrate_dry_run = false;
    bool migrate_integrate_shell = false;
    bool migrate_no_health = false;

    // services
    std::vector<std::string> service_names;
    bool services_logs_follow = false;
    uint32_t services_stop_timeout = 10;

    // smoke
    std::string smoke_defs;
    int smoke_max = 0;

    // self-update
    bool selfupdate_check = false;
    std::string selfupdate_version;
    bool selfupdate_force = false;

    // run
    std::vector<std::string> run_argv;

    // which
    std::string which_file;

    // log
    int log_count = 20;
    bool log_json = false;

    // tap
    std::vector<std::string> tap_args; // [add name [source]]
    bool tap_list_flag = false;
    std::string tap_remove_name;
    std::string tap_url;

    void setup();
};

void Cli::M::setup() {
    app.set_version_flag("--version", std::string(DEN_VERSION));
    app.require_subcommand(0, 1);

    app.add_flag("--help-agent", help_agent, "Print help text and agent guide");
    app.add_flag("--unattended", unattended,
                 "Apply the unattended trust policy (trust.unattended) as the daemon does");

    // --- install ---
    auto* install = app.add_subcommand("install", "Install packages");
    install->add_option("names", install_names, "Package names to install")->required();
    install->add_flag("-s,--build-from-source", build_from_source,
                      "Build from source instead of pouring a bottle");
    install->add_flag("--overwrite", install_overwrite,
                      "Replace files in the prefix that conflict with these packages' links "
                      "(like `brew link --overwrite`)");
    install->add_option("--provider", install_provider,
                        "Provider to install through (default: auto-resolve)");
    install->add_flag("--force", install_force,
                      "Install a formula, or a formula dependency, that is disabled upstream "
                      "(warn instead of refusing; disabled casks are still refused)");
    install->callback([this] {
        auto cfg = Config::detect();
        auto idx = load_index(cfg.cache / "index.json");
        // Merge third-party taps (🎯T67) so tapped formulae resolve by both
        // their bare and "user/repo/name" forms before any provider runs.
        merge_taps_into_index(cfg, idx);
        // "user/repo/name" is recorded qualified in the manifest; the keg
        // and links use the bare name.
        for (auto& name : install_names)
            name = follow_rename(idx, resolve_tap_install_name(idx, name));

        MaterialiseOptions link_opts;
        if (this->install_overwrite) {
            link_opts.overwrite.insert(install_names.begin(), install_names.end());
        }

        if (this->build_from_source) {
            // Source builds are Homebrew-only — the Ruby DSL belongs to the
            // Homebrew ecosystem. --provider is ignored on this path.
            if (idx.empty())
                throw UserError(empty_index_message());
            install_from_source(cfg, idx, install_names, link_opts, active_source_plan_runner(),
                                this->install_force);
            return;
        }

        auto registry = make_default_registry(&idx);
        auto active = active_env_path(cfg.den_home);
        auto manifest = read_manifest(cfg.den_home, active);

        std::map<PackageProvider*, std::vector<std::string>> by_provider;
        for (const auto& name : install_names) {
            auto& provider = resolve_provider(registry, name, manifest, this->install_provider);
            by_provider[&provider].push_back(name);
        }
        // The index is only consulted by the Homebrew provider; require it
        // only when a Homebrew install is actually in the batch, so installs
        // through pip/npm/go/cargo work without `den update`. Refuse the
        // whole batch up front (non-zero exit) rather than installing the
        // rest and silently exiting 0.
        if (idx.empty()) {
            for (const auto& [provider, names] : by_provider) {
                if (provider->provider_name() == "homebrew")
                    throw UserError(empty_index_message());
            }
        }
        for (auto& [provider, names] : by_provider) {
            install_packages(cfg, *provider, idx, names, link_opts, this->install_force);
        }
    });

    // --- uninstall ---
    auto* uninstall = app.add_subcommand("uninstall", "Uninstall packages");
    uninstall->add_option("names", uninstall_names, "Package names to uninstall")->required();
    uninstall->add_option("--provider", uninstall_provider,
                          "Provider that owns the package (default: auto-resolve)");
    uninstall->add_flag("--ignore-dependencies", uninstall_ignore_deps,
                        "Uninstall even if other installed packages need it at runtime");
    uninstall->callback([this] {
        auto cfg = Config::detect();
        auto registry = make_default_registry(nullptr);
        auto active = active_env_path(cfg.den_home);
        auto manifest = read_manifest(cfg.den_home, active);

        // Map each name onto the manifest key it means, so bare, qualified
        // and user/homebrew-repo forms of a tap formula all find its entry.
        std::optional<PackageIndex> idx;
        auto index = [&]() -> const PackageIndex& {
            if (!idx) {
                idx = load_index(cfg.cache / "index.json");
                merge_taps_into_index(cfg, *idx);
            }
            return *idx;
        };
        for (auto& name : uninstall_names) {
            bool listed = false;
            for (const auto& [_, bucket] : manifest.packages) {
                listed = listed || bucket.count(name) > 0;
            }
            if (!listed) {
                name = resolve_uninstall_name(index(), manifest, name);
            }
        }

        std::map<PackageProvider*, std::vector<std::string>> by_provider;
        for (const auto& name : uninstall_names) {
            auto& provider = resolve_provider(registry, name, manifest, this->uninstall_provider);
            by_provider[&provider].push_back(name);
        }

        // brew parity: refuse to remove a package that installed packages
        // (in any environment) still need at runtime. Build-only
        // dependencies don't count. Nothing is removed if any is refused.
        if (!this->uninstall_ignore_deps) {
            std::vector<std::string> brew_names;
            for (const auto& [provider, names] : by_provider) {
                if (provider->provider_name() == "homebrew") {
                    brew_names.insert(brew_names.end(), names.begin(), names.end());
                }
            }
            if (!brew_names.empty()) {
                auto dependents = find_runtime_dependents(cfg, index(), active, brew_names);
                if (!dependents.empty()) {
                    std::cerr << format_dependents_refusal(dependents);
                    std::exit(1);
                }
            }
        }
        for (auto& [provider, names] : by_provider) {
            uninstall_packages(cfg, *provider, names);
        }
        // Re-materialise the active environment so the symlinks for the
        // removed packages are cleaned up (materialise's stale-cleanup pass
        // does the actual unlink work).
        materialise(cfg.den_home, cfg.store, active);
    });

    // --- upgrade ---
    auto* upgrade = app.add_subcommand("upgrade", "Upgrade installed packages");
    upgrade->add_option("names", upgrade_names, "Package names to upgrade (all if empty)");
    upgrade->add_option("--provider", upgrade_provider,
                        "Provider whose packages to upgrade (default: auto-resolve)");
    upgrade->add_flag("-n,--dry-run", upgrade_dry_run,
                      "Show what would be upgraded or held, without changing anything");
    upgrade->add_flag("--force", upgrade_force,
                      "Upgrade a named formula, or pour a formula dependency, that is disabled "
                      "upstream (warn instead of refusing; disabled casks are still refused)");
    upgrade->callback([this] {
        auto cfg = Config::detect();
        auto idx = load_index(cfg.cache / "index.json");
        // Manifest entries may be tap-qualified ("user/repo/name").
        merge_taps_into_index(cfg, idx);
        // Every form (all, named, --dry-run) needs the index to know what is
        // newer; without it there is nothing to compare against, so fail
        // rather than exit 0 as though everything were up to date.
        require_index(idx);
        auto registry = make_default_registry(&idx);
        auto active = active_env_path(cfg.den_home);
        auto manifest = read_manifest(cfg.den_home, active);

        if (upgrade_names.empty()) {
            upgrade_all_packages(cfg, registry, idx, upgrade_dry_run, upgrade_force);
            return;
        }

        std::map<PackageProvider*, std::vector<std::string>> by_provider;
        for (const auto& name : upgrade_names) {
            auto& provider = resolve_provider(registry, name, manifest, this->upgrade_provider);
            by_provider[&provider].push_back(name);
        }
        for (auto& [provider, names] : by_provider) {
            // The package index describes Homebrew formulae and casks only. A
            // package another provider owns (e.g. pip's cowsay) is not that
            // index entry, so comparing them would report a bogus downgrade
            // or plan a homebrew install over it. Those providers have no
            // upgrade source yet; say so rather than misclassify.
            if (provider->provider_name() != "homebrew") {
                for (const auto& name : names)
                    std::cerr << "warning: '" << name << "' is managed by the "
                              << provider->provider_name()
                              << " provider; den upgrade does not track its versions yet\n";
                continue;
            }
            upgrade_packages(cfg, *provider, idx, names, upgrade_dry_run, upgrade_force);
        }
    });

    // --- pin / unpin ---
    register_pin_commands(app);

    // --- update ---
    auto* update = app.add_subcommand("update", "Refresh the package index");
    update->callback([] {
        auto cfg = Config::detect();
        auto idx = fetch_and_transform(cfg);
        auto cache_path = cfg.cache / "index.json";
        save_index(idx, cache_path);
        std::cout << "Updated index: " << idx.packages.size() << " packages loaded.\n";

        // One-time move of the root env from envs/ROOT/{bin,lib,...} into
        // the prefix (see migrate_root_env_to_prefix). No-op once done.
        auto report = migrate_root_env_to_prefix(cfg.den_home, cfg.store, &idx);
        if (report.migrated) {
            std::cout << "==> Linked the root environment into " << cfg.store.parent_path().string()
                      << " (" << report.removed << " old envs/ROOT links replaced";
            if (report.moved) {
                std::cout << ", " << report.moved << " other links moved";
            }
            std::cout << ").\n    envs/ROOT/bin etc. now point at the prefix; restart your "
                         "shell or run `eval \"$(den shell-env)\"` to pick up the new PATH.\n";
            for (const auto& p : report.preserved) {
                std::cout << "    kept non-link files aside in " << p.string() << "\n";
            }
            if (!report.conflicts.empty()) {
                std::cerr << "warning: " << report.conflicts << "\n";
            }
        }

        // Kegs poured before bottles bound their dependencies through
        // <prefix>/opt keep loading the versioned keg they were poured
        // against; relink them (see relink_pinned_kegs). No-op once done.
        auto relink = relink_pinned_kegs(cfg);
        if (!relink.relinked.empty()) {
            std::cout << "==> Relinked " << relink.relinked.size()
                      << " keg(s) to load their dependencies through "
                      << (cfg.store.parent_path() / "opt").string() << " (" << relink.files
                      << " files).\n";
            for (const auto& r : relink.relinked) {
                std::cout << "    " << r << "\n";
            }
        }
        if (!relink.left_pinned.empty()) {
            std::cout << "    " << relink.left_pinned.size()
                      << " pin(s) left in place (opt link not den's or another version); see "
                         "`den doctor`.\n";
        }
        for (const auto& f : relink.failed) {
            std::cerr << "warning: could not relink " << f
                      << " (file unchanged; `den reinstall` rebinds it)\n";
        }
    });

    // --- list ---
    auto* list = app.add_subcommand("list", "List installed packages");
    list->add_flag("--cellar", list_cellar,
                   "Inspect the Cellar: per-keg disk usage, env references, orphans");
    list->callback([this] {
        auto cfg = Config::detect();

        if (this->list_cellar) {
            auto kegs = inspect_cellar(cfg.store, cfg.den_home);
            if (kegs.empty()) {
                std::cout << "No kegs in Cellar.\n";
                return;
            }
            // Column widths for deterministic alignment.
            size_t max_name = std::string("NAME").size();
            size_t max_ver = std::string("VERSION").size();
            for (const auto& k : kegs) {
                max_name = std::max(max_name, k.name.size());
                max_ver = std::max(max_ver, k.version.size());
            }
            std::cout << std::left << std::setw(static_cast<int>(max_name + 2)) << "NAME"
                      << std::setw(static_cast<int>(max_ver + 2)) << "VERSION" << std::setw(12)
                      << "SIZE" << "REFS\n";
            uint32_t orphan_count = 0;
            for (const auto& k : kegs) {
                std::string refs;
                if (k.orphaned()) {
                    refs = "(orphan)";
                    ++orphan_count;
                } else {
                    for (size_t i = 0; i < k.env_refs.size(); ++i) {
                        if (i > 0)
                            refs += ",";
                        refs += k.env_refs[i];
                    }
                }
                std::cout << std::left << std::setw(static_cast<int>(max_name + 2)) << k.name
                          << std::setw(static_cast<int>(max_ver + 2)) << k.version << std::setw(12)
                          << human_size(k.disk_bytes) << refs << "\n";
            }
            std::cout << "\n" << kegs.size() << " keg(s), " << orphan_count << " orphan(s).\n";
            return;
        }

        // Uniform listing across providers: enumerate the active environment's
        // manifest (resolved through the parent chain), grouped per provider.
        // Each row is name / version / provider — no provider-specific syntax
        // leaks into the name column, and the set tracks `den install` /
        // `den uninstall` for every provider identically (T60 criterion 3).
        auto active = active_env_path(cfg.den_home);
        auto per_provider = resolve_per_provider(cfg.den_home, active);

        struct Row {
            std::string name;
            std::string version;
            std::string provider;
        };
        std::vector<Row> rows;
        for (const auto& [provider_name, pkgs] : per_provider) {
            for (const auto& [name, version] : pkgs) {
                rows.push_back({name, version, provider_name});
            }
        }

        if (rows.empty()) {
            std::cout << "No packages installed.\n";
            return;
        }

        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return std::tie(a.name, a.version) < std::tie(b.name, b.version);
        });

        size_t max_name = 0;
        size_t max_ver = 0;
        for (const auto& r : rows) {
            max_name = std::max(max_name, r.name.size());
            max_ver = std::max(max_ver, r.version.size());
        }
        for (const auto& r : rows) {
            std::cout << std::left << std::setw(static_cast<int>(max_name + 2)) << r.name
                      << std::setw(static_cast<int>(max_ver + 2)) << r.version << r.provider
                      << "\n";
        }
    });

    // --- run ---
    // `den run <binary> [args...]` execs a binary from the active env's bin/
    // directory, with that bin/ prepended to PATH so peer tools resolve. This
    // is the provider-agnostic way to invoke an installed package: a Homebrew
    // keg, a pip venv script, an npm CLI, a Go binary, and a Cargo binary all
    // surface into the same env/bin and run identically.
    auto* run = app.add_subcommand("run", "Run an installed binary from the active environment");
    run->add_option("cmd", run_argv, "Binary name followed by its arguments")->required();
    run->prefix_command(); // stop parsing after the binary name; pass the rest through verbatim
    run->callback([this, run] {
        // With prefix_command(), the binary name lands in run_argv and every
        // token after it (including flags like --version) is held as the
        // subcommand's remaining args. Stitch them back into one argv.
        std::vector<std::string> tokens = run_argv;
        for (const auto& extra : run->remaining()) {
            tokens.push_back(extra);
        }
        if (tokens.empty()) {
            std::cerr << "error: `den run` needs a binary name\n";
            std::exit(2);
        }
        auto cfg = Config::detect();
        auto active = active_env_path(cfg.den_home);
        auto bin_dir = env_dir(cfg.den_home, active) / "bin";
        auto binary = bin_dir / tokens.front();

        std::error_code ec;
        if (!fs::exists(binary, ec)) {
            std::cerr << "error: '" << tokens.front() << "' is not installed in environment '"
                      << active << "'\n";
            std::exit(127);
        }

        std::vector<std::string> argv;
        argv.reserve(tokens.size());
        argv.push_back(binary.string());
        for (size_t i = 1; i < tokens.size(); ++i) {
            argv.push_back(tokens[i]);
        }

        int code = run_tool_interactive(argv, {}, bin_dir.string());
        std::exit(code < 0 ? 127 : code);
    });

    // --- info ---
    auto* info = app.add_subcommand("info", "Show package info");
    info->add_option("name", info_name, "Package name")->required();
    info->callback([this] {
        auto cfg = Config::detect();
        auto idx = load_index(cfg.cache / "index.json");
        // Tapped formulae (🎯T67) are queryable by `den info` once merged.
        merge_taps_into_index(cfg, idx);
        info_name = follow_rename(idx, info_name);

        // Resolve which provider owns this package. A manifest hint or
        // `--provider` would refine this; for info we use the active env's
        // manifest so an already-installed non-Homebrew package reports under
        // its owning provider instead of falling through to the index.
        auto active = active_env_path(cfg.den_home);
        auto manifest = read_manifest(cfg.den_home, active);
        auto registry = make_default_registry(&idx);
        auto& provider = resolve_provider(registry, info_name, manifest);
        auto provider_name = std::string(provider.provider_name());

        // Non-Homebrew providers have no rich index metadata; report the
        // uniform fields drawn from the provider's own installed set.
        if (provider_name != "homebrew") {
            std::string version;
            for (const auto& installed : provider.list_installed(cfg)) {
                if (installed.name == info_name) {
                    version = installed.version;
                    break;
                }
            }
            if (version.empty())
                throw UserError("package not found: " + info_name + " (provider " + provider_name +
                                ")");
            std::cout << "Name:         " << info_name << "\n"
                      << "Version:      " << version << "\n"
                      << "Provider:     " << provider_name << "\n";
            return;
        }

        const auto* pkg = idx.find(info_name);
        if (!pkg) {
            if (idx.empty())
                throw UserError("package not found: " + info_name + " (" + empty_index_message() +
                                ")");
            throw UserError("package not found: " + info_name +
                            " (run `den update` to refresh the index)");
        }
        std::cout << "Name:         " << pkg->name << "\n"
                  << "Version:      " << pkg->version << "\n"
                  << "Description:  " << pkg->description << "\n"
                  << "Homepage:     " << pkg->homepage << "\n"
                  << "License:      " << pkg->license << "\n"
                  << "Type:         "
                  << (pkg->artifact_type == ArtifactType::App ? "app" : "binary") << "\n";
        if (!pkg->dependencies.empty()) {
            std::cout << "Dependencies: ";
            for (size_t i = 0; i < pkg->dependencies.size(); ++i) {
                if (i > 0)
                    std::cout << ", ";
                std::cout << pkg->dependencies[i];
            }
            std::cout << "\n";
        }
        // Show archive availability for current platform.
        auto candidates = bottle_tag_candidates(cfg.arch, cfg.macos_version);
        std::vector<std::string> available_tags;
        for (const auto& [tag, _] : pkg->archives) {
            available_tags.push_back(tag);
        }
        auto best = best_archive_tag(cfg.arch, cfg.macos_version, available_tags);
        if (best) {
            std::cout << "Archive:      available (" << *best << ")\n";
        } else if (!pkg->archives.empty()) {
            std::cout << "Archive:      not available for this platform\n"
                      << "              available for:";
            for (const auto& [tag, _] : pkg->archives) {
                std::cout << " " << tag;
            }
            std::cout << "\n";
        } else {
            std::cout << "Archive:      none (source build only)\n";
        }
    });

    // --- search ---
    auto* search = app.add_subcommand("search", "Search for packages");
    search->add_option("query", search_query, "Search query")->required();
    search->callback([this] {
        auto cfg = Config::detect();
        auto idx = load_index(cfg.cache / "index.json");
        // With no index there is nothing to search; fail rather than exit 0
        // as though the query simply had no matches.
        require_index(idx);
        // Search scans every package, so decode the whole index up front.
        idx.materialise_all();
        // Case-insensitive substring match.
        std::string query_lower = search_query;
        std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);
        std::vector<const Package*> matches;
        for (const auto& [name, pkg] : idx.packages) {
            std::string name_lower = name;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
            std::string desc_lower = pkg.description;
            std::transform(desc_lower.begin(), desc_lower.end(), desc_lower.begin(), ::tolower);
            if (name_lower.find(query_lower) != std::string::npos ||
                desc_lower.find(query_lower) != std::string::npos) {
                matches.push_back(&pkg);
            }
        }
        if (matches.empty()) {
            std::cout << "No packages matching '" << search_query << "'.\n";
            return;
        }
        // Find widest name for alignment.
        size_t max_name = 0;
        size_t max_ver = 0;
        for (const auto* pkg : matches) {
            max_name = std::max(max_name, pkg->name.size());
            max_ver = std::max(max_ver, pkg->version.size());
        }
        for (const auto* pkg : matches) {
            std::string desc = pkg->description;
            constexpr size_t kMaxDesc = 60;
            if (desc.size() > kMaxDesc) {
                desc = desc.substr(0, kMaxDesc - 3) + "...";
            }
            std::cout << std::left << std::setw(static_cast<int>(max_name + 2)) << pkg->name
                      << std::setw(static_cast<int>(max_ver + 2)) << pkg->version << desc << "\n";
        }
        std::cout << matches.size() << " packages found.\n";
    });

    // --- deps ---
    auto* deps = app.add_subcommand("deps", "Show package dependencies");
    deps->add_option("names", deps_names, "Package name(s)")->required();
    deps->add_flag("--tree", deps_tree, "Show dependency tree");
    deps->add_flag("--explain", deps_explain,
                   "Explain why the solver chose an assignment (clauses, preferences)");
    deps->callback([this] {
        auto cfg = Config::detect();
        auto idx = load_index(cfg.cache / "index.json");

        // --explain: surface the SAT solver's reasoning (🎯T63).
        if (deps_explain) {
            print_deps_explain(idx, deps_names);
            return;
        }

        const std::string& deps_name = deps_names.front();
        const auto* pkg = idx.find(deps_name);
        if (!pkg) {
            std::cerr << "Package not found: " << deps_name << "\n";
            return;
        }
        if (pkg->dependencies.empty()) {
            std::cout << deps_name << " has no dependencies.\n";
            return;
        }
        if (!deps_tree) {
            for (const auto& dep : pkg->dependencies) {
                std::cout << dep << "\n";
            }
            return;
        }
        // Recursive tree print with cycle detection.
        std::set<std::string> visited;
        struct TreePrinter {
            const PackageIndex& idx;
            std::set<std::string>& visited;
            void print(const std::string& name, int depth) {
                for (int i = 0; i < depth; ++i)
                    std::cout << "  ";
                std::cout << name;
                if (visited.count(name)) {
                    std::cout << " (circular)\n";
                    return;
                }
                std::cout << "\n";
                visited.insert(name);
                const auto* p = idx.find(name);
                if (p) {
                    for (const auto& dep : p->dependencies) {
                        print(dep, depth + 1);
                    }
                }
                visited.erase(name);
            }
        };
        TreePrinter printer{idx, visited};
        for (const auto& dep : pkg->dependencies) {
            printer.print(dep, 0);
        }
    });

    // --- cleanup ---
    // The Cellar is shared with Homebrew. Only kegs den itself poured (those
    // carrying an ownership receipt) are eligible, and only when no
    // environment still references that version.
    auto* cleanup = app.add_subcommand(
        "cleanup",
        "Remove unused versions den installed, and clear the archive cache. Kegs den "
        "did not install (including Homebrew's), and versions another keg still loads by "
        "versioned path, are kept");
    cleanup->add_flag("-n,--dry-run", cleanup_dry_run,
                      "Show what would be removed without deleting anything");
    cleanup->callback([this] {
        auto cfg = Config::detect();
        auto report = cleanup_kegs(cfg.store, cfg.den_home, cfg.cache, cleanup_dry_run);

        for (const auto& pkg : report.removed) {
            if (cleanup_dry_run) {
                std::cout << "Would remove " << pkg.name << " " << pkg.version << "\n";
            } else {
                std::cout << "Removing " << pkg.name << " " << pkg.version << "\n";
            }
        }
        for (const auto& k : report.kept_referenced) {
            std::cout << (cleanup_dry_run ? "Would keep " : "Keeping ") << k.keg.name << " "
                      << k.keg.version << ": " << k.by.name << " " << k.by.version
                      << " still loads it (" << k.file.lexically_relative(k.by.path).string()
                      << " -> " << k.load_path << ")\n";
        }
        if (report.kept_unowned > 0) {
            std::cout << (cleanup_dry_run ? "Would leave " : "Left ") << report.kept_unowned
                      << " keg(s) den did not install untouched.\n";
        }
        if (report.cache_cleared) {
            std::cout << (cleanup_dry_run ? "Would clear archive cache.\n"
                                          : "Cleared archive cache.\n");
        }
        if (cleanup_dry_run) {
            std::cout << "Would remove " << report.removed.size() << " old package version(s).\n";
        } else {
            std::cout << "Cleaned up " << report.removed.size() << " old package version(s).\n";
        }
    });

    // --- autoremove ---
    auto* autoremove = app.add_subcommand("autoremove", "Remove unused dependencies");
    autoremove->callback([] {
        auto cfg = Config::detect();
        auto active = active_env_path(cfg.den_home);
        auto manifest = read_manifest(cfg.den_home, active);

        // Autoremove is scoped to Homebrew today — its dependency graph is
        // available via the index. Other providers will grow their own
        // autoremove paths when they land.
        auto idx = load_index(cfg.cache / "index.json");
        const auto& homebrew_pkgs = manifest.packages["homebrew"];
        const auto& homebrew_auto = manifest.auto_deps["homebrew"];

        std::set<std::string> needed;
        for (const auto& [name, _] : homebrew_pkgs) {
            if (homebrew_auto.count(name))
                continue;
            // This is an explicit package — mark all its deps as needed.
            auto* pkg = idx.find(name);
            if (pkg) {
                for (const auto& dep : pkg->dependencies)
                    needed.insert(dep);
            }
        }

        std::vector<std::string> to_remove;
        for (const auto& name : homebrew_auto) {
            if (!needed.count(name)) {
                to_remove.push_back(name);
            }
        }

        if (to_remove.empty()) {
            std::cout << "No unused dependencies to remove.\n";
            return;
        }

        with_manifest(cfg.den_home, active, [&](Manifest& m) {
            auto& bucket = m.packages["homebrew"];
            auto& auto_bucket = m.auto_deps["homebrew"];
            for (const auto& name : to_remove) {
                bucket.erase(name);
                auto_bucket.erase(name);
                std::cout << "Removing unused dependency: " << name << "\n";
            }
        });

        auto links = materialise(cfg.den_home, cfg.store, active);
        std::cout << "Removed " << to_remove.size() << " unused dep(s). " << links
                  << " symlinks.\n";
    });

    // --- trust ---
    auto* trust_cmd =
        app.add_subcommand("trust", "Review installs the unattended trust policy held back");
    trust_cmd->require_subcommand(1);
    trust_cmd->add_subcommand("review", "List held-back, accepted and degraded installs")
        ->callback([] {
            auto cfg = Config::detect();
            format_trust_review(std::cout, summarize_degraded_installs(cfg));
        });
    static std::string trust_accept_target;
    auto* trust_accept = trust_cmd->add_subcommand(
        "accept", "Let one held-back <formula>@<version> install unattended");
    trust_accept->add_option("target", trust_accept_target, "<formula>@<version>")->required();
    trust_accept->callback([] {
        if (auto why = unattended_reason())
            throw UserError("`den trust accept` needs a person: this run is unattended (" + *why +
                            ")");
        auto target = parse_formula_at_version(trust_accept_target);
        if (!target)
            throw UserError("expected <formula>@<version>, got '" + trust_accept_target + "'");
        auto cfg = Config::detect();
        for (const auto& d : summarize_degraded_installs(cfg).refused_installs)
            if (d.name == target->first && d.version == target->second &&
                !hold_is_acceptable(d.reason))
                throw UserError(trust_accept_target + " is held back for '" + d.reason +
                                "', which `den trust accept` cannot override: only a "
                                "single-source hold can be accepted (see `den trust review`)");
        if (!accept_refused_install(cfg, target->first, target->second))
            throw UserError("nothing held back for " + trust_accept_target +
                            " (see `den trust review`)");
        std::cout << "Accepted " << trust_accept_target
                  << ". The daemon installs it on its next pass; `den upgrade " << target->first
                  << "` installs it now.\n";
    });

    // --- doctor ---
    auto* doctor_cmd = app.add_subcommand("doctor", "Check system for potential problems");
    doctor_cmd->callback([] {
        auto cfg = Config::detect();
        auto findings = doctor(cfg);
        if (findings.empty()) {
            std::cout << "Your system is ready to brew.\n";
        }
    });

    // --- replica-verify (🎯T42/T64) ---
    // Diff-based replica verifier used by the replica-verify CI pipeline.
    // Given a downloaded bottle and its claimed SHA256, recompute the digest
    // and report match/mismatch. Exit 0 on match, non-zero on mismatch so the
    // workflow can fail the run. Hidden from --help (an internal CI tool).
    auto* rv = app.add_subcommand("replica-verify",
                                  "Verify a downloaded bottle against a claimed SHA256 (CI tool)");
    rv->group(""); // hide from help listing
    auto rv_args = std::make_shared<std::array<std::string, 4>>();
    auto rv_bottle = std::make_shared<std::string>();
    rv->add_option("--name", (*rv_args)[0], "Formula name")->required();
    rv->add_option("--version", (*rv_args)[1], "Formula version")->required();
    rv->add_option("--tag", (*rv_args)[2], "Platform tag")->required();
    rv->add_option("--sha256", (*rv_args)[3], "Claimed SHA256 digest")->required();
    rv->add_option("--bottle", *rv_bottle, "Path to the downloaded bottle file")->required();
    rv->callback([rv_args, rv_bottle] {
        ReplicaDiffEntry entry{(*rv_args)[0], (*rv_args)[1], (*rv_args)[2], (*rv_args)[3]};
        auto outcome = verify_diff_entry(entry, fs::path(*rv_bottle));
        if (outcome == DiffVerifyOutcome::Match) {
            std::cout << "MATCH " << entry.formula_name << " " << entry.version << " "
                      << entry.platform_tag << "\n";
        } else {
            std::cout << "MISMATCH " << entry.formula_name << " " << entry.version << " "
                      << entry.platform_tag << "\n";
            throw UserError("replica diff verification failed for " + entry.formula_name + " " +
                            entry.version + " (" + entry.platform_tag + ")");
        }
    });

    // --- config ---
    auto* config = app.add_subcommand("config", "Show detected configuration");
    config->callback([] {
        auto cfg = Config::detect();
        auto na = [](const std::optional<std::string>& v) { return v ? *v : std::string("n/a"); };

        std::cout << "den_home:          " << cfg.den_home.string() << "\n"
                  << "prefix:            " << cfg.prefix.string()
                  << (cfg.shares_homebrew_cellar()
                          ? " (shared with Homebrew. This is an older den setup. Run `den "
                            "migrate-prefix` to give den its own folder.)"
                          : "")
                  << "\n"
                  << "prefix_source:     " << to_string(cfg.prefix_source) << "\n"
                  << "store:             " << cfg.store.string() << "\n"
                  << "cache:             " << cfg.cache.string() << "\n"
                  << "homebrew_prefix:   " << cfg.homebrew_prefix.string() << "\n"
                  << "homebrew_cellar:   " << cfg.homebrew_cellar.string() << "\n"
                  << "shares_brew_cellar: " << (cfg.shares_homebrew_cellar() ? "yes" : "no") << "\n"
                  << "prefix_precedence: DEN_PREFIX > config.json store.prefix > "
                     "DEN_HOME/prefix (if populated) > legacy shared Cellar > DEN_HOME/prefix\n"
                  << "                   (HOMEBREW_PREFIX/HOMEBREW_CELLAR only locate Homebrew "
                     "for `den migrate`; they never choose den's store)\n"
                  << "arch:              " << to_string(cfg.arch) << "\n"
                  << "macos_version:     "
                  << (cfg.macos_version ? cfg.macos_version->to_string() : "n/a") << "\n";

        // Host toolchain / OS facts (🎯T66/T2).
        auto facts = detect_host_facts();
#ifdef __APPLE__
        std::cout << "xcode_version:     " << na(facts.xcode_version) << "\n"
                  << "clt_version:       " << na(facts.clt_version) << "\n"
                  << "sdk_path:          " << na(facts.sdk_path) << "\n";
#else
        std::cout << "os_name:           " << na(facts.os_name) << "\n"
                  << "os_version:        " << na(facts.os_version) << "\n"
                  << "kernel:            " << na(facts.kernel) << "\n"
                  << "glibc:             " << na(facts.glibc) << "\n";
#endif

        // Homebrew-config parity: echo the stable `brew config` keys.
        if (facts.homebrew_config.empty()) {
            std::cout << "homebrew_config:   n/a (brew not found)\n";
        } else {
            std::cout << "homebrew_config:\n";
            for (const auto& [key, value] : facts.homebrew_config) {
                std::cout << "  " << std::left << std::setw(28) << (key + ":") << value << "\n";
            }
        }
    });

    // --- env ---
    auto* env = app.add_subcommand("env", "Manage environments");
    env->require_subcommand(1);

    auto* env_create = env->add_subcommand("create", "Create an environment");
    env_create->add_option("name", env_create_name, "Environment name")->required();
    env_create->callback([this] {
        auto cfg = Config::detect();
        std::string name = env_create_name;
        if (name.empty() || name[0] != '/') {
            name = "/" + name;
        }
        while (name.size() > 1 && name.back() == '/') {
            name.pop_back();
        }
        if (name == "/") {
            throw UserError("the root environment '/' always exists");
        }
        const auto mf = cfg.den_home / "manifests" / env_slug(name) / "manifest.json";
        if (fs::exists(mf)) {
            // Never clobber an existing env's packages.
            throw UserError("environment '" + name + "' already exists");
        }
        Manifest m;
        m.origin = env_parent(name);
        write_manifest(cfg.den_home, name, m);
        std::cout << "Created environment '" << name << "' (inherits from " << *m.origin << ").\n";
    });

    auto* env_list = env->add_subcommand("list", "List environments");
    env_list->callback([] {
        auto cfg = Config::detect();
        auto envs = list_all(cfg.den_home);
        if (envs.empty()) {
            std::cout << "No environments.\n";
        } else {
            for (const auto& e : envs)
                std::cout << e << "\n";
        }
    });

    auto* env_remove = env->add_subcommand("remove", "Remove an environment");
    env_remove->add_option("name", env_remove_name, "Environment name")->required();
    env_remove->callback([this] {
        if (env_remove_name == "/") {
            SPDLOG_ERROR("cannot remove the root environment");
            return;
        }
        auto cfg = Config::detect();
        // Path naming makes /ml/exp a child of /ml; silently re-parenting it
        // to / would change what it contains. Refuse and name the children.
        if (auto kids = env_descendants(cfg.den_home, env_remove_name); !kids.empty()) {
            std::string list;
            for (const auto& k : kids) {
                list += "\n  " + k;
            }
            throw UserError("cannot remove environment '" + env_remove_name +
                            "': other environments inherit from it:" + list +
                            "\nRemove those first.");
        }
        auto manifest_dir = cfg.den_home / "manifests" / env_slug(env_remove_name);
        auto envdir = env_dir(cfg.den_home, env_remove_name);
        std::error_code ec;
        fs::remove_all(manifest_dir, ec);
        fs::remove_all(envdir, ec);
        std::cout << "Removed environment '" << env_remove_name << "'.\n";
    });

    auto* env_use = env->add_subcommand("use", "Switch to an environment");
    env_use->add_option("name", env_use_name, "Environment name")->required();
    env_use->callback([this] {
        auto cfg = Config::detect();
        set_active_env(cfg.den_home, env_use_name);
        auto slug = env_slug(env_use_name);
        print_env_switch(cfg, slug);
    });

    auto* env_show = env->add_subcommand("show", "Show active environment");
    env_show->callback([] {
        auto cfg = Config::detect();
        auto active = active_env_path(cfg.den_home);
        auto resolved = resolve(cfg.den_home, active);
        if (resolved.empty()) {
            std::cout << "Environment '" << active << "' has no packages.\n";
        } else {
            std::cout << "Environment: " << active << "\n";
            for (const auto& [name, version] : resolved)
                std::cout << "  " << std::left << std::setw(30) << name << " " << version << "\n";
        }
    });

    auto* env_freeze = env->add_subcommand("freeze", "Export environment as lockfile");
    env_freeze->callback([] {
        auto cfg = Config::detect();
        auto active = active_env_path(cfg.den_home);
        auto resolved = resolve(cfg.den_home, active);
        // Output as JSON lockfile.
        nlohmann::json j;
        j["environment"] = active;
        j["packages"] = nlohmann::json::object();
        for (const auto& [name, version] : resolved) {
            j["packages"][name] = version;
        }
        std::cout << j.dump(2) << "\n";
    });

    // --- use ---
    auto* use = app.add_subcommand("use", "Switch active version of a package");
    use->add_option("name", use_name, "Package name")->required();
    use->add_option("version", use_version, "Version to activate")->required();
    use->callback([this] {
        auto cfg = Config::detect();
        auto active = active_env_path(cfg.den_home);
        auto manifest = read_manifest(cfg.den_home, active);

        auto idx = load_index(cfg.cache / "index.json");
        auto registry = make_default_registry(&idx);
        auto& provider = resolve_provider(registry, use_name, manifest);
        auto provider_name = std::string(provider.provider_name());

        // Check the requested version exists in the store for this provider.
        if (!provider.is_installed(cfg, use_name, use_version)) {
            // Try to install it via the resolved provider.
            auto* pkg = idx.find(use_name);
            if (pkg && pkg->version == use_version) {
                install_packages(cfg, provider, idx, {use_name});
            } else {
                SPDLOG_ERROR("version {} of {} is not installed and not available in the index",
                             use_version, use_name);
                return;
            }
        }

        // Switch the manifest entry and relink as one unit: a version that
        // cannot be linked leaves the current one recorded and linked.
        std::string from;
        {
            auto m = read_manifest(cfg.den_home, active);
            if (auto b = m.packages.find(provider_name); b != m.packages.end()) {
                if (auto it = b->second.find(use_name); it != b->second.end())
                    from = it->second;
            }
        }
        VersionChangeOptions vopts;
        vopts.install = false;
        auto res =
            apply_version_changes(cfg, provider, nullptr, {{use_name, from, use_version}}, vopts);
        report_version_change_failures(cfg, res, "switch");
        std::cout << "Switched " << use_name << " to " << use_version << " (" << res.links
                  << " symlinks).\n";
    });

    // --- init ---
    auto* init = app.add_subcommand("init", "Print shell init script");
    init->add_option("--shell", init_shell, "Shell type (bash, zsh, fish)");
    init->callback([this] {
        auto cfg = Config::detect();
        std::string shell = init_shell;
        if (shell.empty()) {
            const char* env_shell = std::getenv("SHELL");
            if (env_shell) {
                // Extract basename: /bin/zsh -> zsh
                std::string s(env_shell);
                auto pos = s.rfind('/');
                shell = (pos != std::string::npos) ? s.substr(pos + 1) : s;
            } else {
                shell = "bash";
            }
        }
        print_shell_init(cfg, shell);
    });

    // --- shell-env (eval'd by init / den env use wrapper; not a user command) ---
    auto* shell_env =
        app.add_subcommand("shell-env", "Print env switch exports for eval (used by den init)");
    shell_env->add_option("env", shell_env_name,
                          "Environment path or slug (default: active; / = root)");
    shell_env->callback([this] {
        auto cfg = Config::detect();
        print_env_switch(cfg, shell_env_name);
    });

    // --- status ---
    auto* status = app.add_subcommand("status", "Show environment status");
    status->callback([] {
        auto cfg = Config::detect();
        auto active = active_env_path(cfg.den_home);
        auto resolved = resolve(cfg.den_home, active);

        std::cout << "den: " << DEN_VERSION << "\n";
        std::cout << "Active environment: " << active << "\n";
        std::cout << "Packages: " << resolved.size() << "\n";

        // Check daemon state.
        auto state = read_daemon_state(cfg.den_home);
        if (!state.pending.empty()) {
            std::cout << "Pending upgrades: " << state.pending.size() << "\n";
            for (const auto& p : state.pending) {
                std::cout << "  " << p.name << " " << p.installed << " -> " << p.available << "\n";
            }
        }
        if (!state.index_older.empty()) {
            std::cout << "Not downgrading (index older than installed): "
                      << state.index_older.size() << "\n";
            for (const auto& p : state.index_older) {
                std::cout << "  " << p.name << " " << p.installed << " (index has " << p.available
                          << ")\n";
            }
        }
        if (!state.version_held.empty()) {
            std::cout << "Held (index version below installed): " << state.version_held.size()
                      << "\n";
            for (const auto& h : state.version_held)
                std::cout << "  " << h.name << " " << h.installed << " (" << h.reason << ")\n";
        }
        format_disabled_blocked_section(std::cout, state);

        if (is_daemon_running(cfg)) {
            std::cout << "Daemon: running\n";
        } else {
            std::cout << "Daemon: not running\n";
        }
    });

    // --- set ---
    auto* set = app.add_subcommand("set", "Set a configuration value");
    set->add_option("key", set_key, "Setting key")->required();
    set->add_option("value", set_value, "Setting value")->required();
    set->callback([this] {
        auto cfg = Config::detect();
        try {
            set_setting(cfg.den_home, set_key, set_value);
        } catch (const SettingsFileError& e) {
            throw UserError(e.what());
        }
        std::cout << set_key << " = " << set_value << "\n";
    });

    // --- get ---
    // Exit status: 0 = set (value on stdout), 1 = not set, 2 = config.json
    // unreadable/invalid or bad key. install.sh relies on this split.
    auto* get = app.add_subcommand("get", "Print a configuration value");
    get->add_option("key", get_key, "Setting key")->required();
    get->callback([this] {
        auto cfg = Config::detect();
        try {
            std::cout << get_setting(cfg.den_home, get_key) << "\n";
        } catch (const SettingNotFound&) {
            std::cerr << "den: " << get_key << " is not set\n";
            throw CLI::RuntimeError(1);
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            throw CLI::RuntimeError(2);
        }
    });

    // --- settings ---
    auto* settings_cmd = app.add_subcommand("settings", "Show all configuration settings");
    settings_cmd->callback([] {
        auto cfg = Config::detect();
        std::cout << display_all_settings(cfg.den_home) << "\n";
    });

    // --- tap (🎯T67: third-party Homebrew taps) ---
    auto* tap = app.add_subcommand("tap", "Add, list, or remove third-party formula taps");
    tap->add_option("args", tap_args, "`add <user/repo> [source]` to tap; bare for list")
        ->expected(0, 3);
    tap->add_flag("--list", tap_list_flag, "List registered taps");
    tap->add_option("--remove", tap_remove_name, "Remove a registered tap (user/repo)");
    tap->add_option("--url", tap_url, "Clone source for `tap add` (git URL or local path)");
    tap->callback([this] {
        auto cfg = Config::detect();

        // --remove takes priority.
        if (!tap_remove_name.empty()) {
            tap_remove(cfg, tap_remove_name);
            std::cout << "Untapped " << tap_remove_name << "\n";
            return;
        }

        // `tap add <name> [source]` — also accepts `--url <source>`.
        if (!tap_args.empty() && tap_args.front() == "add") {
            if (tap_args.size() < 2) {
                std::cerr << "error: `den tap add` needs a tap name (user/repo)\n";
                std::exit(2);
            }
            const std::string& name = tap_args[1];
            std::string source = tap_args.size() >= 3 ? tap_args[2] : tap_url;
            if (source.empty()) {
                // Same default as `brew tap user/repo`:
                // https://github.com/user/homebrew-repo.git.
                source = default_tap_url(name);
            }
            auto added = tap_add(cfg, name, source);
            std::cout << "Tapped " << added.name << "\n";
            return;
        }

        // Bare `den tap` or `den tap --list`: print registered taps.
        auto taps = tap_list(cfg);
        if (taps.empty()) {
            std::cout << "No taps registered.\n";
            return;
        }
        for (const auto& t : taps) {
            std::cout << t.name << "\n";
        }
    });

    // --- migrate ---
    auto* migrate =
        app.add_subcommand("migrate", "Migrate formulae, casks, taps, and services from Homebrew");
    migrate->add_option("names", migrate_names,
                        "Specific formulae/casks to migrate (default: everything)");
    migrate->add_flag("--dry-run", migrate_dry_run,
                      "Compute and print the migration summary without writing anything");
    migrate->add_flag("--integrate-shell", migrate_integrate_shell,
                      "Append `eval \"$(den init)\"` to your shell profile (opt-in)");
    migrate->add_flag("--no-health-check", migrate_no_health,
                      "Skip the post-migration health check");
    migrate->callback([this] {
        auto cfg = Config::detect();
        MigrateOptions opts;
        opts.dry_run = migrate_dry_run;
        opts.integrate_shell = migrate_integrate_shell;
        opts.query_services = true; // real CLI path may consult `brew services`
        migrate_from_homebrew(cfg, migrate_names, opts);
        if (!migrate_dry_run && !migrate_no_health) {
            check_migration_health(cfg, /*print=*/true);
        }
    });

    // --- migrate-prefix ---
    auto* migrate_prefix = app.add_subcommand(
        "migrate-prefix",
        "Move den out of Homebrew's prefix into its own (dry run unless --apply)");
    static std::string mp_to;
    static bool mp_apply = false;
    static bool mp_resume = false;
    static bool mp_rollback = false;
    static bool mp_stop_services = false;
    static std::vector<std::string> mp_only;
    migrate_prefix->add_option("--to", mp_to, "Den's new prefix (default: $DEN_HOME/prefix)");
    migrate_prefix->add_flag("--apply", mp_apply,
                             "Reinstall den's kegs into the new prefix and switch to it");
    migrate_prefix
        ->add_option("--only", mp_only,
                     "Pour only these kegs this run (den switches once all are in)")
        ->delimiter(',');
    migrate_prefix->add_flag("--resume", mp_resume, "Continue a run that stopped part-way");
    migrate_prefix->add_flag("--rollback", mp_rollback,
                             "Switch den back to the old prefix (keeps the new one on disk)");
    migrate_prefix->add_flag("--stop-services", mp_stop_services,
                             "Stop running den services and the daemon for the move, then "
                             "restart them");
    migrate_prefix->callback([] {
        if (static_cast<int>(mp_apply) + static_cast<int>(mp_resume) +
                static_cast<int>(mp_rollback) >
            1) {
            throw UserError("use one of --apply, --resume, --rollback");
        }
        auto cfg = Config::detect();
        MigratePrefixOptions opts;
        opts.to = mp_to;
        opts.apply = mp_apply;
        opts.resume = mp_resume;
        opts.rollback = mp_rollback;
        opts.stop_services = mp_stop_services;
        opts.only = std::set<std::string>(mp_only.begin(), mp_only.end());
        const char* home = std::getenv("HOME");
        opts.home = home ? fs::path(home) : fs::path("/tmp");
        int rc = run_migrate_prefix(cfg, opts, std::cout);
        std::cout.flush();
        if (rc != 0) {
            throw CLI::RuntimeError(rc);
        }
    });

    // --- daemon ---
    auto* daemon = app.add_subcommand("daemon", "Manage the background daemon");
    daemon->require_subcommand(1);

    auto* daemon_run = daemon->add_subcommand("run", "Run daemon foreground");
    static bool daemon_once = false;
    daemon_run->add_flag("--once", daemon_once, "Run one maintenance cycle now and exit");
    daemon_run->callback([] {
        auto cfg = Config::detect();
        run_daemon(cfg, daemon_once);
    });

    auto* daemon_stop = daemon->add_subcommand("stop", "Stop the daemon");
    daemon_stop->callback([] {
        auto cfg = Config::detect();
        stop_daemon(cfg);
    });

    auto* daemon_status = daemon->add_subcommand("status", "Show daemon status");
    daemon_status->callback([] {
        auto cfg = Config::detect();
        if (is_daemon_running(cfg))
            std::cout << "Daemon: running\n";
        else
            std::cout << "Daemon: not running\n";

        auto state = read_daemon_state(cfg.den_home);
        format_daemon_status(std::cout, state);
        auto ledger = summarize_degraded_installs(cfg);
        format_degraded_trust(std::cout, ledger);
        format_trust_status(std::cout, ledger,
                            make_trust_policy(read_settings(cfg.den_home).trust, std::nullopt));

        auto s = read_settings(cfg.den_home);
        std::cout << "Auto-download: " << (s.daemon.auto_download ? "on" : "off") << "\n";
        std::cout << "Auto-upgrade: " << (s.daemon.auto_upgrade ? "on" : "off") << "\n";
        if (s.daemon.upgrade_window)
            std::cout << "Upgrade window: " << *s.daemon.upgrade_window << "\n";
    });

    auto* daemon_install = daemon->add_subcommand("install", "Install daemon service");
    daemon_install->callback([] {
#ifdef __APPLE__
        auto cfg = Config::detect();
        // Resolve this process's binary (macOS has no /proc/self/exe).
        fs::path exe;
        {
            char buf[4096];
            uint32_t size = sizeof(buf);
            if (_NSGetExecutablePath(buf, &size) != 0) {
                SPDLOG_ERROR("cannot determine den executable path for launchd");
                return;
            }
            std::error_code ec;
            exe = fs::canonical(buf, ec);
            if (ec) {
                exe = fs::path(buf);
            }
        }
        auto plist = launchd_plist(exe, cfg.den_home, cfg.prefix);
        auto home = fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp");
        auto plist_path = home / "Library" / "LaunchAgents" / "dev.den.daemon.plist";
        // Regenerating is the explicit fix for a stale plist (den doctor
        // points here), so a differing plist is replaced, after a backup.
        write_daemon_plist(plist_path, plist, cfg.den_home / "dev.den.daemon.plist.bak", std::cout);
        // bootout gui/<uid>/<label> + bootstrap gui/<uid> <plist>, launchctl
        // run with a scrubbed environment, then confirm a running pid (see
        // selfupdate/launchd_restart.h).
        auto ops = real_launchd_ops("dev.den.daemon", plist_path);
        auto before = ops.launchctl({"print", ops.domain + "/" + ops.label});
        auto old = parse_launchctl_print(before.spawned && before.exit_code == 0, before.output);
        std::ostringstream detail;
        int rc = restart_launchd_job(ops, old.pid, detail) == LaunchdRestartResult::Failed ? 1 : 0;
        std::cout << detail.str();
        if (rc == 0)
            std::cout << "Daemon installed and started.\n  Plist: " << plist_path << "\n";
        else
            SPDLOG_ERROR("failed to load launchd plist");
#else
        SPDLOG_ERROR("daemon install via launchd is only supported on macOS");
#endif
    });

    auto* daemon_uninstall = daemon->add_subcommand("uninstall", "Uninstall daemon service");
    daemon_uninstall->callback([] {
#ifdef __APPLE__
        auto home = fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp");
        auto plist_path = home / "Library" / "LaunchAgents" / "dev.den.daemon.plist";
        if (fs::exists(plist_path)) {
            auto ops = real_launchd_ops("dev.den.daemon", plist_path);
            (void)ops.launchctl({"bootout", ops.domain + "/" + ops.label});
            fs::remove(plist_path);
            std::cout << "Daemon uninstalled.\n";
        } else {
            std::cout << "Daemon is not installed.\n";
        }
#else
        SPDLOG_ERROR("daemon uninstall via launchd is only supported on macOS");
#endif
    });

    // --- outdated ---
    auto* outdated = app.add_subcommand("outdated", "List packages with updates available");
    outdated->callback([] {
        auto cfg = Config::detect();
        auto idx = load_index(cfg.cache / "index.json");
        // Without the index nothing can be compared; fail rather than exit 0
        // as though everything were up to date (same as `den upgrade`).
        require_index(idx);
        // The installed set is the active environment's manifest, per
        // provider, not the keg store: the same view `den upgrade`, the
        // daemon and `den doctor` use (outdated_plan -> compute_pending), so
        // the lists cannot drift. Kegs no manifest records (a stray or older
        // keg) are left to `den doctor` / `den cleanup`.
        const auto plan = outdated_plan(cfg, idx);
        if (plan.homebrew == 0 && plan.untracked.empty()) {
            std::cout << "No packages installed.\n";
            return;
        }
        const auto pins = read_pins(cfg.den_home);
        struct Row {
            std::string name, installed, available, note;
            bool upgrade;
        };
        const auto dstate = read_daemon_state(cfg.den_home);
        std::vector<Row> rows;
        for (const auto& p : plan.pending) {
            std::string note;
            if (auto pin = pins.find(p.name); pin != pins.end())
                note = "  [pinned at " + pin->second + "]";
            for (const auto& b : dstate.disabled_blocked)
                if (b.name == p.name && b.available == p.available)
                    note += "  [daemon: would pour " + b.disabled + ", disabled upstream]";
            rows.push_back({p.name, p.installed, p.available, note, true});
        }
        // Never offered as upgrades; listed so they are not a surprise.
        for (const auto& p : plan.index_older)
            rows.push_back({p.name, p.installed, p.available,
                            "  [index older than installed; not upgrading]", false});
        for (const auto& h : plan.held)
            rows.push_back(
                {h.name, h.installed, h.available,
                 "  [" + h.reason + "; to downgrade: den use " + h.name + " " + h.available + "]",
                 false});
        std::sort(rows.begin(), rows.end(),
                  [](const Row& a, const Row& b) { return a.name < b.name; });
        size_t count = 0;
        size_t max_name = 0;
        for (const auto& r : rows)
            max_name = std::max(max_name, r.name.size());
        for (const auto& u : plan.untracked)
            max_name = std::max(max_name, u.name.size());
        for (const auto& r : rows) {
            std::cout << std::left << std::setw(static_cast<int>(max_name + 2)) << r.name
                      << r.installed << " -> " << r.available << r.note << "\n";
            if (r.upgrade)
                ++count;
        }
        // Check for den self-update.
        auto self = check_for_update(cfg);
        if (self) {
            max_name = std::max(max_name, std::string("den").size());
            std::cout << std::left << std::setw(static_cast<int>(max_name + 2)) << "den"
                      << DEN_VERSION << " -> " << self->latest_version << "\n";
            ++count;
        }

        // Surface upgrades the daemon has deferred because a managed binary is
        // in use (🎯T51 / 🎯T72), so the user understands why an available
        // upgrade has not yet applied.
        if (!dstate.deferred.empty()) {
            std::cout << "Deferred (managed binary in use):\n";
            for (const auto& d : dstate.deferred) {
                std::cout << "  " << std::left << std::setw(static_cast<int>(max_name + 2))
                          << d.name << d.installed << " -> " << d.available << "  ["
                          << (d.reason.empty() ? "in use" : d.reason) << "]\n";
            }
        }
        // Upgrades the daemon will not apply because they would pour a
        // package disabled upstream (one itself disabled is not a row above).
        format_disabled_blocked_section(std::cout, dstate);
        if (dstate.deferred.empty() && dstate.disabled_blocked.empty() && count == 0) {
            std::cout << (plan.untracked.empty() ? "All packages are up to date.\n"
                                                 : "All tracked packages are up to date.\n");
        }

        // Other providers' packages (pip, cargo, go, npm) are not compared
        // against the Homebrew index; `den upgrade <name>` warns and skips
        // them the same way.
        if (!plan.untracked.empty()) {
            std::cout << "Not tracked for upgrades (managed by another provider):\n";
            for (const auto& u : plan.untracked)
                std::cout << "  " << std::left << std::setw(static_cast<int>(max_name + 2))
                          << u.name << u.version << "  (" << u.provider << ")\n";
        }
    });

    // --- log ---
    auto* log_cmd = app.add_subcommand("log", "Show upgrade activity log");
    log_cmd->add_option("-n,--count", log_count, "Number of entries to show (default 20)");
    log_cmd->add_flag("--json", log_json, "Output as JSON");
    log_cmd->callback([this] {
        auto cfg = Config::detect();
        auto entries =
            read_activity(cfg.den_home, log_count > 0 ? static_cast<size_t>(log_count) : 0);
        if (entries.empty()) {
            std::cout << "No upgrade activity recorded.\n";
            return;
        }
        if (log_json) {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& e : entries) {
                arr.push_back({
                    {"timestamp", e.timestamp},
                    {"package", e.package},
                    {"from", e.from_version},
                    {"to", e.to_version},
                    {"trigger", e.trigger},
                });
            }
            std::cout << arr.dump(2) << "\n";
            return;
        }
        // Human-readable output.
        for (const auto& e : entries) {
            std::time_t t = static_cast<std::time_t>(e.timestamp);
            std::tm* lt = std::localtime(&t);
            char buf[20] = {};
            if (lt)
                std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", lt);
            std::cout << buf << "  " << std::left << std::setw(24) << e.package << e.from_version
                      << " -> " << e.to_version << "  (" << e.trigger << ")\n";
        }
    });

    // --- whence ---
    auto* whence = app.add_subcommand("whence", "Show which package owns a file");
    whence->add_option("file", which_file, "File path or command name to look up")->required();
    whence->callback([this] {
        auto cfg = Config::detect();
        fs::path target(which_file);

        // Look up in the shared Cellar.
        auto try_lookup = [&](const fs::path& file) -> std::optional<InstalledPackage> {
            return which_package(cfg.store, file);
        };

        // If it's an explicit path (contains a separator), just look it up directly.
        if (target.filename() != target) {
            auto result = try_lookup(target);
            if (result) {
                std::cout << result->name << " " << result->version << "\n";
            } else {
                std::cerr << "No package owns " << which_file << "\n";
                std::exit(1);
            }
            return;
        }

        // Bare name: scan entire PATH, report all matches.
        const char* path_env = std::getenv("PATH");
        if (!path_env) {
            std::cerr << "PATH is not set\n";
            std::exit(1);
        }

        // First pass: collect all matches.
        struct Match {
            std::string path;
            std::optional<InstalledPackage> pkg;
        };
        std::vector<Match> matches;

        std::string path_str(path_env);
        std::string::size_type start = 0;
        while (start <= path_str.size()) {
            auto end = path_str.find(':', start);
            auto dir = path_str.substr(start, end == std::string::npos ? end : end - start);
            if (!dir.empty()) {
                auto candidate = fs::path(dir) / which_file;
                std::error_code ec;
                if (fs::exists(candidate, ec)) {
                    matches.push_back({candidate.string(), try_lookup(candidate)});
                }
            }
            if (end == std::string::npos)
                break;
            start = end + 1;
        }

        if (matches.empty()) {
            std::cerr << which_file << " not found on PATH\n";
            std::exit(1);
        }

        // Second pass: print column-aligned.
        size_t max_path = 0;
        for (const auto& m : matches)
            max_path = std::max(max_path, m.path.size());

        for (const auto& m : matches) {
            std::cout << std::left << std::setw(static_cast<int>(max_path + 2)) << m.path;
            if (m.pkg) {
                std::cout << m.pkg->name << " " << m.pkg->version;
            } else {
                std::cout << "(not managed by den)";
            }
            std::cout << "\n";
        }
    });

    // --- services (🎯T33 / 🎯T61: built-in supervisor, no launchctl) ---
    auto* services = app.add_subcommand("services", "Manage package services");
    services->require_subcommand(1);

    auto fmt_uptime = [](uint64_t now, std::optional<uint64_t> start) -> std::string {
        if (!start)
            return "-";
        if (now < *start)
            return "0s";
        uint64_t s = now - *start;
        std::ostringstream o;
        if (s >= 86400)
            o << (s / 86400) << "d";
        else if (s >= 3600)
            o << (s / 3600) << "h";
        else if (s >= 60)
            o << (s / 60) << "m";
        else
            o << s << "s";
        return o.str();
    };

    auto* svc_list = services->add_subcommand("list", "List running services");
    svc_list->callback([fmt_uptime] {
        auto cfg = Config::detect();
        auto entries = list_services(cfg);
        if (entries.empty()) {
            std::cout << "No services found.\n";
            return;
        }
        std::cout << std::left << std::setw(24) << "NAME" << std::setw(8) << "STATUS"
                  << std::setw(10) << "PID" << std::setw(10) << "UPTIME" << "RESTART\n";
        auto now = static_cast<uint64_t>(std::time(nullptr));
        for (const auto& s : entries) {
            std::cout << std::left << std::setw(24) << s.name << std::setw(8)
                      << (s.running ? "running" : "stopped") << std::setw(10)
                      << (s.svc_pid ? std::to_string(*s.svc_pid) : std::string("-"))
                      << std::setw(10)
                      << (s.running ? fmt_uptime(now, s.start_time) : std::string("-"))
                      << restart_policy_to_string(s.restart) << "\n";
        }
    });

    auto* svc_start = services->add_subcommand("start", "Start services");
    svc_start->add_option("names", service_names, "Service names")->required();
    svc_start->callback([this] {
        auto cfg = Config::detect();
        for (const auto& name : service_names) {
            auto spec = resolve_service_spec(cfg, name);
            if (!spec) {
                SPDLOG_ERROR("no service spec for '{}' (looked for spec.json and homebrew plist)",
                             name);
                continue;
            }
            if (start_service(cfg, *spec)) {
                std::cout << "Started " << name << "\n";
            } else {
                SPDLOG_ERROR("failed to start '{}'", name);
            }
        }
    });

    auto* svc_stop = services->add_subcommand("stop", "Stop services");
    svc_stop->add_option("names", service_names, "Service names")->required();
    svc_stop->add_option("--timeout", services_stop_timeout,
                         "Seconds to wait for graceful exit before SIGKILL (default 10)");
    svc_stop->callback([this] {
        auto cfg = Config::detect();
        auto stopped = stop_services_ordered(cfg, service_names, services_stop_timeout);
        for (const auto& name : stopped)
            std::cout << "Stopped " << name << "\n";
    });

    auto* svc_restart = services->add_subcommand("restart", "Restart services");
    svc_restart->add_option("names", service_names, "Service names")->required();
    svc_restart->callback([this] {
        auto cfg = Config::detect();
        // Stop in dependency order, then start in reverse (dependencies first).
        auto stop_order = stop_services_ordered(cfg, service_names, services_stop_timeout);
        std::vector<std::string> start_order(stop_order.rbegin(), stop_order.rend());
        for (const auto& name : start_order) {
            auto spec = resolve_service_spec(cfg, name);
            if (!spec) {
                SPDLOG_ERROR("no service spec for '{}'", name);
                continue;
            }
            if (start_service(cfg, *spec))
                std::cout << "Restarted " << name << "\n";
            else
                SPDLOG_ERROR("failed to restart '{}'", name);
        }
    });

    auto* svc_status = services->add_subcommand("status", "Show status of a service");
    svc_status->add_option("name", service_names, "Service name")->required();
    svc_status->callback([this, fmt_uptime] {
        auto cfg = Config::detect();
        auto now = static_cast<uint64_t>(std::time(nullptr));
        for (const auto& name : service_names) {
            auto s = service_status(cfg, name);
            if (!s) {
                SPDLOG_ERROR("unknown service '{}'", name);
                continue;
            }
            std::cout << "name:     " << s->name << "\n";
            std::cout << "status:   " << (s->running ? "running" : "stopped") << "\n";
            std::cout << "pid:      " << (s->svc_pid ? std::to_string(*s->svc_pid) : "-") << "\n";
            std::cout << "sup-pid:  " << (s->sup_pid ? std::to_string(*s->sup_pid) : "-") << "\n";
            std::cout << "uptime:   "
                      << (s->running ? fmt_uptime(now, s->start_time) : std::string("-")) << "\n";
            std::cout << "restart:  " << restart_policy_to_string(s->restart) << "\n";
            std::cout << "restarts: " << s->restart_count << "\n";
            std::cout << "last-exit:" << (s->last_exit ? std::to_string(*s->last_exit) : "-")
                      << "\n";
        }
    });

    auto* svc_logs = services->add_subcommand("logs", "Show service logs");
    svc_logs->add_option("name", service_names, "Service name")->required();
    svc_logs->add_flag("-f,--follow", services_logs_follow, "Follow log output");
    svc_logs->callback([this] {
        auto cfg = Config::detect();
        for (const auto& name : service_names) {
            if (services_logs_follow)
                follow_service_log(cfg, name);
            else
                cat_service_log(cfg, name);
        }
    });

    // --- self-update ---
    auto* selfupdate = app.add_subcommand("self-update", "Update den to the latest version");
    selfupdate->add_flag("--check,--dry-run", selfupdate_check,
                         "Print the latest available version without downloading or applying it");
    selfupdate->add_option("--version", selfupdate_version,
                           "Install this release instead of the latest (e.g. 0.15.0, "
                           "v0.15.0-rc.1); release candidates and downgrades are allowed");
    selfupdate->add_flag("--force", selfupdate_force,
                         "Reinstall even when the requested release's binary is identical to "
                         "the installed one (and restart the daemon onto it)");
    selfupdate->callback([this] {
        // Validate an explicit version before anything touches the network.
        const std::string wanted_tag =
            selfupdate_version.empty() ? std::string() : normalize_release_tag(selfupdate_version);
        auto cfg = Config::detect();
        // A brew-installed den is upgraded by brew, never in place.
        const auto exe = running_exe_path();
        const bool brew_managed = exe && brew_keg_for_exe(*exe).has_value();
        if (!wanted_tag.empty()) {
            if (brew_managed && !selfupdate_check)
                refuse_self_update_if_brew_managed(*exe);
            auto info = fetch_release_for_version(cfg, wanted_tag);
            if (selfupdate_check) {
                std::cout << "den " << info.tag << " is available (current: " << DEN_VERSION
                          << ").\n";
                return;
            }
            if (apply_update(info, cfg, selfupdate_force))
                restart_daemon_after_update(cfg, std::cout);
            return;
        }
        if (selfupdate_check) {
            auto latest = resolve_latest_version();
            if (latest.empty()) {
                std::cout << "Unable to determine the latest version.\n";
                return;
            }
            // Compare numerically: a GitHub "latest" older than this binary
            // (e.g. 0.12.0 vs 1.0.0) is not an available update.
            if (sat::version_compare(latest, DEN_VERSION) <= 0) {
                std::cout << "den " << DEN_VERSION << " is the latest version.\n";
            } else {
                std::cout << "den " << latest << " is available (current: " << DEN_VERSION
                          << ").\n";
                if (brew_managed)
                    std::cout << "Installed by Homebrew: run `" << kBrewUpgradeCommand
                              << "` to update.\n";
            }
            return;
        }
        if (brew_managed)
            refuse_self_update_if_brew_managed(*exe);
        auto info = check_for_update(cfg);
        if (!info) {
            std::cout << "den " << DEN_VERSION << " is up to date.\n";
            return;
        }
        if (apply_update(*info, cfg, selfupdate_force))
            restart_daemon_after_update(cfg, std::cout);
    });

    // --- smoke ---
    auto* smoke = app.add_subcommand("smoke", "Run smoke tests against installed packages");
    smoke->add_option("--defs", smoke_defs, "Path to test definitions JSON")
        ->default_val("tests/smoke/tier1.json");
    smoke->add_option("--max,-n", smoke_max, "Max packages to test (0 = all)");
    smoke->callback([this] {
        // Use a fresh isolated DEN_HOME for smoke tests.
        auto cfg = Config::detect();
        auto smoke_home = fs::temp_directory_path() / "den-smoke-test";
        fs::create_directories(smoke_home);
        cfg.den_home = smoke_home;
        cfg.set_prefix(smoke_home / "prefix", PrefixSource::DenHome);
        cfg.cache = smoke_home / "cache";

        // Copy the index cache so we don't re-fetch.
        auto real_cfg = Config::detect();
        auto src_index = real_cfg.cache / "index.json";
        if (fs::exists(src_index)) {
            fs::create_directories(cfg.cache);
            fs::copy_file(src_index, cfg.cache / "index.json",
                          fs::copy_options::overwrite_existing);
        }

        auto results = run_smoke_tests(smoke_defs, cfg, smoke_max);
        int failed = 0;
        for (const auto& r : results)
            if (!r.all_passed())
                ++failed;
        if (failed > 0) {
            std::exit(1);
        }
    });
}

Cli::Cli() : m(std::make_unique<M>()) {
    m->setup();
}

Cli::~Cli() = default;

int Cli::run(int argc, char** argv) {
    // Subcommand callbacks run inside parse(), so mark the run unattended
    // before parsing rather than from the flag's value afterwards.
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--unattended") {
            set_unattended(std::string("--unattended"));
            break;
        }
        if (std::string_view(argv[i]) == "--")
            break;
    }
    try {
        m->app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return m->app.exit(e);
    } catch (const UserError& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }

    if (m->help_agent) {
        std::cout << m->app.help() << "\n" << agent_guide;
        return 0;
    }

    return 0;
}

} // namespace den

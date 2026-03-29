// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "cli.h"
#include "install.h"
#include "shell.h"

#include "../core/config.h"
#include "../core/error.h"
#include "../daemon/daemon.h"
#include "../doctor/doctor.h"
#include "../env/environment.h"
#include "../env/manifest.h"
#include "../index/index.h"
#include "../migrate/migrate.h"
#include "../platform/platform.h"
#include "../settings/settings.h"
#include "../store/store.h"

#include <CLI11.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace den {

namespace {

const char* agent_guide = "See agents-guide.md for the full agent guide.\n";

void stub(const std::string& command) {
    SPDLOG_INFO("{}: not yet implemented", command);
}

} // namespace

struct Cli::M {
    CLI::App app{"den — universal development environment manager", "den"};

    // Top-level flags.
    bool help_agent = false;

    // install
    std::vector<std::string> install_names;
    bool build_from_source = false;

    // uninstall
    std::vector<std::string> uninstall_names;

    // upgrade
    std::vector<std::string> upgrade_names;

    // info
    std::string info_name;

    // search
    std::string search_query;

    // deps
    std::string deps_name;
    bool deps_tree = false;

    // use
    std::string use_name;
    std::string use_version;

    // env subcommands
    std::string env_create_name;
    std::string env_remove_name;
    std::string env_use_name;

    // init
    std::string init_shell;

    // set
    std::string set_key;
    std::string set_value;

    // services
    std::vector<std::string> service_names;

    void setup();
};

void Cli::M::setup() {
    app.set_version_flag("--version", std::string(DEN_VERSION));
    app.require_subcommand(0, 1);

    app.add_flag("--help-agent", help_agent, "Print help text and agent guide");

    // --- install ---
    auto* install = app.add_subcommand("install", "Install packages");
    install->add_option("names", install_names, "Package names to install")->required();
    install->add_flag("-s,--build-from-source", build_from_source,
                      "Build from source instead of pouring a bottle");
    install->callback([this] {
        auto cfg = Config::detect();
        auto idx = load_index(cfg.cache / "index.json");
        if (idx.packages.empty()) {
            SPDLOG_ERROR("package index is empty — run `den update` first");
            return;
        }
        install_packages(cfg, idx, install_names);
    });

    // --- uninstall ---
    auto* uninstall = app.add_subcommand("uninstall", "Uninstall packages");
    uninstall->add_option("names", uninstall_names, "Package names to uninstall")->required();
    uninstall->callback([this] {
        auto cfg = Config::detect();
        uninstall_packages(cfg, uninstall_names);
    });

    // --- upgrade ---
    auto* upgrade = app.add_subcommand("upgrade", "Upgrade installed packages");
    upgrade->add_option("names", upgrade_names, "Package names to upgrade (all if empty)");
    upgrade->callback([this] {
        auto cfg = Config::detect();
        auto idx = load_index(cfg.cache / "index.json");
        if (idx.packages.empty()) {
            SPDLOG_ERROR("package index is empty — run `den update` first");
            return;
        }
        upgrade_packages(cfg, idx, upgrade_names);
    });

    // --- update ---
    auto* update = app.add_subcommand("update", "Refresh the package index");
    update->callback([] {
        auto cfg = Config::detect();
        auto idx = fetch_and_transform(cfg);
        auto cache_path = cfg.cache / "index.json";
        save_index(idx, cache_path);
        std::cout << "Updated index: " << idx.packages.size() << " packages loaded.\n";
    });

    // --- list ---
    auto* list = app.add_subcommand("list", "List installed packages");
    list->callback([] {
        auto cfg = Config::detect();
        auto installed = list_installed(cfg.store);
        if (installed.empty()) {
            std::cout << "No packages installed.\n";
            return;
        }
        // Find widest name for column alignment.
        size_t max_name = 0;
        for (const auto& pkg : installed) {
            max_name = std::max(max_name, pkg.name.size());
        }
        for (const auto& pkg : installed) {
            std::cout << std::left << std::setw(static_cast<int>(max_name + 2)) << pkg.name
                      << pkg.version << "\n";
        }
    });

    // --- info ---
    auto* info = app.add_subcommand("info", "Show package info");
    info->add_option("name", info_name, "Package name")->required();
    info->callback([this] {
        auto cfg = Config::detect();
        auto idx = load_index(cfg.cache / "index.json");
        const auto* pkg = idx.find(info_name);
        if (!pkg) {
            std::cerr << "Package not found: " << info_name << "\n";
            std::cerr << "Run 'den update' to refresh the index.\n";
            return;
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
        if (idx.packages.empty()) {
            std::cerr << "Index is empty. Run 'den update' first.\n";
            return;
        }
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
    deps->add_option("name", deps_name, "Package name")->required();
    deps->add_flag("--tree", deps_tree, "Show dependency tree");
    deps->callback([this] {
        auto cfg = Config::detect();
        auto idx = load_index(cfg.cache / "index.json");
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
    auto* cleanup = app.add_subcommand("cleanup", "Remove old versions and cache files");
    cleanup->callback([this] { stub("cleanup"); });

    // --- autoremove ---
    auto* autoremove = app.add_subcommand("autoremove", "Remove unused dependencies");
    autoremove->callback([this] { stub("autoremove"); });

    // --- doctor ---
    auto* doctor_cmd = app.add_subcommand("doctor", "Check system for potential problems");
    doctor_cmd->callback([] {
        auto cfg = Config::detect();
        auto findings = doctor(cfg);
        if (findings.empty()) {
            std::cout << "Your system is ready to brew.\n";
        }
    });

    // --- config ---
    auto* config = app.add_subcommand("config", "Show detected configuration");
    config->callback([] {
        auto cfg = Config::detect();
        std::cout << "den_home:          " << cfg.den_home.string() << "\n"
                  << "store:             " << cfg.store.string() << "\n"
                  << "cache:             " << cfg.cache.string() << "\n"
                  << "homebrew_prefix:   " << cfg.homebrew_prefix.string() << "\n"
                  << "homebrew_cellar:   " << cfg.homebrew_cellar.string() << "\n"
                  << "arch:              " << to_string(cfg.arch) << "\n"
                  << "macos_version:     "
                  << (cfg.macos_version ? cfg.macos_version->to_string() : "n/a") << "\n";
    });

    // --- env ---
    auto* env = app.add_subcommand("env", "Manage environments");
    env->require_subcommand(1);

    auto* env_create = env->add_subcommand("create", "Create an environment");
    env_create->add_option("name", env_create_name, "Environment name")->required();
    env_create->callback([this] {
        auto cfg = Config::detect();
        write_manifest(cfg.den_home, env_create_name, Manifest{});
        std::cout << "Created environment '" << env_create_name << "'.\n";
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
    env_freeze->callback([this] { stub("env freeze"); });

    // --- use ---
    auto* use = app.add_subcommand("use", "Switch active version of a package");
    use->add_option("name", use_name, "Package name")->required();
    use->add_option("version", use_version, "Version to activate")->required();
    use->callback([this] { stub("use"); });

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

    // --- status ---
    auto* status = app.add_subcommand("status", "Show environment status");
    status->callback([this] { stub("status"); });

    // --- set ---
    auto* set = app.add_subcommand("set", "Set a configuration value");
    set->add_option("key", set_key, "Setting key")->required();
    set->add_option("value", set_value, "Setting value")->required();
    set->callback([this] {
        auto cfg = Config::detect();
        set_setting(cfg.den_home, set_key, set_value);
        std::cout << set_key << " = " << set_value << "\n";
    });

    // --- settings ---
    auto* settings_cmd = app.add_subcommand("settings", "Show all configuration settings");
    settings_cmd->callback([] {
        auto cfg = Config::detect();
        std::cout << display_all_settings(cfg.den_home) << "\n";
    });

    // --- migrate ---
    auto* migrate = app.add_subcommand("migrate", "Migrate from Homebrew Cellar");
    migrate->callback([] {
        auto cfg = Config::detect();
        migrate_from_homebrew(cfg, {});
    });

    // --- daemon ---
    auto* daemon = app.add_subcommand("daemon", "Manage the background daemon");
    daemon->require_subcommand(1);

    auto* daemon_run = daemon->add_subcommand("run", "Run daemon foreground");
    daemon_run->callback([] {
        auto cfg = Config::detect();
        run_daemon(cfg);
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
        if (state.last_check) {
            auto ago = now_secs() - *state.last_check;
            std::cout << "Last check: " << ago << "s ago\n";
        } else {
            std::cout << "Last check: never\n";
        }

        if (state.pending.empty()) {
            std::cout << "Pending upgrades: none\n";
        } else {
            std::cout << "Pending upgrades:\n";
            for (const auto& p : state.pending)
                std::cout << "  " << p.name << " " << p.installed << " -> " << p.available << "\n";
        }

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
        auto exe = fs::canonical("/proc/self/exe").string();
        // Fall back to argv[0] approximation on macOS.
        auto plist = launchd_plist(fs::path(exe), cfg.den_home);
        auto home = fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp");
        auto plist_path = home / "Library" / "LaunchAgents" / "dev.den.daemon.plist";
        fs::create_directories(plist_path.parent_path());
        {
            std::ofstream f(plist_path);
            f << plist;
        }
        auto rc = std::system(("launchctl load -w " + plist_path.string()).c_str());
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
            std::system(("launchctl unload -w " + plist_path.string()).c_str());
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
        if (idx.packages.empty()) {
            std::cerr << "Index is empty. Run 'den update' first.\n";
            return;
        }
        auto installed = list_installed(cfg.store);
        if (installed.empty()) {
            std::cout << "No packages installed.\n";
            return;
        }
        size_t count = 0;
        size_t max_name = 0;
        // Pre-scan for column width.
        for (const auto& pkg : installed) {
            max_name = std::max(max_name, pkg.name.size());
        }
        for (const auto& pkg : installed) {
            const auto* index_pkg = idx.find(pkg.name);
            if (index_pkg && index_pkg->version != pkg.version) {
                std::cout << std::left << std::setw(static_cast<int>(max_name + 2)) << pkg.name
                          << pkg.version << " -> " << index_pkg->version << "\n";
                ++count;
            }
        }
        if (count == 0) {
            std::cout << "All packages are up to date.\n";
        }
    });

    // --- services ---
    auto* services = app.add_subcommand("services", "Manage package services");
    services->require_subcommand(1);

    auto* svc_list = services->add_subcommand("list", "List running services");
    svc_list->callback([this] { stub("services list"); });

    auto* svc_start = services->add_subcommand("start", "Start services");
    svc_start->add_option("names", service_names, "Service names");
    svc_start->callback([this] { stub("services start"); });

    auto* svc_stop = services->add_subcommand("stop", "Stop services");
    svc_stop->add_option("names", service_names, "Service names");
    svc_stop->callback([this] { stub("services stop"); });

    auto* svc_restart = services->add_subcommand("restart", "Restart services");
    svc_restart->add_option("names", service_names, "Service names");
    svc_restart->callback([this] { stub("services restart"); });
}

Cli::Cli() : m(std::make_unique<M>()) {
    m->setup();
}

Cli::~Cli() = default;

int Cli::run(int argc, char** argv) {
    try {
        m->app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return m->app.exit(e);
    }

    if (m->help_agent) {
        std::cout << m->app.help() << "\n" << agent_guide;
        return 0;
    }

    return 0;
}

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "cli.h"

#include "../core/config.h"
#include "../core/error.h"

#include <CLI11.hpp>
#include <spdlog/spdlog.h>

#include <iostream>
#include <string>
#include <vector>

namespace den {

namespace {

const char* agent_guide =
    "See agents-guide.md for the full agent guide.\n";

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

    app.add_flag("--help-agent", help_agent,
                 "Print help text and agent guide");

    // --- install ---
    auto* install = app.add_subcommand("install", "Install packages");
    install->add_option("names", install_names, "Package names to install")
        ->required();
    install->add_flag("-s,--build-from-source", build_from_source,
                      "Build from source instead of pouring a bottle");
    install->callback([this] { stub("install"); });

    // --- uninstall ---
    auto* uninstall = app.add_subcommand("uninstall", "Uninstall packages");
    uninstall->add_option("names", uninstall_names, "Package names to uninstall")
        ->required();
    uninstall->callback([this] { stub("uninstall"); });

    // --- upgrade ---
    auto* upgrade = app.add_subcommand("upgrade",
                                       "Upgrade installed packages");
    upgrade->add_option("names", upgrade_names,
                        "Package names to upgrade (all if empty)");
    upgrade->callback([this] { stub("upgrade"); });

    // --- update ---
    auto* update = app.add_subcommand("update", "Refresh the package index");
    update->callback([this] { stub("update"); });

    // --- list ---
    auto* list = app.add_subcommand("list", "List installed packages");
    list->callback([this] { stub("list"); });

    // --- info ---
    auto* info = app.add_subcommand("info", "Show package info");
    info->add_option("name", info_name, "Package name")->required();
    info->callback([this] { stub("info"); });

    // --- search ---
    auto* search = app.add_subcommand("search", "Search for packages");
    search->add_option("query", search_query, "Search query")->required();
    search->callback([this] { stub("search"); });

    // --- deps ---
    auto* deps = app.add_subcommand("deps", "Show package dependencies");
    deps->add_option("name", deps_name, "Package name")->required();
    deps->add_flag("--tree", deps_tree, "Show dependency tree");
    deps->callback([this] { stub("deps"); });

    // --- cleanup ---
    auto* cleanup = app.add_subcommand("cleanup",
                                       "Remove old versions and cache files");
    cleanup->callback([this] { stub("cleanup"); });

    // --- autoremove ---
    auto* autoremove = app.add_subcommand("autoremove",
                                          "Remove unused dependencies");
    autoremove->callback([this] { stub("autoremove"); });

    // --- doctor ---
    auto* doctor = app.add_subcommand("doctor",
                                      "Check system for potential problems");
    doctor->callback([this] { stub("doctor"); });

    // --- config ---
    auto* config = app.add_subcommand("config",
                                      "Show detected configuration");
    config->callback([] {
        auto cfg = Config::detect();
        std::cout << "den_home:          " << cfg.den_home.string() << "\n"
                  << "store:             " << cfg.store.string() << "\n"
                  << "cache:             " << cfg.cache.string() << "\n"
                  << "homebrew_prefix:   " << cfg.homebrew_prefix.string() << "\n"
                  << "homebrew_cellar:   " << cfg.homebrew_cellar.string() << "\n"
                  << "arch:              " << to_string(cfg.arch) << "\n"
                  << "macos_version:     "
                  << (cfg.macos_version ? cfg.macos_version->to_string()
                                        : "n/a")
                  << "\n";
    });

    // --- env ---
    auto* env = app.add_subcommand("env", "Manage environments");
    env->require_subcommand(1);

    auto* env_create = env->add_subcommand("create", "Create an environment");
    env_create->add_option("name", env_create_name, "Environment name")
        ->required();
    env_create->callback([this] { stub("env create"); });

    auto* env_list = env->add_subcommand("list", "List environments");
    env_list->callback([this] { stub("env list"); });

    auto* env_remove = env->add_subcommand("remove", "Remove an environment");
    env_remove->add_option("name", env_remove_name, "Environment name")
        ->required();
    env_remove->callback([this] { stub("env remove"); });

    auto* env_use = env->add_subcommand("use", "Switch to an environment");
    env_use->add_option("name", env_use_name, "Environment name")->required();
    env_use->callback([this] { stub("env use"); });

    auto* env_show = env->add_subcommand("show", "Show active environment");
    env_show->callback([this] { stub("env show"); });

    auto* env_freeze = env->add_subcommand("freeze",
                                           "Export environment as lockfile");
    env_freeze->callback([this] { stub("env freeze"); });

    // --- use ---
    auto* use = app.add_subcommand("use",
                                   "Switch active version of a package");
    use->add_option("name", use_name, "Package name")->required();
    use->add_option("version", use_version, "Version to activate")->required();
    use->callback([this] { stub("use"); });

    // --- init ---
    auto* init = app.add_subcommand("init", "Print shell init script");
    init->add_option("--shell", init_shell,
                     "Shell type (bash, zsh, fish)");
    init->callback([this] { stub("init"); });

    // --- status ---
    auto* status = app.add_subcommand("status", "Show environment status");
    status->callback([this] { stub("status"); });

    // --- set ---
    auto* set = app.add_subcommand("set", "Set a configuration value");
    set->add_option("key", set_key, "Setting key")->required();
    set->add_option("value", set_value, "Setting value")->required();
    set->callback([this] { stub("set"); });

    // --- settings ---
    auto* settings = app.add_subcommand("settings",
                                        "Show all configuration settings");
    settings->callback([this] { stub("settings"); });

    // --- migrate ---
    auto* migrate = app.add_subcommand("migrate",
                                       "Migrate from Homebrew Cellar");
    migrate->callback([this] { stub("migrate"); });

    // --- daemon ---
    auto* daemon = app.add_subcommand("daemon", "Manage the background daemon");
    daemon->require_subcommand(1);

    auto* daemon_run = daemon->add_subcommand("run", "Run daemon foreground");
    daemon_run->callback([this] { stub("daemon run"); });

    auto* daemon_stop = daemon->add_subcommand("stop", "Stop the daemon");
    daemon_stop->callback([this] { stub("daemon stop"); });

    auto* daemon_status = daemon->add_subcommand("status",
                                                 "Show daemon status");
    daemon_status->callback([this] { stub("daemon status"); });

    auto* daemon_install = daemon->add_subcommand("install",
                                                  "Install daemon service");
    daemon_install->callback([this] { stub("daemon install"); });

    auto* daemon_uninstall = daemon->add_subcommand("uninstall",
                                                    "Uninstall daemon service");
    daemon_uninstall->callback([this] { stub("daemon uninstall"); });

    // --- outdated ---
    auto* outdated = app.add_subcommand("outdated",
                                        "List packages with updates available");
    outdated->callback([this] { stub("outdated"); });

    // --- services ---
    auto* services = app.add_subcommand("services", "Manage package services");
    services->require_subcommand(1);

    auto* svc_list = services->add_subcommand("list",
                                              "List running services");
    svc_list->callback([this] { stub("services list"); });

    auto* svc_start = services->add_subcommand("start", "Start services");
    svc_start->add_option("names", service_names, "Service names");
    svc_start->callback([this] { stub("services start"); });

    auto* svc_stop = services->add_subcommand("stop", "Stop services");
    svc_stop->add_option("names", service_names, "Service names");
    svc_stop->callback([this] { stub("services stop"); });

    auto* svc_restart = services->add_subcommand("restart",
                                                 "Restart services");
    svc_restart->add_option("names", service_names, "Service names");
    svc_restart->callback([this] { stub("services restart"); });
}

Cli::Cli() : m(std::make_unique<M>()) { m->setup(); }

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

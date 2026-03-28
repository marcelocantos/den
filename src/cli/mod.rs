// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

mod daemon_cmd;
mod env_cmd;
mod install;
mod migrate;
mod query;
mod services;
mod shell;
mod tap_cmd;

use clap::{Parser, Subcommand};

use crate::api::FormulaIndex;
use crate::config::Config;
use crate::{env, manifest, settings, tap};

use daemon_cmd::DaemonCommand;
use env_cmd::EnvCommand;
use services::ServiceCommand;

const AGENT_GUIDE: &str = include_str!("../../agents-guide.md");

#[derive(Parser)]
#[command(
    name = "den",
    version,
    about = "A universal development environment manager"
)]
pub struct Cli {
    /// Print help text and agent guide
    #[arg(long)]
    help_agent: bool,

    #[command(subcommand)]
    command: Option<Command>,
}

#[derive(Subcommand)]
enum Command {
    /// Install a formula or cask
    Install {
        /// Formula or cask name(s)
        names: Vec<String>,
        /// Build from source instead of pouring a bottle
        #[arg(long, short = 's')]
        build_from_source: bool,
        /// Install a cask
        #[arg(long)]
        cask: bool,
    },
    /// Uninstall a formula or cask
    Uninstall {
        /// Formula or cask name(s)
        names: Vec<String>,
    },
    /// Upgrade outdated formulae and casks
    Upgrade {
        /// Specific formula or cask name(s) (default: all outdated)
        names: Vec<String>,
    },
    /// Fetch latest Homebrew and tap data
    Update,
    /// List installed formulae and casks
    List {
        /// Formula or cask name(s) to list (default: all)
        names: Vec<String>,
    },
    /// Show info about a formula or cask
    Info {
        /// Formula or cask name
        name: String,
    },
    /// Search for formulae and casks
    Search {
        /// Search text or regex
        text: String,
    },
    /// Show dependencies for a formula
    Deps {
        /// Formula name
        name: String,
        /// Show full dependency tree
        #[arg(long)]
        tree: bool,
    },
    /// Manage taps (third-party repositories)
    Tap {
        /// Tap name (user/repo) to add, or empty to list
        name: Option<String>,
    },
    /// Remove a tap
    Untap {
        /// Tap name (user/repo)
        name: String,
    },
    /// Symlink a keg into the prefix
    Link {
        /// Formula name
        name: String,
        /// Overwrite existing files
        #[arg(long)]
        overwrite: bool,
    },
    /// Remove symlinks for a keg
    Unlink {
        /// Formula name
        name: String,
    },
    /// Remove old versions and cache files
    Cleanup,
    /// Remove unneeded dependencies
    Autoremove,
    /// Check system for potential problems
    Doctor,
    /// Show den configuration
    Config,
    /// Manage environments
    Env {
        #[command(subcommand)]
        command: Option<EnvCommand>,
    },
    /// Switch active version of a package in the active environment
    Use {
        /// Package name with optional version (e.g. tree=2.3.1)
        name: String,
    },
    /// Print shell integration for eval (add `eval "$(den init)"` to .zshrc)
    Init {
        /// Shell type (default: zsh)
        #[arg(long, default_value = "zsh")]
        shell: String,
    },
    /// Show pending upgrades and environment status
    Status,
    /// Configure den settings
    Set {
        /// Setting name (e.g. daemon.auto_upgrade)
        key: String,
        /// Setting value
        value: String,
    },
    /// Show current settings
    Settings,
    /// Import existing Homebrew Cellar into den
    Migrate,
    /// Manage the background daemon
    Daemon {
        #[command(subcommand)]
        command: DaemonCommand,
    },
    /// List packages with available upgrades
    Outdated,
    /// Manage background services
    Services {
        #[command(subcommand)]
        command: Option<ServiceCommand>,
    },
}

fn make_client() -> anyhow::Result<reqwest::Client> {
    Ok(reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(300))
        .build()?)
}

impl Cli {
    pub async fn run(self) -> anyhow::Result<()> {
        if self.help_agent {
            let mut cmd = <Self as clap::CommandFactory>::command();
            cmd.print_help()?;
            println!("\n\n{AGENT_GUIDE}");
            return Ok(());
        }

        let config = Config::detect()?;

        match self.command {
            None => {
                let s = settings::read(&config.den_home);
                if !manifest::manifest_exists(&config.den_home, "/")
                    && config.cellar.is_dir()
                    && !s.migration_declined
                {
                    println!("den {}", env!("CARGO_PKG_VERSION"));
                    println!("A universal development environment manager");
                    println!();
                    println!("Homebrew detected at {}.", config.prefix.display());
                    print!("Would you like to import your Homebrew packages? [Y/n] ");
                    use std::io::Write;
                    std::io::stdout().flush()?;

                    let mut input = String::new();
                    std::io::stdin().read_line(&mut input)?;
                    let answer = input.trim().to_lowercase();

                    if answer.is_empty() || answer == "y" || answer == "yes" {
                        migrate::migrate_cellar(&config)?;
                    } else {
                        let mut s = s;
                        s.migration_declined = true;
                        settings::write(&config.den_home, &s)?;
                        println!("OK. Run `den migrate` if you change your mind.");
                    }
                } else {
                    println!("den {}", env!("CARGO_PKG_VERSION"));
                    println!("A universal development environment manager");
                    println!();
                    println!("Run `den --help` for usage.");
                }
                Ok(())
            }
            Some(Command::Config) => {
                println!("{config}");
                Ok(())
            }
            Some(Command::Install {
                names,
                build_from_source,
                cask: is_cask,
            }) => {
                if build_from_source {
                    anyhow::bail!("source builds not yet supported");
                }
                if names.is_empty() {
                    anyhow::bail!("no formula specified");
                }

                let client = make_client()?;
                let active = env::active_env_path(&config.den_home);

                if is_cask {
                    #[cfg(not(target_os = "macos"))]
                    anyhow::bail!("cask installation is only supported on macOS");
                    #[cfg(target_os = "macos")]
                    for name in &names {
                        install::install_cask_cmd(&client, &config, &active, name).await?;
                    }
                } else {
                    // Separate tap-qualified names from regular names.
                    let mut regular_names = Vec::new();
                    let mut tap_refs = Vec::new();
                    for name in &names {
                        if let Some((tap_name, formula_name)) = tap::parse_tap_formula_ref(name) {
                            tap_refs.push((tap_name, formula_name));
                        } else {
                            regular_names.push(name.as_str());
                        }
                    }

                    // Install tap formulae.
                    for (tap_name, formula_name) in &tap_refs {
                        tap_cmd::install_tap_formula(
                            &client,
                            &config,
                            &active,
                            tap_name,
                            formula_name,
                        )
                        .await?;
                    }

                    // Install regular formulae.
                    if !regular_names.is_empty() {
                        let cache_dir = config.den_home.join("cache");
                        println!("==> Loading formula index...");
                        let index = FormulaIndex::load(&client, &cache_dir).await?;
                        println!("  {} formulae", index.len());
                        for name in &regular_names {
                            install::install_formula(&client, &config, &index, &active, name)
                                .await?;
                        }
                    }
                }
                Ok(())
            }
            Some(Command::Uninstall { names }) => {
                if names.is_empty() {
                    anyhow::bail!("no formula specified");
                }
                let active = env::active_env_path(&config.den_home);
                for name in &names {
                    install::uninstall_package(&config, &active, name)?;
                }
                Ok(())
            }
            Some(Command::Upgrade { names }) => {
                let client = make_client()?;
                let active = env::active_env_path(&config.den_home);
                let cache_dir = config.den_home.join("cache");
                println!("==> Loading formula index...");
                let index = FormulaIndex::load(&client, &cache_dir).await?;
                install::upgrade_packages(&client, &config, &index, &active, &names).await
            }
            Some(Command::Update) => {
                let client = make_client()?;
                let cache_dir = config.den_home.join("cache");
                println!("==> Refreshing formula index...");
                let index = FormulaIndex::refresh(&client, &cache_dir).await?;
                println!("  {} formulae indexed.", index.len());
                Ok(())
            }
            Some(Command::Outdated) => {
                let client = make_client()?;
                let active = env::active_env_path(&config.den_home);
                let cache_dir = config.den_home.join("cache");
                println!("==> Loading formula index...");
                let index = FormulaIndex::load(&client, &cache_dir).await?;
                install::show_outdated(&config, &index, &active)
            }
            Some(Command::Migrate) => migrate::migrate_cellar(&config),
            Some(Command::Daemon { command }) => {
                daemon_cmd::run_daemon_command(&config, command).await
            }
            Some(Command::List { names }) => query::list_packages(&config, &names),
            Some(Command::Use { name }) => query::use_version(&config, &name),
            Some(Command::Init { shell }) => {
                shell::print_shell_init(&config, &shell);
                Ok(())
            }
            Some(Command::Env { command }) => env_cmd::run_env_command(&config, command),
            Some(Command::Info { name }) => {
                let client = make_client()?;
                let cache_dir = config.den_home.join("cache");
                let index = FormulaIndex::load(&client, &cache_dir).await?;
                query::show_info(&config, &index, &name)
            }
            Some(Command::Search { text }) => {
                let client = make_client()?;
                let cache_dir = config.den_home.join("cache");
                let index = FormulaIndex::load(&client, &cache_dir).await?;
                query::search_packages(&index, &text)
            }
            Some(Command::Deps { name, tree }) => {
                let client = make_client()?;
                let cache_dir = config.den_home.join("cache");
                let index = FormulaIndex::load(&client, &cache_dir).await?;
                query::show_deps(&index, &name, tree)
            }
            Some(Command::Cleanup) => query::cleanup(&config),
            Some(Command::Autoremove) => {
                let active = env::active_env_path(&config.den_home);
                install::autoremove(&config, &active)
            }
            Some(Command::Services { command }) => services::run_services_command(&config, command),
            Some(Command::Set { key, value }) => {
                settings::set(&config.den_home, &key, &value)?;
                println!("{key} = {value}");
                Ok(())
            }
            Some(Command::Settings) => {
                println!("{}", settings::display_all(&config.den_home));
                Ok(())
            }
            Some(Command::Tap { name }) => {
                match name {
                    Some(tap_name) => tap::add_tap(&config.den_home, &tap_name)?,
                    None => {
                        let taps = tap::list_taps(&config.den_home)?;
                        if taps.is_empty() {
                            println!("No taps installed.");
                        } else {
                            for t in &taps {
                                println!("{t}");
                            }
                        }
                    }
                }
                Ok(())
            }
            Some(Command::Untap { name }) => {
                tap::remove_tap(&config.den_home, &name)?;
                Ok(())
            }
            Some(Command::Link { .. }) => {
                anyhow::bail!("'link' is not yet implemented")
            }
            Some(Command::Unlink { .. }) => {
                anyhow::bail!("'unlink' is not yet implemented")
            }
            Some(Command::Doctor) => {
                anyhow::bail!("'doctor' is not yet implemented")
            }
            Some(Command::Status) => {
                anyhow::bail!("'status' is not yet implemented")
            }
        }
    }
}

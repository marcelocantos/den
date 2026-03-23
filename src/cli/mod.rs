// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use clap::{Parser, Subcommand};

use crate::config::Config;

#[derive(Parser)]
#[command(name = "den", version, about = "A universal development environment manager")]
pub struct Cli {
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
    Cleanup {
        /// Formula or cask name(s) (default: all)
        names: Vec<String>,
    },
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
    /// Switch active version of a package
    Use {
        /// Package name with version (e.g. python@3.11)
        name: String,
    },
    /// Show pending upgrades and environment status
    Status,
}

#[derive(Subcommand)]
enum EnvCommand {
    /// Create a new environment
    Create {
        /// Environment name
        name: String,
        /// Parent environment to inherit from
        #[arg(long)]
        from: Option<String>,
    },
    /// List all environments
    List,
    /// Remove an environment
    Remove {
        /// Environment name
        name: String,
    },
    /// Activate an environment in the current shell
    Use {
        /// Environment name
        name: String,
    },
    /// Export environment as a lockfile
    Freeze,
}

impl Cli {
    pub async fn run(self) -> anyhow::Result<()> {
        let config = Config::detect()?;

        match self.command {
            None => {
                println!("den {}", env!("CARGO_PKG_VERSION"));
                println!("A universal development environment manager");
                println!();
                println!("Run `den --help` for usage.");
                Ok(())
            }
            Some(Command::Config) => {
                println!("{config}");
                Ok(())
            }
            Some(cmd) => {
                let _ = (cmd, config);
                anyhow::bail!("command not yet implemented")
            }
        }
    }
}

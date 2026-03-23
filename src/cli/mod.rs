// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use clap::{Parser, Subcommand};

use crate::config::Config;
use crate::{api, bottle, env, formula, keg, link, platform, tab};

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
    /// Configure den settings
    Set {
        /// Setting name (e.g. auto-upgrade)
        key: String,
        /// Setting value
        value: String,
    },
    /// Show current settings
    Settings,
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
            Some(Command::Install {
                names,
                build_from_source,
                cask,
            }) => {
                if build_from_source {
                    anyhow::bail!("source builds not yet supported");
                }
                if cask {
                    anyhow::bail!("cask installs not yet supported");
                }
                if names.is_empty() {
                    anyhow::bail!("no formula specified");
                }

                let client = reqwest::Client::new();
                let env_path = env::default_env(&config.den_home)?;

                for name in &names {
                    install_formula(&client, &config, &env_path, name).await?;
                }
                Ok(())
            }
            Some(Command::List { names }) => {
                list_packages(&config, &names)
            }
            Some(Command::Use { name }) => {
                use_version(&config, &name)
            }
            Some(Command::Env { command }) => {
                match command {
                    Some(EnvCommand::List) | None => {
                        let envs = env::list_envs(&config.den_home)?;
                        if envs.is_empty() {
                            println!("No environments. Create one with `den env create <name>`.");
                        } else {
                            for name in &envs {
                                println!("{name}");
                            }
                        }
                        Ok(())
                    }
                    Some(EnvCommand::Create { name, from: _ }) => {
                        let path = env::ensure_env(&config.den_home, &name)?;
                        println!("Created environment '{}' at {}", name, path.display());
                        Ok(())
                    }
                    Some(cmd) => {
                        let _ = cmd;
                        anyhow::bail!("env subcommand not yet implemented")
                    }
                }
            }
            Some(_) => {
                anyhow::bail!("command not yet implemented")
            }
        }
    }
}

async fn install_formula(
    client: &reqwest::Client,
    config: &Config,
    env_path: &std::path::Path,
    name: &str,
) -> anyhow::Result<()> {
    println!("==> Fetching {name}...");
    let info = api::fetch_formula(client, name).await?;

    let pkg_version = info.pkg_version();
    let keg_path = config.cellar.join(&info.name).join(&pkg_version);

    // Check if already installed.
    if keg_path.is_dir() {
        println!("{} {} is already installed.", info.name, pkg_version);
        // Still link if not linked.
        if link::linked_version(env_path, &info.name).is_none() {
            println!("==> Linking {} {}...", info.name, pkg_version);
            let created = link::link_keg(&keg_path, env_path)?;
            link::record_linked_version(env_path, &info.name, &pkg_version)?;
            println!("  {} symlinks created", created.len());
        }
        return Ok(());
    }

    // Find the right bottle for this platform.
    let macos = config
        .macos_version
        .as_ref()
        .ok_or_else(|| anyhow::anyhow!("cannot determine macOS version"))?;

    let available_tags: Vec<String> = info.bottle.stable.files.keys().cloned().collect();
    let tag = platform::best_bottle_tag(config.arch, macos, &available_tags)
        .ok_or_else(|| {
            anyhow::anyhow!(
                "no bottle available for {} on {} {}",
                info.name,
                config.arch,
                macos
            )
        })?;

    let bottle_file = &info.bottle.stable.files[&tag];
    let ghcr_path = formula::ghcr_path(&info.name);

    println!(
        "==> Downloading {} {} ({})...",
        info.name, pkg_version, tag
    );
    let bottle_data = bottle::download_bottle(client, bottle_file, &ghcr_path).await?;
    println!("  {} bytes, SHA256 verified", bottle_data.len());

    println!("==> Pouring {} {}...", info.name, pkg_version);
    let poured_keg = bottle::pour_bottle(&bottle_data, &config.cellar)?;
    println!("  Poured to {}", poured_keg.display());

    // Write a tab.
    tab::write_tab(&poured_keg, &config.arch.to_string())?;

    // Link into the default environment.
    println!("==> Linking {} {}...", info.name, pkg_version);
    let created = link::link_keg(&poured_keg, env_path)?;
    link::record_linked_version(env_path, &info.name, &pkg_version)?;
    println!("  {} symlinks created", created.len());

    if let Some(ref caveats) = info.caveats {
        println!("==> Caveats");
        println!("{caveats}");
    }

    println!(
        "==> {} {} installed successfully.",
        info.name, pkg_version
    );
    Ok(())
}

fn list_packages(config: &Config, names: &[String]) -> anyhow::Result<()> {
    let env_path = config.den_home.join("envs").join("default");
    let kegs = keg::list_installed(&config.cellar)?;

    if kegs.is_empty() {
        println!("No packages installed.");
        return Ok(());
    }

    let filtered: Vec<_> = if names.is_empty() {
        kegs
    } else {
        kegs.into_iter()
            .filter(|k| names.iter().any(|n| k.name == *n))
            .collect()
    };

    for keg in &filtered {
        let linked = link::linked_version(&env_path, &keg.name);
        let marker = match linked {
            Some(ref v) if v == &keg.version => " (active)",
            _ => "",
        };
        println!("{} {}{}", keg.name, keg.version, marker);
    }

    Ok(())
}

fn use_version(config: &Config, name: &str) -> anyhow::Result<()> {
    let env_path = env::default_env(&config.den_home)?;

    // Parse "tree=2.3.1" or "tree" (latest).
    let (formula_name, requested_version) = if let Some((n, v)) = name.split_once('=') {
        (n, Some(v))
    } else {
        (name, None)
    };

    let versions = keg::find_versions(&config.cellar, formula_name)?;
    if versions.is_empty() {
        anyhow::bail!(
            "{formula_name} is not installed. Run `den install {formula_name}` first."
        );
    }

    // Select the target keg.
    let keg = if let Some(req_ver) = requested_version {
        versions
            .iter()
            .find(|k| k.version == req_ver)
            .ok_or_else(|| {
                let available: Vec<_> = versions.iter().map(|k| k.version.as_str()).collect();
                anyhow::anyhow!(
                    "{formula_name} version {req_ver} not found. Available: {}",
                    available.join(", ")
                )
            })?
    } else {
        // Pick the latest.
        versions.last().unwrap()
    };

    // Check if already active.
    let current = link::linked_version(&env_path, formula_name);
    if current.as_deref() == Some(&keg.version) {
        println!("{} {} is already active.", formula_name, keg.version);
        return Ok(());
    }

    // Unlink current version if any.
    if let Some(ref cur_ver) = current {
        let old_keg = config.cellar.join(formula_name).join(cur_ver);
        if old_keg.is_dir() {
            println!("==> Unlinking {} {}...", formula_name, cur_ver);
            link::unlink_keg(&old_keg, &env_path)?;
        }
    }

    // Link the requested version.
    println!("==> Linking {} {}...", formula_name, keg.version);
    let created = link::link_keg(&keg.path, &env_path)?;
    link::record_linked_version(&env_path, formula_name, &keg.version)?;
    println!("  {} symlinks created", created.len());
    println!("==> Now using {} {}.", formula_name, keg.version);
    Ok(())
}

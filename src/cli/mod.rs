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
    /// Print shell integration for eval (add `eval "$(den init)"` to .zshrc)
    Init {
        /// Shell type (default: auto-detect)
        #[arg(long, default_value = "zsh")]
        shell: String,
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
            Some(Command::Init { shell }) => {
                print_shell_init(&config, &shell);
                Ok(())
            }
            Some(Command::Env { command }) => {
                match command {
                    Some(EnvCommand::List) | None => {
                        let envs = env::list_envs(&config.den_home)?;
                        if envs.is_empty() {
                            println!("No environments. Create one with `den env create <name>`.");
                        } else {
                            let active = env::active_env_name(&config.den_home);
                            for name in &envs {
                                let marker = if Some(name.as_str()) == active.as_deref() {
                                    " (active)"
                                } else {
                                    ""
                                };
                                println!("{name}{marker}");
                            }
                        }
                        Ok(())
                    }
                    Some(EnvCommand::Create { name, from: _ }) => {
                        let path = env::ensure_env(&config.den_home, &name)?;
                        println!("Created environment '{}' at {}", name, path.display());
                        Ok(())
                    }
                    Some(EnvCommand::Use { name }) => {
                        // Verify the environment exists.
                        let env_path = config.den_home.join("envs").join(&name);
                        if !env_path.is_dir() {
                            anyhow::bail!(
                                "environment '{}' does not exist. Create it with `den env create {}`.",
                                name, name
                            );
                        }
                        // Write active env marker.
                        env::set_active_env(&config.den_home, &name)?;
                        // Output shell commands to switch PATH.
                        print_env_switch_commands(&config, &name);
                        Ok(())
                    }
                    Some(EnvCommand::Remove { name }) => {
                        let env_path = config.den_home.join("envs").join(&name);
                        if name == "default" {
                            anyhow::bail!("cannot remove the default environment");
                        }
                        if !env_path.is_dir() {
                            anyhow::bail!("environment '{}' does not exist", name);
                        }
                        std::fs::remove_dir_all(&env_path)?;
                        println!("Removed environment '{name}'.");
                        Ok(())
                    }
                    Some(EnvCommand::Freeze) => {
                        anyhow::bail!("env freeze not yet implemented")
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
            let created = link::link_keg(&keg_path, env_path, &info.name)?;
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
    let created = link::link_keg(&poured_keg, env_path, &info.name)?;
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
            link::unlink_keg(&old_keg, &env_path, formula_name)?;
        }
    }

    // Link the requested version.
    println!("==> Linking {} {}...", formula_name, keg.version);
    let created = link::link_keg(&keg.path, &env_path, formula_name)?;
    link::record_linked_version(&env_path, formula_name, &keg.version)?;
    println!("  {} symlinks created", created.len());
    println!("==> Now using {} {}.", formula_name, keg.version);
    Ok(())
}

fn print_shell_init(config: &Config, shell: &str) {
    let den_home = config.den_home.display();
    match shell {
        "zsh" | "bash" => {
            print!(
                r#"# den shell integration
export DEN_HOME="{den_home}"
export DEN_ENV="default"

# Add default environment to PATH (before Homebrew).
export PATH="{den_home}/envs/default/bin:$PATH"

# Shell function wrapping `den env use` so it can modify the current shell.
den() {{
    if [ "$1" = "env" ] && [ "$2" = "use" ] && [ -n "$3" ]; then
        local _den_output
        _den_output="$(command den env use "$3" 2>&1)"
        local _den_rc=$?
        if [ $_den_rc -eq 0 ]; then
            eval "$_den_output"
        else
            echo "$_den_output" >&2
            return $_den_rc
        fi
    else
        command den "$@"
    fi
}}
"#
            );
        }
        "fish" => {
            print!(
                r#"# den shell integration
set -gx DEN_HOME "{den_home}"
set -gx DEN_ENV "default"

# Add default environment to PATH.
fish_add_path --prepend "{den_home}/envs/default/bin"

# Wrapper function for `den env use`.
function den
    if test (count $argv) -ge 3; and test "$argv[1]" = "env"; and test "$argv[2]" = "use"
        set -l output (command den env use $argv[3] 2>&1)
        set -l rc $status
        if test $rc -eq 0
            eval $output
        else
            echo $output >&2
            return $rc
        end
    else
        command den $argv
    end
end
"#
            );
        }
        _ => {
            eprintln!("unsupported shell: {shell}. Use zsh, bash, or fish.");
        }
    }
}

fn print_env_switch_commands(config: &Config, env_name: &str) {
    let den_home = &config.den_home;
    let new_env_bin = den_home.join("envs").join(env_name).join("bin");

    // Output shell commands that the wrapper function will eval.
    // Remove any existing den env bin from PATH, then prepend the new one.
    println!(
        r#"export PATH="$(echo "$PATH" | sed "s|{den_home}/envs/[^:]*bin:||g")"
export PATH="{new_bin}:$PATH"
export DEN_ENV="{env_name}""#,
        den_home = den_home.display(),
        new_bin = new_env_bin.display(),
    );
}

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use clap::{Parser, Subcommand};

use crate::config::Config;
use crate::{api, bottle, env, formula, keg, link, manifest, platform, tab};

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
    /// Switch active version of a package in the active environment
    Use {
        /// Package name with optional version (e.g. python@3.11 or tree=2.3.1)
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
    /// Create a new environment (path-based: / is root, /ml, /work/legacy)
    Create {
        /// Environment path (e.g. /ml, /work/legacy)
        path: String,
    },
    /// List all environments
    List,
    /// Remove an environment
    Remove {
        /// Environment path
        path: String,
    },
    /// Activate an environment in the current shell
    Use {
        /// Environment path (e.g. /, /ml)
        path: String,
    },
    /// Show resolved packages for an environment
    Show {
        /// Environment path (default: active)
        path: Option<String>,
    },
    /// Export resolved environment as a lockfile
    Freeze {
        /// Environment path (default: active)
        path: Option<String>,
    },
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
                let active = env::active_env_path(&config.den_home);

                for name in &names {
                    install_formula(&client, &config, &active, name).await?;
                }
                Ok(())
            }
            Some(Command::List { names }) => list_packages(&config, &names),
            Some(Command::Use { name }) => use_version(&config, &name),
            Some(Command::Init { shell }) => {
                print_shell_init(&config, &shell);
                Ok(())
            }
            Some(Command::Env { command }) => run_env_command(&config, command),
            Some(_) => {
                anyhow::bail!("command not yet implemented")
            }
        }
    }
}

fn run_env_command(config: &Config, command: Option<EnvCommand>) -> anyhow::Result<()> {
    match command {
        Some(EnvCommand::List) | None => {
            let envs = env::list_envs(&config.den_home)?;
            if envs.is_empty() {
                // Ensure root exists.
                manifest::write_manifest(
                    &config.den_home,
                    "/",
                    &manifest::Manifest::default(),
                )?;
                println!("/ (active)");
            } else {
                let active = env::active_env_path(&config.den_home);
                for env_path in &envs {
                    let marker = if *env_path == active {
                        " (active)"
                    } else {
                        ""
                    };
                    // Show inheritance.
                    let parent_info = manifest::parent_path(env_path)
                        .map(|p| format!(" (from {p})"))
                        .unwrap_or_default();
                    println!("{env_path}{parent_info}{marker}");
                }
            }
            Ok(())
        }
        Some(EnvCommand::Create { path }) => {
            let env_path = normalise_env_path(&path);

            // Ensure parent exists (except for root).
            if let Some(parent) = manifest::parent_path(&env_path) {
                if !manifest::manifest_exists(&config.den_home, &parent) {
                    anyhow::bail!(
                        "parent environment '{}' does not exist. Create it first.",
                        parent
                    );
                }
            }

            if manifest::manifest_exists(&config.den_home, &env_path) {
                anyhow::bail!("environment '{}' already exists", env_path);
            }

            manifest::write_manifest(
                &config.den_home,
                &env_path,
                &manifest::Manifest::default(),
            )?;

            // Materialise (inherits from parent).
            let links = env::materialise(&config.den_home, &config.cellar, &env_path)?;
            let slug = manifest::env_slug(&env_path);
            println!(
                "Created environment '{env_path}' ({links} symlinks, dir: {slug})"
            );
            Ok(())
        }
        Some(EnvCommand::Use { path }) => {
            let env_path = normalise_env_path(&path);

            if !manifest::manifest_exists(&config.den_home, &env_path) {
                anyhow::bail!(
                    "environment '{}' does not exist. Create it with `den env create {}`.",
                    env_path,
                    path
                );
            }

            env::set_active_env(&config.den_home, &env_path)?;
            let slug = manifest::env_slug(&env_path);
            print_env_switch_commands(config, &slug);
            Ok(())
        }
        Some(EnvCommand::Show { path }) => {
            let env_path = path
                .map(|p| normalise_env_path(&p))
                .unwrap_or_else(|| env::active_env_path(&config.den_home));

            let resolved = manifest::resolve(&config.den_home, &env_path)?;
            if resolved.is_empty() {
                println!("Environment '{env_path}' has no packages.");
            } else {
                println!("Resolved packages for '{env_path}':");
                for (name, version) in &resolved {
                    // Show which level contributed this package.
                    let source = find_source_env(&config, &env_path, name);
                    let source_info = if source != env_path {
                        format!(" (from {source})")
                    } else {
                        String::new()
                    };
                    println!("  {name} {version}{source_info}");
                }
            }
            Ok(())
        }
        Some(EnvCommand::Remove { path }) => {
            let env_path = normalise_env_path(&path);
            if env_path == "/" {
                anyhow::bail!("cannot remove the root environment");
            }
            if !manifest::manifest_exists(&config.den_home, &env_path) {
                anyhow::bail!("environment '{}' does not exist", env_path);
            }

            // Remove materialised env.
            let env_dir = env::env_dir(&config.den_home, &env_path);
            if env_dir.is_dir() {
                std::fs::remove_dir_all(&env_dir)?;
            }

            // Remove manifest.
            let manifest_dir = config
                .den_home
                .join("manifests")
                .join(env_path.trim_matches('/'));
            if manifest_dir.is_dir() {
                std::fs::remove_dir_all(&manifest_dir)?;
            } else {
                // Single file case.
                let _ = std::fs::remove_file(&manifest_dir);
            }

            println!("Removed environment '{env_path}'.");
            Ok(())
        }
        Some(EnvCommand::Freeze { path }) => {
            let env_path = path
                .map(|p| normalise_env_path(&p))
                .unwrap_or_else(|| env::active_env_path(&config.den_home));

            let resolved = manifest::resolve(&config.den_home, &env_path)?;
            let json = serde_json::to_string_pretty(&resolved)?;
            println!("{json}");
            Ok(())
        }
    }
}

/// Find which environment in the ancestor chain provides a given package.
fn find_source_env(config: &Config, env_path: &str, package: &str) -> String {
    let chain = manifest::ancestor_chain(env_path);
    // Walk from leaf to root to find the most specific provider.
    for ancestor in chain.iter().rev() {
        if let Ok(m) = manifest::read_manifest(&config.den_home, ancestor) {
            if m.packages.contains_key(package) {
                return ancestor.clone();
            }
        }
    }
    env_path.to_string()
}

async fn install_formula(
    client: &reqwest::Client,
    config: &Config,
    active_env: &str,
    name: &str,
) -> anyhow::Result<()> {
    println!("==> Fetching {name}...");
    let info = api::fetch_formula(client, name).await?;

    let pkg_version = info.pkg_version();
    let keg_path = config.cellar.join(&info.name).join(&pkg_version);

    // Download and pour if not in Cellar.
    if !keg_path.is_dir() {
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
        let bottle_data =
            bottle::download_bottle(client, bottle_file, &ghcr_path).await?;
        println!("  {} bytes, SHA256 verified", bottle_data.len());

        println!("==> Pouring {} {}...", info.name, pkg_version);
        let poured_keg = bottle::pour_bottle(&bottle_data, &config.cellar)?;
        println!("  Poured to {}", poured_keg.display());

        tab::write_tab(&poured_keg, &config.arch.to_string())?;
    } else {
        println!("{} {} already in Cellar.", info.name, pkg_version);
    }

    // Add to the active environment's manifest.
    let mut m = manifest::read_manifest(&config.den_home, active_env)?;
    m.packages
        .insert(info.name.clone(), pkg_version.clone());
    manifest::write_manifest(&config.den_home, active_env, &m)?;

    // Re-materialise the environment.
    println!("==> Materialising environment '{active_env}'...");
    let links = env::materialise(&config.den_home, &config.cellar, active_env)?;
    println!("  {links} symlinks");

    if let Some(ref caveats) = info.caveats {
        println!("==> Caveats");
        println!("{caveats}");
    }

    println!(
        "==> {} {} installed to '{active_env}'.",
        info.name, pkg_version
    );
    Ok(())
}

fn list_packages(config: &Config, names: &[String]) -> anyhow::Result<()> {
    let active = env::active_env_path(&config.den_home);
    let resolved = manifest::resolve(&config.den_home, &active)?;
    let env_dir = env::env_dir(&config.den_home, &active);

    if resolved.is_empty() {
        println!("No packages in environment '{active}'.");
        return Ok(());
    }

    let filtered: Vec<_> = if names.is_empty() {
        resolved.iter().collect()
    } else {
        resolved
            .iter()
            .filter(|(k, _)| names.iter().any(|n| k.as_str() == n))
            .collect()
    };

    for (name, version) in &filtered {
        let linked = link::linked_version(&env_dir, name);
        let marker = match linked {
            Some(ref v) if v == *version => " (active)",
            _ => "",
        };
        println!("{name} {version}{marker}");
    }

    Ok(())
}

fn use_version(config: &Config, name: &str) -> anyhow::Result<()> {
    let active = env::active_env_path(&config.den_home);

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

    let keg = if let Some(req_ver) = requested_version {
        versions
            .iter()
            .find(|k| k.version == req_ver)
            .ok_or_else(|| {
                let available: Vec<_> =
                    versions.iter().map(|k| k.version.as_str()).collect();
                anyhow::anyhow!(
                    "{formula_name} version {req_ver} not found. Available: {}",
                    available.join(", ")
                )
            })?
    } else {
        versions.last().unwrap()
    };

    // Update the manifest.
    let mut m = manifest::read_manifest(&config.den_home, &active)?;
    m.packages
        .insert(formula_name.to_string(), keg.version.clone());
    manifest::write_manifest(&config.den_home, &active, &m)?;

    // Re-materialise.
    println!(
        "==> Switching {} to {} in '{active}'...",
        formula_name, keg.version
    );
    let links = env::materialise(&config.den_home, &config.cellar, &active)?;
    println!("  {links} symlinks");
    println!("==> Now using {} {}.", formula_name, keg.version);
    Ok(())
}

fn normalise_env_path(path: &str) -> String {
    if path.is_empty() || path == "/" {
        return "/".to_string();
    }
    let p = if path.starts_with('/') {
        path.to_string()
    } else {
        format!("/{path}")
    };
    // Remove trailing slash.
    p.trim_end_matches('/').to_string()
}

fn print_shell_init(config: &Config, shell: &str) {
    let den_home = config.den_home.display();
    let root_slug = manifest::env_slug("/");
    match shell {
        "zsh" | "bash" => {
            print!(
                r#"# den shell integration
export DEN_HOME="{den_home}"
export DEN_ENV="/"

# Add root environment to PATH (before Homebrew).
export PATH="{den_home}/envs/{root_slug}/bin:$PATH"

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
set -gx DEN_ENV "/"

# Add root environment to PATH.
fish_add_path --prepend "{den_home}/envs/{root_slug}/bin"

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

fn print_env_switch_commands(config: &Config, env_slug: &str) {
    let den_home = &config.den_home;
    let new_env_bin = den_home.join("envs").join(env_slug).join("bin");
    let env_path = manifest::slug_to_path(env_slug);

    println!(
        r#"export PATH="$(echo "$PATH" | sed "s|{den_home}/envs/[^:]*bin:||g")"
export PATH="{new_bin}:$PATH"
export DEN_ENV="{env_path}""#,
        den_home = den_home.display(),
        new_bin = new_env_bin.display(),
    );
}

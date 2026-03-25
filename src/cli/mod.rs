// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use clap::{Parser, Subcommand};

use crate::api::FormulaIndex;
use crate::config::Config;
use crate::{
    bottle, cask, daemon, deps, env, formula, keg, link, manifest, platform, service, settings, tab,
};

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

#[derive(Subcommand)]
enum ServiceCommand {
    /// List all services
    List,
    /// Start a service
    Start {
        /// Service/formula name
        name: String,
    },
    /// Stop a service
    Stop {
        /// Service/formula name
        name: String,
    },
    /// Restart a service
    Restart {
        /// Service/formula name
        name: String,
    },
}

#[derive(Subcommand)]
enum DaemonCommand {
    /// Run the daemon in the foreground (use with launchd or nohup)
    Run,
    /// Stop the running daemon
    Stop,
    /// Show daemon status and pending upgrades
    Status,
    /// Install the launchd plist for auto-start at login
    Install,
    /// Remove the launchd plist
    Uninstall,
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
        if self.help_agent {
            // Print clap help followed by the agent guide.
            let mut cmd = <Self as clap::CommandFactory>::command();
            cmd.print_help()?;
            println!("\n\n{AGENT_GUIDE}");
            return Ok(());
        }

        let config = Config::detect()?;

        match self.command {
            None => {
                // First run: if no root manifest exists and Homebrew is
                // installed, offer to migrate.
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
                        migrate_cellar(&config)?;
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

                let client = reqwest::Client::new();
                let active = env::active_env_path(&config.den_home);

                if is_cask {
                    for name in &names {
                        install_cask_cmd(&client, &config, &active, name).await?;
                    }
                } else {
                    let cache_dir = config.den_home.join("cache");
                    println!("==> Loading formula index...");
                    let index = FormulaIndex::load(&client, &cache_dir).await?;
                    println!("  {} formulae", index.len());
                    for name in &names {
                        install_formula(&client, &config, &index, &active, name).await?;
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
                    uninstall_package(&config, &active, name)?;
                }
                Ok(())
            }
            Some(Command::Upgrade { names }) => {
                let client = reqwest::Client::new();
                let active = env::active_env_path(&config.den_home);
                let cache_dir = config.den_home.join("cache");
                println!("==> Loading formula index...");
                let index = FormulaIndex::load(&client, &cache_dir).await?;
                upgrade_packages(&client, &config, &index, &active, &names).await
            }
            Some(Command::Update) => {
                let client = reqwest::Client::new();
                let cache_dir = config.den_home.join("cache");
                println!("==> Refreshing formula index...");
                let index = FormulaIndex::refresh(&client, &cache_dir).await?;
                println!("  {} formulae indexed.", index.len());
                Ok(())
            }
            Some(Command::Outdated) => {
                let client = reqwest::Client::new();
                let active = env::active_env_path(&config.den_home);
                let cache_dir = config.den_home.join("cache");
                println!("==> Loading formula index...");
                let index = FormulaIndex::load(&client, &cache_dir).await?;
                show_outdated(&config, &index, &active)
            }
            Some(Command::Migrate) => migrate_cellar(&config),
            Some(Command::Daemon { command }) => run_daemon_command(&config, command).await,
            Some(Command::List { names }) => list_packages(&config, &names),
            Some(Command::Use { name }) => use_version(&config, &name),
            Some(Command::Init { shell }) => {
                print_shell_init(&config, &shell);
                Ok(())
            }
            Some(Command::Env { command }) => run_env_command(&config, command),
            Some(Command::Info { name }) => {
                let client = reqwest::Client::new();
                let cache_dir = config.den_home.join("cache");
                let index = FormulaIndex::load(&client, &cache_dir).await?;
                show_info(&config, &index, &name)
            }
            Some(Command::Search { text }) => {
                let client = reqwest::Client::new();
                let cache_dir = config.den_home.join("cache");
                let index = FormulaIndex::load(&client, &cache_dir).await?;
                search_packages(&index, &text)
            }
            Some(Command::Deps { name, tree }) => {
                let client = reqwest::Client::new();
                let cache_dir = config.den_home.join("cache");
                let index = FormulaIndex::load(&client, &cache_dir).await?;
                show_deps(&index, &name, tree)
            }
            Some(Command::Cleanup) => cleanup(&config),
            Some(Command::Autoremove) => {
                let active = env::active_env_path(&config.den_home);
                autoremove(&config, &active)
            }
            Some(Command::Services { command }) => run_services_command(&config, command),
            Some(Command::Set { key, value }) => {
                settings::set(&config.den_home, &key, &value)?;
                println!("{key} = {value}");
                Ok(())
            }
            Some(Command::Settings) => {
                println!("{}", settings::display_all(&config.den_home));
                Ok(())
            }
            Some(Command::Tap { .. }) => {
                anyhow::bail!("'tap' is not yet implemented")
            }
            Some(Command::Untap { .. }) => {
                anyhow::bail!("'untap' is not yet implemented")
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

fn run_env_command(config: &Config, command: Option<EnvCommand>) -> anyhow::Result<()> {
    match command {
        Some(EnvCommand::List) | None => {
            let envs = env::list_envs(&config.den_home)?;
            if envs.is_empty() {
                // Ensure root exists.
                manifest::write_manifest(&config.den_home, "/", &manifest::Manifest::default())?;
                println!("/ (active)");
            } else {
                let active = env::active_env_path(&config.den_home);
                for env_path in &envs {
                    let marker = if *env_path == active { " (active)" } else { "" };
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
            let env_path = normalise_env_path(&path)?;

            // Ensure parent exists (except for root).
            if let Some(parent) = manifest::parent_path(&env_path)
                && !manifest::manifest_exists(&config.den_home, &parent)
            {
                anyhow::bail!(
                    "parent environment '{}' does not exist. Create it first.",
                    parent
                );
            }

            if manifest::manifest_exists(&config.den_home, &env_path) {
                anyhow::bail!("environment '{}' already exists", env_path);
            }

            manifest::write_manifest(&config.den_home, &env_path, &manifest::Manifest::default())?;

            // Materialise (inherits from parent).
            let links = env::materialise(&config.den_home, &config.cellar, &env_path)?;
            let slug = manifest::env_slug(&env_path);
            println!("Created environment '{env_path}' ({links} symlinks, dir: {slug})");
            Ok(())
        }
        Some(EnvCommand::Use { path }) => {
            let env_path = normalise_env_path(&path)?;

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
                .transpose()?
                .unwrap_or_else(|| env::active_env_path(&config.den_home));

            let resolved = manifest::resolve(&config.den_home, &env_path)?;
            if resolved.is_empty() {
                println!("Environment '{env_path}' has no packages.");
            } else {
                println!("Resolved packages for '{env_path}':");
                for (name, version) in &resolved {
                    // Show which level contributed this package.
                    let source = find_source_env(config, &env_path, name);
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
            let env_path = normalise_env_path(&path)?;
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
                .transpose()?
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
        if let Ok(m) = manifest::read_manifest(&config.den_home, ancestor)
            && m.packages.contains_key(package)
        {
            return ancestor.clone();
        }
    }
    env_path.to_string()
}

async fn install_formula(
    client: &reqwest::Client,
    config: &Config,
    index: &FormulaIndex,
    active_env: &str,
    name: &str,
) -> anyhow::Result<()> {
    // Resolve full dependency tree from local index — no network calls.
    println!("==> Resolving dependencies for {name}...");
    let all = deps::resolve_install_order(index, name)?;
    let missing = deps::filter_missing(&all, &config.cellar);

    if missing.len() > 1 {
        let dep_names: Vec<_> = missing
            .iter()
            .filter(|f| f.name != name)
            .map(|f| f.name.as_str())
            .collect();
        if !dep_names.is_empty() {
            println!(
                "  {} dependencies to install: {}",
                dep_names.len(),
                dep_names.join(", ")
            );
        }
    }

    // Pour all missing packages (deps first, then the requested one).
    for info in &all {
        let pkg_version = info.pkg_version();
        let keg_path = config.cellar.join(&info.name).join(&pkg_version);

        if keg_path.is_dir() {
            continue;
        }

        pour_bottle(client, config, info).await?;
    }

    // Update manifest: requested package is explicit, deps are auto.
    let mut m = manifest::read_manifest(&config.den_home, active_env)?;
    for info in &all {
        let pkg_version = info.pkg_version();
        m.packages.insert(info.name.clone(), pkg_version);
        if info.name != name {
            m.auto.insert(info.name.clone());
        }
    }
    manifest::write_manifest(&config.den_home, active_env, &m)?;

    // Re-materialise.
    println!("==> Materialising environment '{active_env}'...");
    let links = env::materialise(&config.den_home, &config.cellar, active_env)?;
    println!("  {links} symlinks");

    // Show caveats for the main package.
    let main_info = all.iter().find(|f| f.name == name);
    if let Some(info) = main_info
        && let Some(ref caveats) = info.caveats
    {
        println!("==> Caveats");
        println!("{caveats}");
    }

    println!("==> {name} installed to '{active_env}'.");
    Ok(())
}

async fn pour_bottle(
    client: &reqwest::Client,
    config: &Config,
    info: &formula::FormulaInfo,
) -> anyhow::Result<()> {
    let pkg_version = info.pkg_version();
    let macos = config
        .macos_version
        .as_ref()
        .ok_or_else(|| anyhow::anyhow!("cannot determine macOS version"))?;

    let bottle_spec = info
        .bottle
        .as_ref()
        .and_then(|b| b.stable.as_ref())
        .ok_or_else(|| anyhow::anyhow!("no bottle available for {}", info.name))?;

    let available_tags: Vec<String> = bottle_spec.files.keys().cloned().collect();
    let tag = platform::best_bottle_tag(config.arch, macos, &available_tags).ok_or_else(|| {
        anyhow::anyhow!(
            "no bottle available for {} on {} {}",
            info.name,
            config.arch,
            macos
        )
    })?;

    let bottle_file = &bottle_spec.files[&tag];
    let ghcr_path = formula::ghcr_path(&info.name);

    let cache_dir = config.den_home.join("cache").join("bottles");
    println!("==> Fetching {} {} ({})...", info.name, pkg_version, tag);
    let bottle_data = bottle::fetch_bottle(client, bottle_file, &ghcr_path, &cache_dir).await?;
    println!("  {} bytes, SHA256 verified", bottle_data.len());

    println!("==> Pouring {} {}...", info.name, pkg_version);
    let poured_keg = bottle::pour_bottle(&bottle_data, &config.cellar)?;
    tab::write_tab(&poured_keg, &config.arch.to_string())?;
    Ok(())
}

fn uninstall_package(config: &Config, active_env: &str, name: &str) -> anyhow::Result<()> {
    let mut m = manifest::read_manifest(&config.den_home, active_env)?;

    if !m.packages.contains_key(name) {
        anyhow::bail!("{name} is not in environment '{active_env}'");
    }

    m.packages.remove(name);
    m.auto.remove(name);
    manifest::write_manifest(&config.den_home, active_env, &m)?;

    // Re-materialise.
    println!("==> Removing {name} from '{active_env}'...");
    let links = env::materialise(&config.den_home, &config.cellar, active_env)?;
    println!("  {links} symlinks remain");
    println!("==> {name} removed from '{active_env}'.");
    println!("  Keg still in Cellar. Run `den autoremove` to clean up orphaned deps.");
    Ok(())
}

fn autoremove(_config: &Config, _active_env: &str) -> anyhow::Result<()> {
    anyhow::bail!("autoremove is not yet implemented")
}

fn migrate_cellar(config: &Config) -> anyhow::Result<()> {
    println!("==> Scanning Cellar at {}...", config.cellar.display());

    let kegs = keg::list_installed(&config.cellar)?;
    if kegs.is_empty() {
        println!("No packages found in Cellar.");
        return Ok(());
    }

    // Ensure root manifest exists.
    let mut m = manifest::read_manifest(&config.den_home, "/")?;
    m.origin = Some("homebrew-migration".to_string());

    let mut added = 0u32;
    let mut skipped = 0u32;

    // For each formula, pick the latest version and check the tab.
    let mut seen = std::collections::HashSet::new();
    for k in kegs.iter().rev() {
        if seen.contains(&k.name) {
            continue;
        }
        seen.insert(k.name.clone());

        // Check if installed on request (from tab).
        let on_request = tab::read_tab(&k.path)
            .map(|t| t.installed_on_request)
            .unwrap_or(true); // Default to explicit if no tab.

        if m.packages.contains_key(&k.name) {
            skipped += 1;
            continue;
        }

        m.packages.insert(k.name.clone(), k.version.clone());
        if !on_request {
            m.auto.insert(k.name.clone());
        }
        added += 1;
    }

    manifest::write_manifest(&config.den_home, "/", &m)?;

    println!("  {} packages added, {} already tracked", added, skipped);

    // Materialise.
    println!("==> Materialising root environment...");
    let links = env::materialise(&config.den_home, &config.cellar, "/")?;
    println!("  {links} symlinks");
    println!("==> Migration complete. Run `eval \"$(den init)\"` to activate.");

    Ok(())
}

fn show_outdated(config: &Config, index: &FormulaIndex, active_env: &str) -> anyhow::Result<()> {
    let resolved = manifest::resolve(&config.den_home, active_env)?;
    if resolved.is_empty() {
        println!("No packages to check.");
        return Ok(());
    }

    let mut outdated = Vec::new();

    for (name, installed_version) in &resolved {
        if let Some(info) = index.get(name) {
            let latest = info.pkg_version();
            if latest != *installed_version {
                outdated.push((name.clone(), installed_version.clone(), latest));
            }
        }
    }

    if outdated.is_empty() {
        println!("All packages are up to date.");
    } else {
        println!("{} packages outdated:", outdated.len());
        for (name, installed, latest) in &outdated {
            println!("  {name} {installed} -> {latest}");
        }
    }

    Ok(())
}

async fn upgrade_packages(
    client: &reqwest::Client,
    config: &Config,
    index: &FormulaIndex,
    active_env: &str,
    names: &[String],
) -> anyhow::Result<()> {
    let resolved = manifest::resolve(&config.den_home, active_env)?;

    let to_check: Vec<_> = if names.is_empty() {
        resolved
            .iter()
            .map(|(k, v)| (k.clone(), v.clone()))
            .collect()
    } else {
        resolved
            .iter()
            .filter(|(k, _)| names.iter().any(|n| n == k.as_str()))
            .map(|(k, v)| (k.clone(), v.clone()))
            .collect()
    };

    if to_check.is_empty() {
        println!("No packages to upgrade.");
        return Ok(());
    }

    let mut upgraded = 0u32;

    for (name, installed_version) in &to_check {
        let info = match index.get(name) {
            Some(info) => info,
            None => continue,
        };

        let latest = info.pkg_version();
        if latest == *installed_version {
            continue;
        }

        println!(
            "==> Upgrading {} {} -> {}...",
            name, installed_version, latest
        );

        let all = deps::resolve_install_order(index, name)?;
        for dep_info in &all {
            let keg_path = config
                .cellar
                .join(&dep_info.name)
                .join(dep_info.pkg_version());
            if !keg_path.is_dir() {
                pour_bottle(client, config, dep_info).await?;
            }
        }

        let mut m = manifest::read_manifest(&config.den_home, active_env)?;
        m.packages.insert(name.clone(), latest.clone());
        for dep_info in &all {
            let dep_ver = dep_info.pkg_version();
            m.packages.insert(dep_info.name.clone(), dep_ver);
            if dep_info.name != *name {
                m.auto.insert(dep_info.name.clone());
            }
        }
        manifest::write_manifest(&config.den_home, active_env, &m)?;
        upgraded += 1;
    }

    if upgraded == 0 {
        println!("All packages are up to date.");
    } else {
        println!("==> Materialising environment '{active_env}'...");
        let links = env::materialise(&config.den_home, &config.cellar, active_env)?;
        println!("  {links} symlinks");
        println!("==> {upgraded} package(s) upgraded.");
    }

    Ok(())
}

async fn install_cask_cmd(
    client: &reqwest::Client,
    config: &Config,
    active_env: &str,
    token: &str,
) -> anyhow::Result<()> {
    println!("==> Fetching cask {token}...");
    let info = cask::fetch_cask(client, token).await?;

    let apps = info.app_artifacts();
    if apps.is_empty() {
        anyhow::bail!(
            "cask '{}' has no app artifacts (pkg/binary casks not yet supported)",
            token
        );
    }

    println!("==> Downloading {} {}...", info.token, info.version);
    let (data, ext) = cask::download_cask(client, &info).await?;
    println!("  {} bytes downloaded", data.len());

    let appdir = std::path::PathBuf::from("/Applications");

    println!("==> Installing {}...", apps.join(", "));
    let installed = match ext.as_str() {
        "dmg" => cask::install_from_dmg(&data, &apps, &appdir)?,
        "zip" => cask::install_from_zip(&data, &apps, &appdir)?,
        _ => anyhow::bail!("unsupported cask format: {ext}"),
    };

    // Track in manifest.
    let mut m = manifest::read_manifest(&config.den_home, active_env)?;
    m.casks.insert(info.token.clone(), info.version.clone());
    manifest::write_manifest(&config.den_home, active_env, &m)?;

    for path in &installed {
        println!("  Installed {}", path.display());
    }

    if let Some(ref caveats) = info.caveats {
        println!("==> Caveats");
        println!("{caveats}");
    }

    println!("==> {} {} installed.", info.token, info.version);
    Ok(())
}

fn run_services_command(config: &Config, command: Option<ServiceCommand>) -> anyhow::Result<()> {
    let env_path = env::active_env_path(&config.den_home);
    let env_dir = env::env_dir(&config.den_home, &env_path);
    let opt_dir = env_dir.join("opt");

    match command {
        Some(ServiceCommand::List) | None => {
            let services = service::list_services(&config.cellar, &opt_dir)?;
            if services.is_empty() {
                println!("No services found.");
            } else {
                for svc in &services {
                    let status = if svc.running { "running" } else { "stopped" };
                    println!("{} ({})", svc.name, status);
                }
            }
            Ok(())
        }
        Some(ServiceCommand::Start { name }) => {
            let services = service::list_services(&config.cellar, &opt_dir)?;
            let svc = services
                .iter()
                .find(|s| s.name == name)
                .ok_or_else(|| anyhow::anyhow!("no service found for '{name}'"))?;
            if svc.running {
                println!("{name} is already running.");
                return Ok(());
            }
            service::start_service(svc)?;
            println!("==> Started {name}.");
            Ok(())
        }
        Some(ServiceCommand::Stop { name }) => {
            let services = service::list_services(&config.cellar, &opt_dir)?;
            let svc = services
                .iter()
                .find(|s| s.name == name)
                .ok_or_else(|| anyhow::anyhow!("no service found for '{name}'"))?;
            if !svc.running {
                println!("{name} is not running.");
                return Ok(());
            }
            service::stop_service(svc)?;
            println!("==> Stopped {name}.");
            Ok(())
        }
        Some(ServiceCommand::Restart { name }) => {
            let services = service::list_services(&config.cellar, &opt_dir)?;
            let svc = services
                .iter()
                .find(|s| s.name == name)
                .ok_or_else(|| anyhow::anyhow!("no service found for '{name}'"))?;
            service::restart_service(svc)?;
            println!("==> Restarted {name}.");
            Ok(())
        }
    }
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
        anyhow::bail!("{formula_name} is not installed. Run `den install {formula_name}` first.");
    }

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
        // Safe: we checked versions.is_empty() above.
        versions.last().expect("versions is non-empty")
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

fn show_info(config: &Config, index: &FormulaIndex, name: &str) -> anyhow::Result<()> {
    let info = index
        .get(name)
        .ok_or_else(|| anyhow::anyhow!("formula '{}' not found", name))?;
    let pkg_version = info.pkg_version();

    println!("{}: stable {}", info.name, pkg_version);
    if let Some(ref desc) = info.desc {
        println!("{desc}");
    }

    // Check if installed.
    let versions = keg::find_versions(&config.cellar, &info.name)?;
    if versions.is_empty() {
        println!("Not installed");
    } else {
        let ver_list: Vec<_> = versions.iter().map(|k| k.version.as_str()).collect();
        println!("Installed: {}", ver_list.join(", "));
    }

    if !info.dependencies.is_empty() {
        println!("Dependencies: {}", info.dependencies.join(", "));
    }

    if info.keg_only {
        println!("Keg-only");
    }

    if let Some(ref caveats) = info.caveats {
        println!("Caveats: {caveats}");
    }

    Ok(())
}

fn search_packages(index: &FormulaIndex, text: &str) -> anyhow::Result<()> {
    let query = text.to_lowercase();

    let mut matches: Vec<_> = index
        .iter()
        .filter(|(_, info)| {
            info.name.to_lowercase().contains(&query)
                || info
                    .desc
                    .as_deref()
                    .unwrap_or("")
                    .to_lowercase()
                    .contains(&query)
        })
        .collect();

    // Sort: exact name matches first, then name contains, then description.
    matches.sort_by(|(_, a), (_, b)| {
        let a_exact = a.name.to_lowercase() == query;
        let b_exact = b.name.to_lowercase() == query;
        let a_name = a.name.to_lowercase().contains(&query);
        let b_name = b.name.to_lowercase().contains(&query);
        b_exact
            .cmp(&a_exact)
            .then(b_name.cmp(&a_name))
            .then(a.name.cmp(&b.name))
    });

    if matches.is_empty() {
        println!("No formulae found for '{text}'.");
    } else {
        for (_, info) in &matches {
            let desc = info.desc.as_deref().unwrap_or("");
            println!("{}: {desc}", info.name);
        }
    }

    Ok(())
}

fn show_deps(index: &FormulaIndex, name: &str, tree: bool) -> anyhow::Result<()> {
    let info = index
        .get(name)
        .ok_or_else(|| anyhow::anyhow!("formula '{}' not found", name))?;

    if tree {
        let mut visited = std::collections::HashSet::new();
        print_dep_tree(index, name, 0, &mut visited);
    } else if info.dependencies.is_empty() {
        println!("{name} has no dependencies.");
    } else {
        for dep in &info.dependencies {
            println!("{dep}");
        }
    }
    Ok(())
}

fn print_dep_tree(
    index: &FormulaIndex,
    name: &str,
    depth: usize,
    visited: &mut std::collections::HashSet<String>,
) {
    let indent = "  ".repeat(depth);
    let circular = visited.contains(name);
    println!(
        "{indent}{name}{}",
        if circular { " (circular)" } else { "" }
    );

    if circular {
        return;
    }
    visited.insert(name.to_string());

    if let Some(info) = index.get(name) {
        for dep in &info.dependencies {
            print_dep_tree(index, dep, depth + 1, visited);
        }
    }
}

fn cleanup(config: &Config) -> anyhow::Result<()> {
    // Remove cached bottles.
    let cache_dir = config.den_home.join("cache").join("bottles");
    if cache_dir.is_dir() {
        let count = std::fs::read_dir(&cache_dir)?.count();
        std::fs::remove_dir_all(&cache_dir)?;
        println!("Removed {count} cached bottles.");
    } else {
        println!("No cached bottles to remove.");
    }
    Ok(())
}

async fn run_daemon_command(config: &Config, command: DaemonCommand) -> anyhow::Result<()> {
    match command {
        DaemonCommand::Run => daemon::run(config).await,
        DaemonCommand::Stop => {
            let pid_path = config.den_home.join("daemon.pid");
            let pid_str = std::fs::read_to_string(&pid_path)
                .map_err(|_| anyhow::anyhow!("daemon is not running"))?;
            let pid: i32 = pid_str.trim().parse()?;

            let result = unsafe { libc::kill(pid, libc::SIGTERM) };
            if result != 0 {
                anyhow::bail!("failed to send SIGTERM to PID {pid}");
            }
            println!("Sent SIGTERM to daemon (PID {pid}).");
            Ok(())
        }
        DaemonCommand::Status => {
            if daemon::is_running(&config.den_home) {
                let pid_str =
                    std::fs::read_to_string(config.den_home.join("daemon.pid")).unwrap_or_default();
                println!("Daemon: running (PID {})", pid_str.trim());
            } else {
                println!("Daemon: not running");
            }

            let state = daemon::read_state(&config.den_home);
            if let Some(last) = state.last_check {
                let ago = daemon::now_secs().saturating_sub(last);
                println!("Last check: {}s ago", ago);
            } else {
                println!("Last check: never");
            }

            if state.pending.is_empty() {
                println!("Pending upgrades: none");
            } else {
                println!("Pending upgrades:");
                for p in &state.pending {
                    println!("  {} {} -> {}", p.name, p.installed, p.available);
                }
            }

            let s = settings::read(&config.den_home);
            println!(
                "Auto-upgrade: {}",
                if s.daemon.auto_upgrade { "on" } else { "off" }
            );
            if let Some(ref window) = s.daemon.upgrade_window {
                println!("Upgrade window: {window}");
            }

            Ok(())
        }
        DaemonCommand::Install => {
            let exe = std::env::current_exe()?;
            let plist = daemon::launchd_plist(&exe, &config.den_home);
            let plist_path = dirs::home_dir()
                .ok_or_else(|| anyhow::anyhow!("cannot determine home directory"))?
                .join("Library/LaunchAgents/dev.den.daemon.plist");

            if let Some(parent) = plist_path.parent() {
                std::fs::create_dir_all(parent)?;
            }
            std::fs::write(&plist_path, &plist)?;

            let status = std::process::Command::new("launchctl")
                .args(["load", "-w"])
                .arg(&plist_path)
                .status()?;

            if status.success() {
                println!("Daemon installed and started.");
                println!("  Plist: {}", plist_path.display());
            } else {
                anyhow::bail!("failed to load launchd plist");
            }
            Ok(())
        }
        DaemonCommand::Uninstall => {
            let plist_path = dirs::home_dir()
                .ok_or_else(|| anyhow::anyhow!("cannot determine home directory"))?
                .join("Library/LaunchAgents/dev.den.daemon.plist");

            if plist_path.exists() {
                let _ = std::process::Command::new("launchctl")
                    .args(["unload", "-w"])
                    .arg(&plist_path)
                    .status();
                std::fs::remove_file(&plist_path)?;
                println!("Daemon uninstalled.");
            } else {
                println!("Daemon is not installed.");
            }
            Ok(())
        }
    }
}

fn normalise_env_path(path: &str) -> anyhow::Result<String> {
    if path.is_empty() || path == "/" {
        return Ok("/".to_string());
    }
    // Reject path traversal.
    if path.contains("..") {
        anyhow::bail!("environment path must not contain '..'");
    }
    let p = if path.starts_with('/') {
        path.to_string()
    } else {
        format!("/{path}")
    };
    // Remove trailing slash.
    Ok(p.trim_end_matches('/').to_string())
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

_den_env="{den_home}/envs/{root_slug}"

# Add den binary and root environment to PATH (before Homebrew).
export PATH="{den_home}/bin:$_den_env/bin:$PATH"

# Build environment — headers, libraries, pkg-config.
export LIBRARY_PATH="$_den_env/lib${{LIBRARY_PATH:+:$LIBRARY_PATH}}"
export CPATH="$_den_env/include${{CPATH:+:$CPATH}}"
export PKG_CONFIG_PATH="$_den_env/lib/pkgconfig:$_den_env/share/pkgconfig${{PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}}"
export CMAKE_PREFIX_PATH="$_den_env${{CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}}"
export MANPATH="$_den_env/share/man${{MANPATH:+:$MANPATH}}:"
export INFOPATH="$_den_env/share/info${{INFOPATH:+:$INFOPATH}}"

unset _den_env

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
set -l _den_env "{den_home}/envs/{root_slug}"

# Add den binary and root environment to PATH.
fish_add_path --prepend "{den_home}/bin" "$_den_env/bin"

# Build environment — headers, libraries, pkg-config.
set -gx LIBRARY_PATH "$_den_env/lib" $LIBRARY_PATH
set -gx CPATH "$_den_env/include" $CPATH
set -gx PKG_CONFIG_PATH "$_den_env/lib/pkgconfig" "$_den_env/share/pkgconfig" $PKG_CONFIG_PATH
set -gx CMAKE_PREFIX_PATH "$_den_env" $CMAKE_PREFIX_PATH
set -gx MANPATH "$_den_env/share/man" $MANPATH
set -gx INFOPATH "$_den_env/share/info" $INFOPATH

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
    let new_env = den_home.join("envs").join(env_slug);
    let env_path = manifest::slug_to_path(env_slug);
    let dh = den_home.display();
    let ne = new_env.display();

    // Swap PATH, build env vars, and DEN_ENV in one eval.
    println!(
        r#"export PATH="$(echo "$PATH" | sed "s|{dh}/envs/[^:]*bin:||g")"
export PATH="{ne}/bin:$PATH"
export LIBRARY_PATH="$(echo "${{LIBRARY_PATH:-}}" | sed "s|{dh}/envs/[^:]*/lib:*||g")"
export LIBRARY_PATH="{ne}/lib${{LIBRARY_PATH:+:$LIBRARY_PATH}}"
export CPATH="$(echo "${{CPATH:-}}" | sed "s|{dh}/envs/[^:]*/include:*||g")"
export CPATH="{ne}/include${{CPATH:+:$CPATH}}"
export PKG_CONFIG_PATH="$(echo "${{PKG_CONFIG_PATH:-}}" | sed "s|{dh}/envs/[^:]*/lib/pkgconfig:*||g;s|{dh}/envs/[^:]*/share/pkgconfig:*||g")"
export PKG_CONFIG_PATH="{ne}/lib/pkgconfig:{ne}/share/pkgconfig${{PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}}"
export CMAKE_PREFIX_PATH="$(echo "${{CMAKE_PREFIX_PATH:-}}" | sed "s|{dh}/envs/[^:]*:*||g")"
export CMAKE_PREFIX_PATH="{ne}${{CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}}"
export MANPATH="$(echo "${{MANPATH:-}}" | sed "s|{dh}/envs/[^:]*/share/man:*||g")"
export MANPATH="{ne}/share/man${{MANPATH:+:$MANPATH}}:"
export INFOPATH="$(echo "${{INFOPATH:-}}" | sed "s|{dh}/envs/[^:]*/share/info:*||g")"
export INFOPATH="{ne}/share/info${{INFOPATH:+:$INFOPATH}}"
export DEN_ENV="{env_path}""#,
    );
}

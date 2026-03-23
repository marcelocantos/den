// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use clap::{Parser, Subcommand};

use crate::config::Config;
use crate::{api, bottle, cask, deps, env, formula, keg, link, manifest, platform, service, tab};

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
    /// Import existing Homebrew Cellar into den
    Migrate,
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

                for name in &names {
                    if is_cask {
                        install_cask_cmd(&client, &config, &active, name).await?;
                    } else {
                        install_formula(&client, &config, &active, name).await?;
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
                upgrade_packages(&client, &config, &active, &names).await
            }
            Some(Command::Update) => {
                println!("den uses the Homebrew API directly — no local state to update.");
                println!("Packages are always fetched from the latest API data.");
                Ok(())
            }
            Some(Command::Outdated) => {
                let client = reqwest::Client::new();
                let active = env::active_env_path(&config.den_home);
                show_outdated(&client, &config, &active).await
            }
            Some(Command::Migrate) => {
                migrate_cellar(&config)
            }
            Some(Command::List { names }) => list_packages(&config, &names),
            Some(Command::Use { name }) => use_version(&config, &name),
            Some(Command::Init { shell }) => {
                print_shell_init(&config, &shell);
                Ok(())
            }
            Some(Command::Env { command }) => run_env_command(&config, command),
            Some(Command::Info { name }) => {
                let client = reqwest::Client::new();
                show_info(&client, &config, &name).await
            }
            Some(Command::Search { text }) => {
                let client = reqwest::Client::new();
                search_packages(&client, &text).await
            }
            Some(Command::Deps { name, tree }) => {
                let client = reqwest::Client::new();
                show_deps(&client, &name, tree).await
            }
            Some(Command::Cleanup { names }) => {
                cleanup(&config, &names)
            }
            Some(Command::Autoremove) => {
                let active = env::active_env_path(&config.den_home);
                autoremove(&config, &active)
            }
            Some(Command::Services { command }) => {
                run_services_command(&config, command)
            }
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
    // Resolve full dependency tree.
    println!("==> Resolving dependencies for {name}...");
    let all = deps::resolve_install_order(client, name, &config.cellar).await?;
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
    if let Some(info) = main_info {
        if let Some(ref caveats) = info.caveats {
            println!("==> Caveats");
            println!("{caveats}");
        }
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

    let cache_dir = config.den_home.join("cache").join("bottles");
    println!("==> Fetching {} {} ({})...", info.name, pkg_version, tag);
    let bottle_data = bottle::fetch_bottle(client, bottle_file, &ghcr_path, &cache_dir).await?;
    println!("  {} bytes, SHA256 verified", bottle_data.len());

    println!("==> Pouring {} {}...", info.name, pkg_version);
    let poured_keg = bottle::pour_bottle(&bottle_data, &config.cellar)?;
    tab::write_tab(&poured_keg, &config.arch.to_string())?;
    Ok(())
}

fn uninstall_package(
    config: &Config,
    active_env: &str,
    name: &str,
) -> anyhow::Result<()> {
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

fn autoremove(config: &Config, active_env: &str) -> anyhow::Result<()> {
    let mut m = manifest::read_manifest(&config.den_home, active_env)?;

    // Find auto-installed packages that are no longer needed as deps
    // of any explicit package.
    let explicit: Vec<String> = m
        .packages
        .keys()
        .filter(|k| !m.auto.contains(k.as_str()))
        .cloned()
        .collect();

    // For now, simple approach: auto packages not in explicit stay.
    // A proper implementation would check the dep graph.
    // TODO: traverse dep graph to find truly orphaned auto packages.

    let orphans: Vec<String> = m
        .auto
        .iter()
        .filter(|name| !explicit.contains(name))
        .cloned()
        .collect();

    if orphans.is_empty() {
        println!("No orphaned dependencies to remove.");
        return Ok(());
    }

    let mut removed = 0;
    for name in &orphans {
        // Check if any explicit package depends on this.
        // For now, keep all auto packages (conservative).
        // Full dep-graph check is a future improvement.
        let _ = name;
    }

    if removed == 0 {
        println!("No orphaned dependencies to remove (dep-graph check pending).");
    }

    Ok(())
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

    println!(
        "  {} packages added, {} already tracked",
        added, skipped
    );

    // Materialise.
    println!("==> Materialising root environment...");
    let links = env::materialise(&config.den_home, &config.cellar, "/")?;
    println!("  {links} symlinks");
    println!("==> Migration complete. Run `eval \"$(den init)\"` to activate.");

    Ok(())
}

async fn show_outdated(
    client: &reqwest::Client,
    config: &Config,
    active_env: &str,
) -> anyhow::Result<()> {
    let resolved = manifest::resolve(&config.den_home, active_env)?;
    if resolved.is_empty() {
        println!("No packages to check.");
        return Ok(());
    }

    println!("==> Checking for updates...");
    let mut outdated = Vec::new();

    for (name, installed_version) in &resolved {
        match api::fetch_formula(client, name).await {
            Ok(info) => {
                let latest = info.pkg_version();
                if latest != *installed_version {
                    outdated.push((name.clone(), installed_version.clone(), latest));
                }
            }
            Err(_) => {
                // Formula might not exist in API (tap-only, etc.)
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
    active_env: &str,
    names: &[String],
) -> anyhow::Result<()> {
    let resolved = manifest::resolve(&config.den_home, active_env)?;

    let to_check: Vec<_> = if names.is_empty() {
        resolved.iter().map(|(k, v)| (k.clone(), v.clone())).collect()
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

    println!("==> Checking for updates...");
    let mut upgraded = 0u32;

    for (name, installed_version) in &to_check {
        let info = match api::fetch_formula(client, &name).await {
            Ok(info) => info,
            Err(_) => continue,
        };

        let latest = info.pkg_version();
        if latest == *installed_version {
            continue;
        }

        println!("==> Upgrading {} {} -> {}...", name, installed_version, latest);

        // Pour the new version (and any new deps).
        let all = deps::resolve_install_order(client, &name, &config.cellar).await?;
        for dep_info in &all {
            let keg_path = config
                .cellar
                .join(&dep_info.name)
                .join(dep_info.pkg_version());
            if !keg_path.is_dir() {
                pour_bottle(client, config, dep_info).await?;
            }
        }

        // Update manifest.
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
        // Re-materialise.
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

    println!(
        "==> Downloading {} {}...",
        info.token, info.version
    );
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

async fn show_info(
    client: &reqwest::Client,
    config: &Config,
    name: &str,
) -> anyhow::Result<()> {
    let info = api::fetch_formula(client, name).await?;
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

async fn search_packages(
    client: &reqwest::Client,
    text: &str,
) -> anyhow::Result<()> {
    // Fetch the full formula index and search by name/description.
    let url = "https://formulae.brew.sh/api/formula.json";
    let response = client
        .get(url)
        .header("User-Agent", "den/0.1.0")
        .send()
        .await?;

    if !response.status().is_success() {
        anyhow::bail!("failed to fetch formula index (HTTP {})", response.status());
    }

    #[derive(serde::Deserialize)]
    struct SearchEntry {
        name: String,
        #[serde(default)]
        desc: Option<String>,
    }

    let entries: Vec<SearchEntry> = response.json().await?;
    let query = text.to_lowercase();

    let matches: Vec<_> = entries
        .iter()
        .filter(|e| {
            e.name.to_lowercase().contains(&query)
                || e.desc
                    .as_deref()
                    .unwrap_or("")
                    .to_lowercase()
                    .contains(&query)
        })
        .collect();

    if matches.is_empty() {
        println!("No formulae found for '{text}'.");
    } else {
        for entry in &matches {
            let desc = entry.desc.as_deref().unwrap_or("");
            println!("{}: {desc}", entry.name);
        }
    }

    Ok(())
}

async fn show_deps(
    client: &reqwest::Client,
    name: &str,
    tree: bool,
) -> anyhow::Result<()> {
    if tree {
        let mut visited = std::collections::HashSet::new();
        print_dep_tree(client, name, 0, &mut visited).await?;
    } else {
        let info = api::fetch_formula(client, name).await?;
        if info.dependencies.is_empty() {
            println!("{name} has no dependencies.");
        } else {
            for dep in &info.dependencies {
                println!("{dep}");
            }
        }
    }
    Ok(())
}

fn print_dep_tree<'a>(
    client: &'a reqwest::Client,
    name: &'a str,
    depth: usize,
    visited: &'a mut std::collections::HashSet<String>,
) -> std::pin::Pin<Box<dyn std::future::Future<Output = anyhow::Result<()>> + 'a>> {
    Box::pin(async move {
        let indent = "  ".repeat(depth);
        let circular = visited.contains(name);
        println!("{indent}{name}{}", if circular { " (circular)" } else { "" });

        if circular {
            return Ok(());
        }
        visited.insert(name.to_string());

        if let Ok(info) = api::fetch_formula(client, name).await {
            for dep in &info.dependencies {
                print_dep_tree(client, dep, depth + 1, visited).await?;
            }
        }

        Ok(())
    })
}

fn cleanup(config: &Config, _names: &[String]) -> anyhow::Result<()> {
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

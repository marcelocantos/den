// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use crate::api::FormulaIndex;
#[cfg(target_os = "macos")]
use crate::cask;
use crate::config::Config;
use crate::{bottle, deps, env, formula, manifest, platform, tab};

pub(super) async fn install_formula(
    client: &reqwest::Client,
    config: &Config,
    index: &FormulaIndex,
    active_env: &str,
    name: &str,
) -> anyhow::Result<()> {
    crate::api::validate_formula_name(name)?;

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

pub(super) async fn pour_bottle(
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
    let ghcr_path = formula::ghcr_path(&info.name)?;

    let cache_dir = config.den_home.join("cache").join("bottles");
    println!("==> Fetching {} {} ({})...", info.name, pkg_version, tag);
    let bottle_data = bottle::fetch_bottle(client, bottle_file, &ghcr_path, &cache_dir).await?;
    println!("  {} bytes, SHA256 verified", bottle_data.len());

    println!("==> Pouring {} {}...", info.name, pkg_version);
    let poured_keg = bottle::pour_bottle(&bottle_data, &config.cellar)?;
    tab::write_tab(&poured_keg, &config.arch.to_string())?;
    Ok(())
}

pub(super) fn uninstall_package(
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

pub(super) fn autoremove(_config: &Config, _active_env: &str) -> anyhow::Result<()> {
    anyhow::bail!("autoremove is not yet implemented")
}

pub(super) fn show_outdated(
    config: &Config,
    index: &FormulaIndex,
    active_env: &str,
) -> anyhow::Result<()> {
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

pub(super) async fn upgrade_packages(
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

#[cfg(target_os = "macos")]
pub(super) async fn install_cask_cmd(
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
    let (data, ext) = cask::download_cask(&info).await?;
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

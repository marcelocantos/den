// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

//! CLI integration for tap formula installation.

use crate::config::Config;
use crate::{env, manifest, tap};

/// Install a formula from a tap.
pub(super) async fn install_tap_formula(
    client: &reqwest::Client,
    config: &Config,
    active_env: &str,
    tap_name: &str,
    formula_name: &str,
) -> anyhow::Result<()> {
    println!("==> Looking up {formula_name} in tap {tap_name}...");
    let formula = tap::find_tap_formula(&config.den_home, tap_name, formula_name)?;

    // Check if the formula has system calls in its install block (source builds).
    // We detect this by checking if it has install steps — if it has none and no
    // bin.install, that might indicate a source build formula, but we handle that
    // gracefully by copying executables.

    let (url, sha256) = tap::resolve_for_platform(&formula, config.arch)?;

    if url.is_empty() {
        anyhow::bail!(
            "no download URL available for {formula_name} on {} {}",
            std::env::consts::OS,
            config.arch
        );
    }

    println!("==> Downloading {} {}...", formula.name, formula.version);

    let cache_dir = config.den_home.join("cache").join("taps");
    let data = tap::download_tap_formula(client, &url, &sha256, &cache_dir).await?;
    println!("  {} bytes, SHA256 verified", data.len());

    println!("==> Installing {} {}...", formula.name, formula.version);
    let keg_path = tap::install_tap_formula_keg(&data, &config.cellar, &formula)?;
    println!("  Installed to {}", keg_path.display());

    // Update manifest.
    let mut m = manifest::read_manifest(&config.den_home, active_env)?;
    m.packages
        .insert(formula.name.clone(), formula.version.clone());
    manifest::write_manifest(&config.den_home, active_env, &m)?;

    // Materialise environment.
    println!("==> Materialising environment '{active_env}'...");
    let links = env::materialise(&config.den_home, &config.cellar, active_env)?;
    println!("  {links} symlinks");

    println!(
        "==> {} {} installed to '{active_env}'.",
        formula.name, formula.version
    );
    Ok(())
}

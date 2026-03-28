// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use crate::config::Config;
use crate::{env, keg, manifest, tab};

pub(super) fn migrate_cellar(config: &Config) -> anyhow::Result<()> {
    println!("==> Scanning Cellar at {}...", config.cellar.display());

    let kegs = keg::list_installed(&config.cellar)?;
    if kegs.is_empty() {
        println!("No packages found in Cellar.");
        return Ok(());
    }

    // Ensure root manifest exists and populate from Cellar.
    let (added, skipped) = manifest::with_manifest_ret(&config.den_home, "/", |m| {
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

        Ok((added, skipped))
    })?;

    println!("  {} packages added, {} already tracked", added, skipped);

    // Materialise.
    println!("==> Materialising root environment...");
    let links = env::materialise(&config.den_home, &config.cellar, "/")?;
    println!("  {links} symlinks");
    println!("==> Migration complete. Run `eval \"$(den init)\"` to activate.");

    Ok(())
}

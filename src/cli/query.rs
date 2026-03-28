// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use crate::api::FormulaIndex;
use crate::config::Config;
use crate::{env, keg, link, manifest};

pub(super) fn list_packages(config: &Config, names: &[String]) -> anyhow::Result<()> {
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

pub(super) fn use_version(config: &Config, name: &str) -> anyhow::Result<()> {
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

pub(super) fn show_info(config: &Config, index: &FormulaIndex, name: &str) -> anyhow::Result<()> {
    crate::api::validate_formula_name(name)?;
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

pub(super) fn search_packages(index: &FormulaIndex, text: &str) -> anyhow::Result<()> {
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

pub(super) fn show_deps(index: &FormulaIndex, name: &str, tree: bool) -> anyhow::Result<()> {
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
        if circular { " (already listed)" } else { "" }
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

pub(super) fn cleanup(config: &Config) -> anyhow::Result<()> {
    // 1. GC kegs not referenced by any manifest.
    let all_envs = env::list_envs(&config.den_home)?;
    let mut referenced: std::collections::HashSet<(String, String)> =
        std::collections::HashSet::new();

    for env_path in &all_envs {
        let resolved = manifest::resolve(&config.den_home, env_path)?;
        for (name, version) in &resolved {
            referenced.insert((name.clone(), version.clone()));
        }
    }

    let kegs = keg::list_installed(&config.cellar)?;
    let mut removed_kegs = 0u32;

    for k in &kegs {
        if !referenced.contains(&(k.name.clone(), k.version.clone())) {
            println!("  Removing unreferenced keg: {} {}", k.name, k.version);
            std::fs::remove_dir_all(&k.path)?;
            // Remove empty rack directory if this was the last version.
            if let Some(rack) = k.path.parent() {
                let _ = std::fs::remove_dir(rack); // Fails silently if not empty.
            }
            removed_kegs += 1;
        }
    }

    if removed_kegs > 0 {
        println!("Removed {removed_kegs} unreferenced keg(s).");
    } else {
        println!("No unreferenced kegs to remove.");
    }

    // 2. Remove cached bottles.
    let cache_dir = config.den_home.join("cache").join("bottles");
    if cache_dir.is_dir() {
        let count = std::fs::read_dir(&cache_dir)?.count();
        std::fs::remove_dir_all(&cache_dir)?;
        println!("Removed {count} cached bottle(s).");
    } else {
        println!("No cached bottles to remove.");
    }

    Ok(())
}

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::path::{Path, PathBuf};

use crate::link;
use crate::manifest;

/// Get the materialised environment directory for an env path.
pub fn env_dir(den_home: &Path, env_path: &str) -> PathBuf {
    den_home.join("envs").join(manifest::env_slug(env_path))
}

/// Ensure a materialised environment directory exists.
pub fn ensure_env_dir(den_home: &Path, env_path: &str) -> anyhow::Result<PathBuf> {
    let dir = env_dir(den_home, env_path);
    std::fs::create_dir_all(&dir)?;
    Ok(dir)
}

/// Materialise an environment: resolve the manifest chain and create
/// a flat symlink directory. Returns the number of symlinks created.
pub fn materialise(den_home: &Path, cellar: &Path, env_path: &str) -> anyhow::Result<u32> {
    let resolved = manifest::resolve(den_home, env_path)?;
    let env_dir = ensure_env_dir(den_home, env_path)?;

    // Clear existing links (but preserve .den/ metadata).
    clear_links(&env_dir)?;

    let mut total = 0u32;
    for (name, version) in &resolved {
        let keg_path = cellar.join(name).join(version);
        if !keg_path.is_dir() {
            tracing::warn!("{name} {version} not in Cellar, skipping");
            continue;
        }
        let created = link::link_keg(&keg_path, &env_dir, name)?;
        link::record_linked_version(&env_dir, name, version)?;
        total += created.len() as u32;
    }

    Ok(total)
}

/// Remove all symlinks and the opt/ directory from an environment,
/// but preserve .den/ metadata.
fn clear_links(env_dir: &Path) -> anyhow::Result<()> {
    if !env_dir.is_dir() {
        return Ok(());
    }

    for entry in std::fs::read_dir(env_dir)? {
        let entry = entry?;
        let name = entry.file_name();
        let name_str = name.to_string_lossy();

        // Preserve .den/ metadata directory.
        if name_str == ".den" {
            continue;
        }

        let path = entry.path();
        if path.is_dir() && !path.is_symlink() {
            std::fs::remove_dir_all(&path)?;
        } else {
            std::fs::remove_file(&path)?;
        }
    }

    Ok(())
}

/// List all environment paths (from manifests).
pub fn list_envs(den_home: &Path) -> anyhow::Result<Vec<String>> {
    manifest::list_all(den_home)
}

/// Read the name of the currently active environment path.
pub fn active_env_name(den_home: &Path) -> Option<String> {
    let path = den_home.join("active_env");
    std::fs::read_to_string(path)
        .ok()
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
}

/// Set the active environment path.
pub fn set_active_env(den_home: &Path, env_path: &str) -> anyhow::Result<()> {
    std::fs::create_dir_all(den_home)?;
    let path = den_home.join("active_env");
    let mut tmp = tempfile::NamedTempFile::new_in(den_home)?;
    std::io::Write::write_all(&mut tmp, env_path.as_bytes())?;
    tmp.persist(&path)?;
    Ok(())
}

/// Get the active environment path, defaulting to "/".
pub fn active_env_path(den_home: &Path) -> String {
    active_env_name(den_home).unwrap_or_else(|| "/".to_string())
}

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::path::{Path, PathBuf};

/// Ensure an environment directory exists and return its path.
pub fn ensure_env(den_home: &Path, name: &str) -> anyhow::Result<PathBuf> {
    let env_path = den_home.join("envs").join(name);
    std::fs::create_dir_all(&env_path)?;
    Ok(env_path)
}

/// List all environment names.
pub fn list_envs(den_home: &Path) -> anyhow::Result<Vec<String>> {
    let envs_dir = den_home.join("envs");
    if !envs_dir.is_dir() {
        return Ok(Vec::new());
    }

    let mut names = Vec::new();
    for entry in std::fs::read_dir(envs_dir)? {
        let entry = entry?;
        if entry.path().is_dir() {
            names.push(entry.file_name().to_string_lossy().into_owned());
        }
    }
    names.sort();
    Ok(names)
}

/// Get the path to the default environment, creating it if needed.
pub fn default_env(den_home: &Path) -> anyhow::Result<PathBuf> {
    ensure_env(den_home, "default")
}

/// Read the name of the currently active environment.
pub fn active_env_name(den_home: &Path) -> Option<String> {
    let path = den_home.join("active_env");
    std::fs::read_to_string(path)
        .ok()
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
}

/// Set the active environment.
pub fn set_active_env(den_home: &Path, name: &str) -> anyhow::Result<()> {
    std::fs::create_dir_all(den_home)?;
    std::fs::write(den_home.join("active_env"), name)?;
    Ok(())
}

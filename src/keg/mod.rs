// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::path::{Path, PathBuf};

/// A keg is a specific version of a formula installed in the Cellar.
#[derive(Debug, Clone)]
pub struct Keg {
    pub name: String,
    pub version: String,
    pub path: PathBuf,
}

impl Keg {
    pub fn new(name: String, version: String, path: PathBuf) -> Self {
        Self {
            name,
            version,
            path,
        }
    }
}

/// List all installed kegs in the Cellar.
pub fn list_installed(cellar: &Path) -> anyhow::Result<Vec<Keg>> {
    let mut kegs = Vec::new();

    if !cellar.is_dir() {
        return Ok(kegs);
    }

    for rack_entry in std::fs::read_dir(cellar)? {
        let rack_entry = rack_entry?;
        let rack_path = rack_entry.path();
        if !rack_path.is_dir() {
            continue;
        }

        let formula_name = rack_entry
            .file_name()
            .to_string_lossy()
            .into_owned();

        for keg_entry in std::fs::read_dir(&rack_path)? {
            let keg_entry = keg_entry?;
            let keg_path = keg_entry.path();
            if !keg_path.is_dir() {
                continue;
            }

            let version = keg_entry
                .file_name()
                .to_string_lossy()
                .into_owned();

            kegs.push(Keg::new(formula_name.clone(), version, keg_path));
        }
    }

    kegs.sort_by(|a, b| a.name.cmp(&b.name).then(a.version.cmp(&b.version)));
    Ok(kegs)
}

/// Find all versions of a formula in the Cellar.
pub fn find_versions(cellar: &Path, name: &str) -> anyhow::Result<Vec<Keg>> {
    let rack = cellar.join(name);
    if !rack.is_dir() {
        return Ok(Vec::new());
    }

    let mut kegs = Vec::new();
    for entry in std::fs::read_dir(&rack)? {
        let entry = entry?;
        let path = entry.path();
        if !path.is_dir() {
            continue;
        }
        let version = entry.file_name().to_string_lossy().into_owned();
        kegs.push(Keg::new(name.to_string(), version, path));
    }

    kegs.sort_by(|a, b| a.version.cmp(&b.version));
    Ok(kegs)
}

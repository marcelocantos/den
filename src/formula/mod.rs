// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::collections::HashMap;

use serde::Deserialize;

/// Formula metadata from the Homebrew JSON API.
#[derive(Debug, Deserialize)]
pub struct FormulaInfo {
    pub name: String,
    pub full_name: String,
    #[serde(default)]
    pub desc: Option<String>,
    pub versions: Versions,
    #[serde(default)]
    pub revision: u32,
    pub bottle: BottleSpec,
    #[serde(default)]
    pub dependencies: Vec<String>,
    #[serde(default)]
    pub build_dependencies: Vec<String>,
    #[serde(default)]
    pub keg_only: bool,
    #[serde(default)]
    pub caveats: Option<String>,
    #[serde(default)]
    pub deprecated: bool,
    #[serde(default)]
    pub disabled: bool,
}

impl FormulaInfo {
    /// The package version string including revision suffix (e.g. "3.12.13_2").
    pub fn pkg_version(&self) -> String {
        if self.revision > 0 {
            format!("{}_{}", self.versions.stable, self.revision)
        } else {
            self.versions.stable.clone()
        }
    }
}

#[derive(Debug, Deserialize)]
pub struct Versions {
    pub stable: String,
    #[serde(default)]
    pub head: Option<String>,
    #[serde(default)]
    pub bottle: bool,
}

#[derive(Debug, Deserialize)]
pub struct BottleSpec {
    pub stable: BottleStable,
}

#[derive(Debug, Deserialize)]
pub struct BottleStable {
    #[serde(default)]
    pub rebuild: u32,
    pub root_url: String,
    pub files: HashMap<String, BottleFile>,
}

#[derive(Debug, Deserialize)]
pub struct BottleFile {
    pub cellar: String,
    pub url: String,
    pub sha256: String,
}

/// Convert a formula name to the GHCR repository path component.
/// e.g. "python@3.12" -> "python/3.12", "tree" -> "tree"
pub fn ghcr_path(name: &str) -> String {
    name.replace('@', "/")
}

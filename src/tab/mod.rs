// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::path::Path;

use serde::{Deserialize, Serialize};

/// Minimal INSTALL_RECEIPT.json representation.
#[derive(Debug, Serialize, Deserialize)]
pub struct Tab {
    #[serde(default)]
    pub homebrew_version: String,
    #[serde(default)]
    pub poured_from_bottle: bool,
    #[serde(default)]
    pub loaded_from_api: bool,
    #[serde(default)]
    pub installed_on_request: bool,
    #[serde(default)]
    pub installed_as_dependency: bool,
    #[serde(default)]
    pub time: Option<u64>,
    #[serde(default)]
    pub compiler: String,
    #[serde(default)]
    pub arch: String,
    #[serde(default)]
    pub runtime_dependencies: Vec<RuntimeDep>,
    #[serde(default)]
    pub source: Option<TabSource>,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct RuntimeDep {
    pub full_name: String,
    #[serde(default)]
    pub version: String,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct TabSource {
    #[serde(default)]
    pub spec: String,
    #[serde(default)]
    pub tap: String,
}

/// Read the INSTALL_RECEIPT.json from a keg directory.
pub fn read_tab(keg_path: &Path) -> anyhow::Result<Tab> {
    let receipt_path = keg_path.join("INSTALL_RECEIPT.json");
    let data = std::fs::read_to_string(&receipt_path)?;
    let tab: Tab = serde_json::from_str(&data)?;
    Ok(tab)
}

/// Write a minimal INSTALL_RECEIPT.json for a den-poured bottle.
pub fn write_tab(keg_path: &Path, arch: &str) -> anyhow::Result<()> {
    let tab = Tab {
        homebrew_version: format!("den/{}", env!("CARGO_PKG_VERSION")),
        poured_from_bottle: true,
        loaded_from_api: true,
        installed_on_request: true,
        installed_as_dependency: false,
        time: Some(
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)?
                .as_secs(),
        ),
        compiler: "clang".to_string(),
        arch: arch.to_string(),
        runtime_dependencies: Vec::new(),
        source: Some(TabSource {
            spec: "stable".to_string(),
            tap: "homebrew/core".to_string(),
        }),
    };

    let json = serde_json::to_string_pretty(&tab)?;
    let path = keg_path.join("INSTALL_RECEIPT.json");
    let mut tmp = tempfile::NamedTempFile::new_in(keg_path)?;
    std::io::Write::write_all(&mut tmp, json.as_bytes())?;
    tmp.persist(&path)?;
    Ok(())
}

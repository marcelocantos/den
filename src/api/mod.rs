// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use reqwest::Client;

use crate::formula::FormulaInfo;

const FORMULA_INDEX_URL: &str = "https://formulae.brew.sh/api/formula.json";
#[allow(dead_code)]
const FORMULA_API_BASE: &str = "https://formulae.brew.sh/api/formula";

/// Maximum age of the cached index before auto-refresh (24 hours).
const INDEX_MAX_AGE: std::time::Duration = std::time::Duration::from_secs(24 * 60 * 60);

/// Local formula index — loaded once, used for all lookups.
pub struct FormulaIndex {
    formulas: HashMap<String, FormulaInfo>,
}

impl FormulaIndex {
    /// Load the formula index, using the local cache if fresh enough,
    /// otherwise fetching from the API.
    pub async fn load(client: &Client, cache_dir: &Path) -> anyhow::Result<Self> {
        let cache_path = index_cache_path(cache_dir);

        // Check if cached index is fresh enough.
        if let Some(data) = read_cached_index(&cache_path) {
            match parse_index(&data) {
                Ok(index) => return Ok(index),
                Err(e) => {
                    eprintln!("warning: cached formula index is corrupt, re-fetching: {e}");
                }
            }
        }

        // Fetch from API.
        let data = fetch_index(client).await?;

        // Cache it.
        std::fs::create_dir_all(cache_dir)?;
        std::fs::write(&cache_path, &data)?;

        parse_index(&data)
    }

    /// Force a refresh of the index from the API.
    pub async fn refresh(client: &Client, cache_dir: &Path) -> anyhow::Result<Self> {
        let cache_path = index_cache_path(cache_dir);
        let data = fetch_index(client).await?;
        std::fs::create_dir_all(cache_dir)?;
        std::fs::write(&cache_path, &data)?;
        parse_index(&data)
    }

    /// Look up a formula by name.
    pub fn get(&self, name: &str) -> Option<&FormulaInfo> {
        self.formulas.get(name)
    }

    /// Number of formulae in the index.
    pub fn len(&self) -> usize {
        self.formulas.len()
    }

    /// Returns true if the index contains no formulae.
    pub fn is_empty(&self) -> bool {
        self.formulas.is_empty()
    }

    /// Iterate over all formulae.
    pub fn iter(&self) -> impl Iterator<Item = (&String, &FormulaInfo)> {
        self.formulas.iter()
    }
}

fn index_cache_path(cache_dir: &Path) -> PathBuf {
    cache_dir.join("formula_index.json")
}

fn read_cached_index(path: &Path) -> Option<Vec<u8>> {
    let metadata = std::fs::metadata(path).ok()?;
    let modified = metadata.modified().ok()?;
    let age = std::time::SystemTime::now().duration_since(modified).ok()?;

    if age > INDEX_MAX_AGE {
        return None;
    }

    std::fs::read(path).ok()
}

async fn fetch_index(client: &Client) -> anyhow::Result<Vec<u8>> {
    let response = client
        .get(FORMULA_INDEX_URL)
        .header("User-Agent", concat!("den/", env!("CARGO_PKG_VERSION")))
        .send()
        .await?;

    if !response.status().is_success() {
        anyhow::bail!("failed to fetch formula index (HTTP {})", response.status());
    }

    Ok(response.bytes().await?.to_vec())
}

fn parse_index(data: &[u8]) -> anyhow::Result<FormulaIndex> {
    let entries: Vec<serde_json::Value> = serde_json::from_slice(data)?;
    let mut map = HashMap::new();
    let mut skipped = 0u32;

    for entry in entries {
        match serde_json::from_value::<FormulaInfo>(entry) {
            Ok(info) => {
                map.insert(info.name.clone(), info);
            }
            Err(e) => {
                skipped += 1;
                eprintln!("warning: skipping formula entry: {e}");
            }
        }
    }

    if skipped > 0 {
        eprintln!("warning: skipped {skipped} formula entries due to parse errors");
    }

    Ok(FormulaIndex { formulas: map })
}

/// Validate a formula name contains only safe characters.
pub fn validate_formula_name(name: &str) -> anyhow::Result<()> {
    if name.is_empty() {
        anyhow::bail!("formula name is empty");
    }
    if !name.chars().all(|c| {
        c.is_ascii_alphanumeric() || c == '-' || c == '_' || c == '.' || c == '@' || c == '+'
    }) {
        anyhow::bail!("invalid formula name: {name}");
    }
    Ok(())
}

/// Fetch a single formula from the API (fallback for tap formulae
/// not in the bulk index).
#[allow(dead_code)]
pub async fn fetch_formula(client: &Client, name: &str) -> anyhow::Result<FormulaInfo> {
    validate_formula_name(name)?;
    let url = format!("{FORMULA_API_BASE}/{name}.json");
    let response = client
        .get(&url)
        .header("User-Agent", concat!("den/", env!("CARGO_PKG_VERSION")))
        .send()
        .await?;

    if !response.status().is_success() {
        anyhow::bail!("formula '{}' not found (HTTP {})", name, response.status());
    }

    let info: FormulaInfo = response.json().await?;
    Ok(info)
}

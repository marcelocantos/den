// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use reqwest::Client;

use crate::formula::FormulaInfo;

const API_BASE: &str = "https://formulae.brew.sh/api/formula";

/// Fetch formula metadata from the Homebrew JSON API.
pub async fn fetch_formula(client: &Client, name: &str) -> anyhow::Result<FormulaInfo> {
    let url = format!("{API_BASE}/{name}.json");
    let response = client
        .get(&url)
        .header("User-Agent", "den/0.1.0")
        .send()
        .await?;

    if !response.status().is_success() {
        anyhow::bail!(
            "formula '{}' not found (HTTP {})",
            name,
            response.status()
        );
    }

    let info: FormulaInfo = response.json().await?;
    Ok(info)
}

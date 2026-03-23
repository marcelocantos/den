// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::collections::HashSet;
use std::path::Path;

use reqwest::Client;

use crate::api;
use crate::formula::FormulaInfo;

/// Resolve all transitive runtime dependencies for a formula.
/// Returns formulae in install order (deps before dependents).
pub async fn resolve_install_order(
    client: &Client,
    name: &str,
    cellar: &Path,
) -> anyhow::Result<Vec<FormulaInfo>> {
    let mut visited = HashSet::new();
    let mut order = Vec::new();
    resolve_recursive(client, name, cellar, &mut visited, &mut order).await?;
    Ok(order)
}

/// Post-order DFS: visit deps first so they appear earlier in the output.
/// Box the future to allow recursion in async.
fn resolve_recursive<'a>(
    client: &'a Client,
    name: &'a str,
    cellar: &'a Path,
    visited: &'a mut HashSet<String>,
    order: &'a mut Vec<FormulaInfo>,
) -> std::pin::Pin<Box<dyn std::future::Future<Output = anyhow::Result<()>> + 'a>> {
    Box::pin(async move {
        if visited.contains(name) {
            return Ok(());
        }
        visited.insert(name.to_string());

        let info = api::fetch_formula(client, name).await?;

        // Visit runtime deps first.
        let deps = info.dependencies.clone();
        for dep in &deps {
            resolve_recursive(client, &dep, cellar, visited, order).await?;
        }

        order.push(info);
        Ok(())
    })
}

/// Check which packages from a resolved list still need to be installed.
pub fn filter_missing<'a>(
    formulas: &'a [FormulaInfo],
    cellar: &Path,
) -> Vec<&'a FormulaInfo> {
    formulas
        .iter()
        .filter(|info| {
            let keg = cellar.join(&info.name).join(info.pkg_version());
            !keg.is_dir()
        })
        .collect()
}

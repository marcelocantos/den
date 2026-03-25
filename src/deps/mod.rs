// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::collections::HashSet;
use std::path::Path;

use crate::api::FormulaIndex;
use crate::formula::FormulaInfo;

/// Resolve all transitive runtime dependencies for a formula.
/// Returns formulae in install order (deps before dependents).
/// Uses the local index — no network calls.
pub fn resolve_install_order(index: &FormulaIndex, name: &str) -> anyhow::Result<Vec<FormulaInfo>> {
    let mut visited = HashSet::new();
    let mut order = Vec::new();
    resolve_recursive(index, name, &mut visited, &mut order)?;
    Ok(order)
}

fn resolve_recursive(
    index: &FormulaIndex,
    name: &str,
    visited: &mut HashSet<String>,
    order: &mut Vec<FormulaInfo>,
) -> anyhow::Result<()> {
    if visited.contains(name) {
        return Ok(());
    }
    visited.insert(name.to_string());

    let info = index
        .get(name)
        .ok_or_else(|| anyhow::anyhow!("formula '{}' not found in index", name))?;

    // Visit runtime deps first (post-order = deps before dependents).
    let deps = info.dependencies.clone();
    for dep in &deps {
        resolve_recursive(index, dep, visited, order)?;
    }

    order.push(info.clone());
    Ok(())
}

/// Check which packages from a resolved list still need to be installed.
pub fn filter_missing<'a>(formulas: &'a [FormulaInfo], cellar: &Path) -> Vec<&'a FormulaInfo> {
    formulas
        .iter()
        .filter(|info| {
            let keg = cellar.join(&info.name).join(info.pkg_version());
            !keg.is_dir()
        })
        .collect()
}

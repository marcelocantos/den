// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

/// A manifest declares which packages an environment adds or overrides
/// relative to its parent. The resolved package set is computed by
/// merging from root (/) down to the leaf.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Manifest {
    /// How this manifest was created (e.g. "homebrew-migration").
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub origin: Option<String>,
    /// Packages at this level: formula_name -> keg version.
    #[serde(default)]
    pub packages: BTreeMap<String, String>,
    /// Names of packages that were auto-installed as dependencies.
    /// These are eligible for autoremove when no longer needed.
    #[serde(default, skip_serializing_if = "BTreeSet::is_empty")]
    pub auto: BTreeSet<String>,
    /// Cask tokens installed at this level: token -> version.
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub casks: BTreeMap<String, String>,
}

/// Encode a single path component for use in a slug.
/// Dashes are encoded as `%2D` so that `--` is unambiguous as the separator.
fn encode_component(component: &str) -> String {
    component.replace('%', "%25").replace('-', "%2D")
}

/// Decode a single slug component back to the original path component.
fn decode_component(component: &str) -> String {
    component.replace("%2D", "-").replace("%25", "%")
}

/// Convert an environment path (e.g. "/work/ml") to a filesystem slug
/// for the materialised environment directory.
pub fn env_slug(env_path: &str) -> String {
    let trimmed = env_path.trim_matches('/');
    if trimmed.is_empty() {
        "ROOT".to_string()
    } else {
        trimmed
            .split('/')
            .map(encode_component)
            .collect::<Vec<_>>()
            .join("--")
    }
}

/// Convert a slug back to an environment path.
pub fn slug_to_path(slug: &str) -> String {
    if slug == "ROOT" {
        "/".to_string()
    } else {
        let components: Vec<String> = slug.split("--").map(decode_component).collect();
        format!("/{}", components.join("/"))
    }
}

/// Return the parent environment path, or None if this is root.
/// "/work/ml" -> "/work", "/ml" -> "/", "/" -> None
pub fn parent_path(env_path: &str) -> Option<String> {
    if env_path == "/" {
        return None;
    }
    let trimmed = env_path.trim_end_matches('/');
    match trimmed.rfind('/') {
        Some(0) => Some("/".to_string()),
        Some(i) => Some(trimmed[..i].to_string()),
        None => Some("/".to_string()),
    }
}

/// Return the chain of environment paths from root to the given path.
/// "/work/ml" -> ["/", "/work", "/work/ml"]
pub fn ancestor_chain(env_path: &str) -> Vec<String> {
    let mut chain = Vec::new();
    let mut current = env_path.to_string();
    loop {
        chain.push(current.clone());
        match parent_path(&current) {
            Some(parent) => current = parent,
            None => break,
        }
    }
    chain.reverse();
    chain
}

/// Path to the manifest file for a given environment path.
fn manifest_file(den_home: &Path, env_path: &str) -> PathBuf {
    let trimmed = env_path.trim_matches('/');
    if trimmed.is_empty() {
        den_home.join("manifests").join("manifest.json")
    } else {
        den_home
            .join("manifests")
            .join(trimmed)
            .join("manifest.json")
    }
}

/// Read a manifest file. Returns a default (empty) manifest if the file
/// doesn't exist.
pub fn read_manifest(den_home: &Path, env_path: &str) -> anyhow::Result<Manifest> {
    let path = manifest_file(den_home, env_path);
    if !path.exists() {
        return Ok(Manifest::default());
    }
    let data = std::fs::read_to_string(&path)?;
    let manifest: Manifest = serde_json::from_str(&data)?;
    Ok(manifest)
}

/// Write a manifest file, creating parent directories as needed.
pub fn write_manifest(den_home: &Path, env_path: &str, manifest: &Manifest) -> anyhow::Result<()> {
    let path = manifest_file(den_home, env_path);
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let json = serde_json::to_string_pretty(manifest)?;
    std::fs::write(path, json)?;
    Ok(())
}

/// Resolve the full package set for an environment by merging from root
/// to leaf. Child entries override parent entries.
pub fn resolve(den_home: &Path, env_path: &str) -> anyhow::Result<BTreeMap<String, String>> {
    let chain = ancestor_chain(env_path);
    let mut resolved = BTreeMap::new();

    for ancestor in &chain {
        let manifest = read_manifest(den_home, ancestor)?;
        for (name, version) in manifest.packages {
            resolved.insert(name, version);
        }
    }

    Ok(resolved)
}

/// Check whether a manifest exists for the given environment path.
pub fn manifest_exists(den_home: &Path, env_path: &str) -> bool {
    manifest_file(den_home, env_path).exists()
}

/// List all environment paths that have manifests.
pub fn list_all(den_home: &Path) -> anyhow::Result<Vec<String>> {
    let manifests_dir = den_home.join("manifests");
    if !manifests_dir.is_dir() {
        return Ok(Vec::new());
    }

    let mut paths = Vec::new();
    collect_manifests(&manifests_dir, &manifests_dir, &mut paths)?;
    paths.sort();
    Ok(paths)
}

fn collect_manifests(base: &Path, dir: &Path, paths: &mut Vec<String>) -> anyhow::Result<()> {
    if !dir.is_dir() {
        return Ok(());
    }

    // Check for manifest.json in this directory.
    if dir.join("manifest.json").exists() {
        let relative = dir.strip_prefix(base).unwrap_or(Path::new(""));
        let env_path = if relative == Path::new("") {
            "/".to_string()
        } else {
            format!("/{}", relative.to_string_lossy())
        };
        paths.push(env_path);
    }

    // Recurse into subdirectories (without following symlinks).
    for entry in std::fs::read_dir(dir)? {
        let entry = entry?;
        let meta = std::fs::symlink_metadata(entry.path())?;
        if meta.file_type().is_dir() {
            collect_manifests(base, &entry.path(), paths)?;
        }
    }

    Ok(())
}

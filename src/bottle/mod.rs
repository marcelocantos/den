// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::io::Read;
use std::path::{Path, PathBuf};

use reqwest::Client;
use serde::Deserialize;
use sha2::{Digest, Sha256};

use crate::formula::BottleFile;

#[derive(Deserialize)]
struct GhcrToken {
    token: String,
}

/// Fetch a bottle, using the content-addressed cache if available.
/// Cache key is the SHA256 digest — the same hash we verify against.
pub async fn fetch_bottle(
    client: &Client,
    bottle: &BottleFile,
    ghcr_repo_path: &str,
    cache_dir: &Path,
) -> anyhow::Result<Vec<u8>> {
    let cache_path = cache_dir.join(&bottle.sha256);

    // Check cache first.
    if cache_path.is_file() {
        let data = std::fs::read(&cache_path)?;
        // Verify integrity (cache could be corrupted/truncated).
        let mut hasher = Sha256::new();
        hasher.update(&data);
        let digest = format!("{:x}", hasher.finalize());
        if digest == bottle.sha256 {
            return Ok(data);
        }
        // Corrupted — re-download.
        let _ = std::fs::remove_file(&cache_path);
    }

    let data = download_bottle(client, bottle, ghcr_repo_path).await?;

    // Write to cache.
    std::fs::create_dir_all(cache_dir)?;
    std::fs::write(&cache_path, &data)?;

    Ok(data)
}

/// Download a bottle from GHCR, verify its SHA256, and return the raw bytes.
pub async fn download_bottle(
    client: &Client,
    bottle: &BottleFile,
    ghcr_repo_path: &str,
) -> anyhow::Result<Vec<u8>> {
    // Fetch anonymous GHCR token.
    let token_url = format!(
        "https://ghcr.io/token?service=ghcr.io&scope=repository:homebrew/core/{}:pull",
        ghcr_repo_path
    );
    let token: GhcrToken = client
        .get(&token_url)
        .header("User-Agent", "den/0.1.0")
        .send()
        .await?
        .json()
        .await?;

    // Download the bottle blob.
    let response = client
        .get(&bottle.url)
        .header("User-Agent", "den/0.1.0")
        .header("Authorization", format!("Bearer {}", token.token))
        .send()
        .await?;

    if !response.status().is_success() {
        anyhow::bail!("bottle download failed (HTTP {})", response.status());
    }

    let bytes = response.bytes().await?.to_vec();

    // Verify SHA256.
    let mut hasher = Sha256::new();
    hasher.update(&bytes);
    let digest = format!("{:x}", hasher.finalize());

    if digest != bottle.sha256 {
        anyhow::bail!(
            "SHA256 mismatch: expected {}, got {}",
            bottle.sha256,
            digest
        );
    }

    Ok(bytes)
}

/// Pour (extract) a bottle tarball into the Cellar directory.
/// Returns the path to the newly created keg.
pub fn pour_bottle(bottle_data: &[u8], cellar: &Path) -> anyhow::Result<PathBuf> {
    let decoder = flate2::read::GzDecoder::new(bottle_data);
    let mut archive = tar::Archive::new(decoder);

    // Peek at the first entry to determine the keg path (name/version/).
    let mut keg_prefix: Option<PathBuf> = None;

    for entry in archive.entries()? {
        let mut entry = entry?;
        let path = entry.path()?.into_owned();

        // Reject paths that could escape the target directory.
        if path.components().any(|c| matches!(c, std::path::Component::ParentDir)) {
            anyhow::bail!(
                "bottle contains path traversal: {}",
                path.display()
            );
        }

        // The bottle tarball contains paths like "tree/2.3.1/bin/tree".
        // Extract the first two components as the keg prefix.
        if keg_prefix.is_none() {
            let components: Vec<_> = path.components().take(2).collect();
            if components.len() == 2 {
                let prefix: PathBuf = components.iter().collect();
                keg_prefix = Some(prefix);
            }
        }

        let dest = cellar.join(&path);

        // Create parent directories.
        if let Some(parent) = dest.parent() {
            std::fs::create_dir_all(parent)?;
        }

        // Extract the entry.
        if entry.header().entry_type().is_dir() {
            std::fs::create_dir_all(&dest)?;
        } else if entry.header().entry_type().is_symlink() {
            if let Some(link_target) = entry.link_name()? {
                // Reject symlink targets that escape the Cellar.
                if link_target
                    .components()
                    .any(|c| matches!(c, std::path::Component::ParentDir))
                {
                    anyhow::bail!(
                        "bottle contains symlink with path traversal: {} -> {}",
                        path.display(),
                        link_target.display()
                    );
                }
                // Remove existing symlink if present.
                let _ = std::fs::remove_file(&dest);
                std::os::unix::fs::symlink(link_target.as_ref(), &dest)?;
            }
        } else {
            let mut data = Vec::new();
            entry.read_to_end(&mut data)?;
            std::fs::write(&dest, &data)?;

            // Preserve executable permission.
            let mode = entry.header().mode()?;
            set_permissions(&dest, mode)?;
        }
    }

    let keg_prefix =
        keg_prefix.ok_or_else(|| anyhow::anyhow!("empty or malformed bottle tarball"))?;

    Ok(cellar.join(keg_prefix))
}

#[cfg(unix)]
fn set_permissions(path: &Path, mode: u32) -> anyhow::Result<()> {
    use std::os::unix::fs::PermissionsExt;
    let perms = std::fs::Permissions::from_mode(mode);
    std::fs::set_permissions(path, perms)?;
    Ok(())
}

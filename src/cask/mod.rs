// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::io::Write;
use std::path::{Path, PathBuf};

use futures_util::StreamExt;
use reqwest::Client;
use serde::Deserialize;
use sha2::{Digest, Sha256};

const CASK_API_BASE: &str = "https://formulae.brew.sh/api/cask";

/// Maximum allowed download size (2 GB).
const MAX_DOWNLOAD_SIZE: u64 = 2 * 1024 * 1024 * 1024;

/// Result of a successful cask download: the path to the temp file, its size, and the format.
pub struct DownloadedCask {
    pub path: PathBuf,
    pub size: u64,
    pub ext: String,
}

/// RAII guard that detaches a mounted DMG when dropped, preventing mount leaks
/// if an error occurs mid-function.
struct DmgMount<'a> {
    mount_path: &'a Path,
    detached: bool,
}

impl<'a> DmgMount<'a> {
    fn new(mount_path: &'a Path) -> Self {
        Self {
            mount_path,
            detached: false,
        }
    }

    /// Explicitly detach (for normal flow). Prevents double-detach in Drop.
    fn detach(&mut self) {
        if !self.detached {
            let _ = std::process::Command::new("hdiutil")
                .args(["detach", "-quiet"])
                .arg(self.mount_path)
                .status();
            self.detached = true;
        }
    }
}

impl Drop for DmgMount<'_> {
    fn drop(&mut self) {
        self.detach();
    }
}

#[derive(Debug, Deserialize)]
#[allow(dead_code)]
pub struct CaskInfo {
    pub token: String,
    #[serde(default)]
    pub name: Vec<String>,
    #[serde(default)]
    pub desc: Option<String>,
    pub url: String,
    pub version: String,
    #[serde(default)]
    pub sha256: Option<String>,
    #[serde(default)]
    pub artifacts: Vec<serde_json::Value>,
    #[serde(default)]
    pub caveats: Option<String>,
    #[serde(default)]
    pub auto_updates: bool,
}

impl CaskInfo {
    /// Extract app artifact names from the artifacts list.
    pub fn app_artifacts(&self) -> Vec<String> {
        let mut apps = Vec::new();
        for artifact in &self.artifacts {
            if let Some(obj) = artifact.as_object()
                && let Some(app_val) = obj.get("app")
                && let Some(arr) = app_val.as_array()
            {
                for item in arr {
                    if let Some(s) = item.as_str() {
                        apps.push(s.to_string());
                    }
                }
            }
        }
        apps
    }

    /// Extract binary artifact specs from the artifacts list.
    #[allow(dead_code)]
    pub fn binary_artifacts(&self) -> Vec<String> {
        let mut bins = Vec::new();
        for artifact in &self.artifacts {
            if let Some(obj) = artifact.as_object()
                && let Some(bin_val) = obj.get("binary")
                && let Some(arr) = bin_val.as_array()
            {
                for item in arr {
                    if let Some(s) = item.as_str() {
                        bins.push(s.to_string());
                    }
                }
            }
        }
        bins
    }
}

/// Fetch cask metadata from the Homebrew JSON API.
pub async fn fetch_cask(client: &Client, token: &str) -> anyhow::Result<CaskInfo> {
    // Validate token to prevent URL path traversal.
    if token.contains('/') || token.contains("..") || token.is_empty() {
        anyhow::bail!("invalid cask token: {token}");
    }
    let url = format!("{CASK_API_BASE}/{token}.json");
    let response = client
        .get(&url)
        .header("User-Agent", concat!("den/", env!("CARGO_PKG_VERSION")))
        .send()
        .await?;

    if !response.status().is_success() {
        anyhow::bail!("cask '{}' not found (HTTP {})", token, response.status());
    }

    let info: CaskInfo = response.json().await?;
    Ok(info)
}

/// Download a cask artifact via streaming, verify SHA256.
/// Returns the path to a temp file containing the verified download, its size, and format.
pub async fn download_cask(info: &CaskInfo) -> anyhow::Result<DownloadedCask> {
    // Enforce HTTPS.
    if !info.url.starts_with("https://") {
        anyhow::bail!("refusing non-HTTPS cask URL: {}", info.url);
    }

    // Require SHA256 verification.
    if info.sha256.as_deref() == Some("no_check") || info.sha256.is_none() {
        anyhow::bail!(
            "cask '{}' has no SHA256 checksum — refusing to install unverified artifact",
            info.token
        );
    }

    // TODO: Create this client once and reuse across downloads for connection pooling.
    // Only follow HTTPS redirects to prevent downgrade attacks.
    let https_only_client = reqwest::Client::builder()
        .redirect(reqwest::redirect::Policy::custom(|attempt| {
            if attempt.url().scheme() == "https" {
                attempt.follow()
            } else {
                attempt.stop()
            }
        }))
        .timeout(std::time::Duration::from_secs(300))
        .build()?;
    let response = https_only_client
        .get(&info.url)
        .header("User-Agent", concat!("den/", env!("CARGO_PKG_VERSION")))
        .send()
        .await?;

    if !response.status().is_success() {
        anyhow::bail!("cask download failed (HTTP {})", response.status());
    }

    // Reject excessively large downloads before streaming.
    if let Some(len) = response.content_length()
        && len > MAX_DOWNLOAD_SIZE
    {
        anyhow::bail!(
            "cask download too large ({} bytes, max {} bytes)",
            len,
            MAX_DOWNLOAD_SIZE
        );
    }

    // Determine file extension from URL.
    let ext = info
        .url
        .rsplit('/')
        .next()
        .and_then(|f| {
            if f.ends_with(".dmg") {
                Some("dmg")
            } else if f.ends_with(".zip") {
                Some("zip")
            } else if f.ends_with(".pkg") {
                Some("pkg")
            } else {
                // Check content-type as fallback.
                None
            }
        })
        .unwrap_or("dmg")
        .to_string();

    // Stream the response to a temp file while computing SHA256 incrementally.
    let mut tmp = tempfile::NamedTempFile::new()?;
    let mut hasher = Sha256::new();
    let mut total_bytes: u64 = 0;
    let mut stream = response.bytes_stream();

    while let Some(chunk) = stream.next().await {
        let chunk = chunk?;
        total_bytes += chunk.len() as u64;
        if total_bytes > MAX_DOWNLOAD_SIZE {
            anyhow::bail!(
                "cask download too large ({} bytes so far, max {} bytes)",
                total_bytes,
                MAX_DOWNLOAD_SIZE
            );
        }
        hasher.update(&chunk);
        tmp.write_all(&chunk)?;
    }
    tmp.flush()?;

    // Verify SHA256.
    if let Some(ref expected) = info.sha256
        && expected != "no_check"
    {
        let digest = format!("{:x}", hasher.finalize());
        if digest != *expected {
            anyhow::bail!("SHA256 mismatch: expected {}, got {}", expected, digest);
        }
    }

    let path = tmp
        .into_temp_path()
        .keep()
        .map_err(|_| anyhow::anyhow!("failed to persist temp file"))?;

    Ok(DownloadedCask {
        path,
        size: total_bytes,
        ext,
    })
}

/// Install a cask from a downloaded DMG file on disk.
pub fn install_from_dmg(
    dmg_path: &Path,
    apps: &[String],
    appdir: &Path,
) -> anyhow::Result<Vec<PathBuf>> {
    let mount_dir = tempfile::tempdir()?;
    let mount_path = mount_dir.path();

    // Mount the DMG.
    let status = std::process::Command::new("hdiutil")
        .args(["attach", "-nobrowse", "-readonly", "-mountpoint"])
        .arg(mount_path)
        .arg(dmg_path)
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()?;

    if !status.success() {
        anyhow::bail!("failed to mount DMG");
    }

    // RAII guard ensures the DMG is detached even if we return early on error.
    let mut dmg_guard = DmgMount::new(mount_path);

    let mut installed = Vec::new();

    // Copy app bundles.
    for app_name in apps {
        validate_artifact_name(app_name)?;
        let src = mount_path.join(app_name);
        if !src.exists() {
            // Try finding the app in the mount root.
            let found = find_app_in_dir(mount_path, app_name);
            if let Some(found_path) = found {
                let dest = appdir.join(app_name);
                copy_app(&found_path, &dest)?;
                installed.push(dest);
            } else {
                tracing::warn!("app '{}' not found in DMG", app_name);
            }
        } else {
            let dest = appdir.join(app_name);
            copy_app(&src, &dest)?;
            installed.push(dest);
        }
    }

    // Explicitly detach (also happens in Drop, but be intentional).
    dmg_guard.detach();

    Ok(installed)
}

/// Install a cask from a downloaded ZIP file on disk.
pub fn install_from_zip(
    zip_path: &Path,
    apps: &[String],
    appdir: &Path,
) -> anyhow::Result<Vec<PathBuf>> {
    let tmp_dir = tempfile::tempdir()?;

    // Extract.
    let status = std::process::Command::new("unzip")
        .args(["-q", "-o"])
        .arg(zip_path)
        .arg("-d")
        .arg(tmp_dir.path())
        .status()?;

    if !status.success() {
        anyhow::bail!("failed to extract ZIP");
    }

    // Validate extracted paths stay within the sandbox directory.
    let canonical_tmp = std::fs::canonicalize(tmp_dir.path())?;
    for entry in walkdir::WalkDir::new(tmp_dir.path()).follow_links(false) {
        let entry = entry?;
        let canonical = std::fs::canonicalize(entry.path())?;
        if !canonical.starts_with(&canonical_tmp) {
            anyhow::bail!("ZIP extraction escaped sandbox: {}", entry.path().display());
        }
    }

    let mut installed = Vec::new();

    for app_name in apps {
        validate_artifact_name(app_name)?;
        let src = tmp_dir.path().join(app_name);
        if src.exists() {
            let dest = appdir.join(app_name);
            copy_app(&src, &dest)?;
            installed.push(dest);
        } else if let Some(found) = find_app_in_dir(tmp_dir.path(), app_name) {
            let dest = appdir.join(app_name);
            copy_app(&found, &dest)?;
            installed.push(dest);
        } else {
            tracing::warn!("app '{}' not found in ZIP", app_name);
        }
    }

    Ok(installed)
}

fn find_app_in_dir(dir: &Path, app_name: &str) -> Option<PathBuf> {
    for entry in walkdir::WalkDir::new(dir).max_depth(3).follow_links(false) {
        if let Ok(entry) = entry
            && entry.file_name().to_string_lossy() == app_name
        {
            return Some(entry.into_path());
        }
    }
    None
}

/// Validate artifact names from the API don't contain path traversal.
fn validate_artifact_name(name: &str) -> anyhow::Result<()> {
    if name.is_empty() {
        anyhow::bail!("empty artifact name");
    }
    if !name
        .chars()
        .all(|c| c.is_ascii_alphanumeric() || c == '.' || c == '-' || c == '_' || c == ' ')
    {
        anyhow::bail!("unsafe artifact name: {name}");
    }
    if name.contains("..") {
        anyhow::bail!("artifact name contains path traversal: {name}");
    }
    Ok(())
}

fn copy_app(src: &Path, dest: &Path) -> anyhow::Result<()> {
    // Remove existing app if present.
    if dest.exists() {
        std::fs::remove_dir_all(dest)?;
    }
    // Use cp -R for app bundles (preserves symlinks, permissions).
    let status = std::process::Command::new("cp")
        .args(["-R"])
        .arg(src)
        .arg(dest)
        .status()?;
    if !status.success() {
        anyhow::bail!("failed to copy {} to {}", src.display(), dest.display());
    }
    Ok(())
}

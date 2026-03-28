// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

//! Tap management — third-party formula repositories.
//!
//! A tap is a git repository at `github.com/{user}/homebrew-{name}`
//! containing Ruby formula files in a `Formula/` directory.

mod parser;

use std::path::{Path, PathBuf};

pub use parser::{InstallStep, PlatformOverride, TapFormula};

use crate::platform::Arch;

/// Maximum allowed tarball download size for tap formulae (2 GB).
const MAX_DOWNLOAD_SIZE: u64 = 2 * 1024 * 1024 * 1024;

/// Parse a tap name like "user/repo" into (user, repo).
/// The repo may or may not have the `homebrew-` prefix — we normalise it.
fn parse_tap_name(name: &str) -> anyhow::Result<(String, String)> {
    let parts: Vec<&str> = name.split('/').collect();
    if parts.len() != 2 {
        anyhow::bail!("invalid tap name '{name}': expected 'user/repo' (e.g. 'marcelocantos/tap')");
    }
    let user = parts[0];
    let repo = parts[1];

    // Validate components to prevent path traversal.
    for component in [user, repo] {
        if component.is_empty() || component == "." || component == ".." {
            anyhow::bail!("invalid tap name component: '{component}'");
        }
        if !component
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_')
        {
            anyhow::bail!("invalid tap name component: '{component}'");
        }
    }

    Ok((user.to_string(), repo.to_string()))
}

/// Return the directory where a tap is stored.
/// e.g. `{den_home}/taps/marcelocantos/homebrew-tap/`
fn tap_dir(den_home: &Path, user: &str, repo: &str) -> PathBuf {
    let dir_name = if repo.starts_with("homebrew-") {
        repo.to_string()
    } else {
        format!("homebrew-{repo}")
    };
    den_home.join("taps").join(user).join(dir_name)
}

/// Return the GitHub clone URL for a tap.
fn tap_clone_url(user: &str, repo: &str) -> String {
    let repo_name = if repo.starts_with("homebrew-") {
        repo.to_string()
    } else {
        format!("homebrew-{repo}")
    };
    format!("https://github.com/{user}/{repo_name}.git")
}

/// Add (clone) a tap.
pub fn add_tap(den_home: &Path, name: &str) -> anyhow::Result<()> {
    let (user, repo) = parse_tap_name(name)?;
    let dir = tap_dir(den_home, &user, &repo);

    if dir.is_dir() {
        anyhow::bail!("tap '{name}' is already installed at {}", dir.display());
    }

    let url = tap_clone_url(&user, &repo);

    // Enforce HTTPS only.
    if !url.starts_with("https://") {
        anyhow::bail!("refusing non-HTTPS tap URL: {url}");
    }

    println!("==> Tapping {name}...");
    println!("  Cloning {url}");

    if let Some(parent) = dir.parent() {
        std::fs::create_dir_all(parent)?;
    }

    let output = std::process::Command::new("git")
        .args(["clone", "--depth", "1", &url])
        .arg(&dir)
        .output()?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        anyhow::bail!("git clone failed: {stderr}");
    }

    let formula_count = count_formulae(&dir);
    println!("==> Tapped {name} ({formula_count} formulae).");
    Ok(())
}

/// Remove a tap.
pub fn remove_tap(den_home: &Path, name: &str) -> anyhow::Result<()> {
    let (user, repo) = parse_tap_name(name)?;
    let dir = tap_dir(den_home, &user, &repo);

    if !dir.is_dir() {
        anyhow::bail!("tap '{name}' is not installed");
    }

    // Verify the directory is within the expected taps directory.
    let taps_root = den_home.join("taps");
    if !dir.starts_with(&taps_root) {
        anyhow::bail!("tap directory escapes taps root: {}", dir.display());
    }

    std::fs::remove_dir_all(&dir)?;

    // Clean up empty parent directory.
    let user_dir = taps_root.join(&user);
    if user_dir.is_dir() {
        let is_empty = std::fs::read_dir(&user_dir)?.next().is_none();
        if is_empty {
            let _ = std::fs::remove_dir(&user_dir);
        }
    }

    println!("==> Untapped {name}.");
    Ok(())
}

/// List installed taps, returning names like "user/repo".
pub fn list_taps(den_home: &Path) -> anyhow::Result<Vec<String>> {
    let taps_dir = den_home.join("taps");
    if !taps_dir.is_dir() {
        return Ok(Vec::new());
    }

    let mut taps = Vec::new();

    for user_entry in std::fs::read_dir(&taps_dir)? {
        let user_entry = user_entry?;
        if !user_entry.file_type()?.is_dir() {
            continue;
        }
        let user = user_entry.file_name().to_string_lossy().to_string();

        for repo_entry in std::fs::read_dir(user_entry.path())? {
            let repo_entry = repo_entry?;
            if !repo_entry.file_type()?.is_dir() {
                continue;
            }
            let repo_dir_name = repo_entry.file_name().to_string_lossy().to_string();
            let short_name = repo_dir_name
                .strip_prefix("homebrew-")
                .unwrap_or(&repo_dir_name);
            taps.push(format!("{user}/{short_name}"));
        }
    }

    taps.sort();
    Ok(taps)
}

/// Update a tap (git pull).
pub fn update_tap(den_home: &Path, name: &str) -> anyhow::Result<()> {
    let (user, repo) = parse_tap_name(name)?;
    let dir = tap_dir(den_home, &user, &repo);

    if !dir.is_dir() {
        anyhow::bail!("tap '{name}' is not installed");
    }

    println!("==> Updating {name}...");
    let output = std::process::Command::new("git")
        .args(["pull", "--ff-only"])
        .current_dir(&dir)
        .output()?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        anyhow::bail!("git pull failed for {name}: {stderr}");
    }

    println!("==> {name} updated.");
    Ok(())
}

/// List all formulae in a tap (parsed from Ruby files).
pub fn list_tap_formulae(den_home: &Path, name: &str) -> anyhow::Result<Vec<TapFormula>> {
    let (user, repo) = parse_tap_name(name)?;
    let dir = tap_dir(den_home, &user, &repo);

    if !dir.is_dir() {
        anyhow::bail!("tap '{name}' is not installed");
    }

    let formula_dir = dir.join("Formula");
    if !formula_dir.is_dir() {
        return Ok(Vec::new());
    }

    let mut formulae = Vec::new();
    for entry in std::fs::read_dir(&formula_dir)? {
        let entry = entry?;
        let path = entry.path();
        if path.extension().is_some_and(|e| e == "rb") {
            match parser::parse_formula_file(&path) {
                Ok(formula) => formulae.push(formula),
                Err(e) => {
                    tracing::warn!("skipping {}: {e}", path.display());
                }
            }
        }
    }

    formulae.sort_by(|a, b| a.name.cmp(&b.name));
    Ok(formulae)
}

/// Find a specific formula in a tap.
pub fn find_tap_formula(
    den_home: &Path,
    tap_name: &str,
    formula_name: &str,
) -> anyhow::Result<TapFormula> {
    let (user, repo) = parse_tap_name(tap_name)?;
    let dir = tap_dir(den_home, &user, &repo);

    if !dir.is_dir() {
        anyhow::bail!("tap '{tap_name}' is not installed");
    }

    let formula_file = dir.join("Formula").join(format!("{formula_name}.rb"));
    if !formula_file.is_file() {
        anyhow::bail!("formula '{formula_name}' not found in tap '{tap_name}'");
    }

    parser::parse_formula_file(&formula_file)
}

/// Resolve the platform-appropriate URL and SHA256 for a tap formula.
pub fn resolve_for_platform(formula: &TapFormula, arch: Arch) -> anyhow::Result<(String, String)> {
    let os = std::env::consts::OS;
    let os_match = match os {
        "macos" => "macos",
        "linux" => "linux",
        _ => "",
    };
    let arch_match = match arch {
        Arch::Arm64 => "arm",
        Arch::X86_64 => "intel",
    };

    // Look for the most specific override first: os+arch, then os-only.
    let mut best: Option<&PlatformOverride> = None;
    for ov in &formula.platform_overrides {
        let os_ok = ov.os.as_deref().is_none_or(|o| o == os_match);
        let arch_ok = ov.arch.as_deref().is_none_or(|a| a == arch_match);
        if os_ok && arch_ok {
            // Prefer more-specific overrides (both os and arch set).
            let specificity = ov.os.is_some() as u8 + ov.arch.is_some() as u8;
            let best_specificity = best
                .map(|b| b.os.is_some() as u8 + b.arch.is_some() as u8)
                .unwrap_or(0);
            if specificity > best_specificity {
                best = Some(ov);
            }
        }
    }

    if let Some(ov) = best {
        Ok((ov.url.clone(), ov.sha256.clone()))
    } else {
        // Fall back to top-level URL/SHA256.
        Ok((formula.url.clone(), formula.sha256.clone()))
    }
}

/// Download and verify a tap formula tarball.
pub async fn download_tap_formula(
    _client: &reqwest::Client,
    url: &str,
    expected_sha256: &str,
    cache_dir: &Path,
) -> anyhow::Result<Vec<u8>> {
    // Enforce HTTPS.
    if !url.starts_with("https://") {
        anyhow::bail!("refusing non-HTTPS download URL: {url}");
    }

    // Check cache first (content-addressed).
    let cache_path = cache_dir.join(expected_sha256);
    if cache_path.is_file() {
        let data = std::fs::read(&cache_path)?;
        let digest = sha256_hex(&data);
        if digest == expected_sha256 {
            return Ok(data);
        }
        // Corrupted — re-download.
        let _ = std::fs::remove_file(&cache_path);
    }

    // Download with HTTPS-only redirect policy.
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
        .get(url)
        .header("User-Agent", concat!("den/", env!("CARGO_PKG_VERSION")))
        .send()
        .await?;

    if !response.status().is_success() {
        anyhow::bail!("download failed (HTTP {}): {url}", response.status());
    }

    // Reject excessively large downloads.
    if let Some(len) = response.content_length()
        && len > MAX_DOWNLOAD_SIZE
    {
        anyhow::bail!("download too large ({len} bytes, max {MAX_DOWNLOAD_SIZE} bytes)");
    }

    let bytes = response.bytes().await?.to_vec();

    if bytes.len() as u64 > MAX_DOWNLOAD_SIZE {
        anyhow::bail!(
            "download too large ({} bytes, max {MAX_DOWNLOAD_SIZE} bytes)",
            bytes.len()
        );
    }

    // Verify SHA256.
    let digest = sha256_hex(&bytes);
    if digest != expected_sha256 {
        anyhow::bail!("SHA256 mismatch: expected {expected_sha256}, got {digest}");
    }

    // Write to cache atomically.
    std::fs::create_dir_all(cache_dir)?;
    let mut tmp = tempfile::NamedTempFile::new_in(cache_dir)?;
    std::io::Write::write_all(&mut tmp, &bytes)?;
    tmp.persist(&cache_path)?;
    crate::trust::set_file_permissions_0600(&cache_path);

    Ok(bytes)
}

/// Extract a tap formula tarball into a keg directory and execute install steps.
pub fn install_tap_formula_keg(
    tarball_data: &[u8],
    cellar: &Path,
    formula: &TapFormula,
) -> anyhow::Result<PathBuf> {
    let keg_path = cellar.join(&formula.name).join(&formula.version);

    if keg_path.is_dir() {
        return Ok(keg_path);
    }

    // Extract to a temp directory first, then move into place.
    let tmp_dir = tempfile::tempdir_in(cellar)?;
    let extract_dir = tmp_dir.path();

    let decoder = flate2::read::GzDecoder::new(tarball_data);
    let mut archive = tar::Archive::new(decoder);

    for entry in archive.entries()? {
        let mut entry = entry?;
        let path = entry.path()?.into_owned();

        // Safety checks (same as bottle module).
        if path.is_absolute() {
            anyhow::bail!("tarball contains absolute path: {}", path.display());
        }
        if path
            .components()
            .any(|c| matches!(c, std::path::Component::ParentDir))
        {
            anyhow::bail!("tarball contains path traversal: {}", path.display());
        }
        if entry.header().entry_type().is_hard_link() {
            anyhow::bail!("tarball contains hardlink (rejected): {}", path.display());
        }

        let dest = extract_dir.join(&path);
        if !dest.starts_with(extract_dir) {
            anyhow::bail!("tarball entry escapes extract dir: {}", path.display());
        }

        if let Some(parent) = dest.parent() {
            std::fs::create_dir_all(parent)?;
        }

        if entry.header().entry_type().is_dir() {
            std::fs::create_dir_all(&dest)?;
        } else if entry.header().entry_type().is_symlink() {
            if let Some(link_target) = entry.link_name()? {
                let _ = std::fs::remove_file(&dest);
                std::os::unix::fs::symlink(link_target.as_ref(), &dest)?;
            }
        } else {
            let mut file = std::fs::File::create(&dest)?;
            std::io::copy(&mut entry, &mut file)?;

            let mode = entry.header().mode()? & 0o0777;
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                std::fs::set_permissions(&dest, std::fs::Permissions::from_mode(mode))?;
            }
        }
    }

    // Now execute install steps: copy files from the extracted tarball
    // into the keg directory structure.
    std::fs::create_dir_all(&keg_path)?;

    // Find the content root — many tarballs have a single top-level directory.
    let content_root = find_content_root(extract_dir)?;

    for step in &formula.install_steps {
        execute_install_step(step, &content_root, &keg_path)?;
    }

    // If no install steps were specified, fall back to copying everything
    // into the keg's bin/ directory if there are executable files.
    if formula.install_steps.is_empty() {
        let bin_dir = keg_path.join("bin");
        std::fs::create_dir_all(&bin_dir)?;
        copy_executables(&content_root, &bin_dir)?;
    }

    // Write INSTALL_RECEIPT.json.
    crate::tab::write_tab(
        &keg_path,
        &std::env::consts::ARCH.replace("aarch64", "arm64"),
    )?;

    Ok(keg_path)
}

/// Find the content root of an extracted tarball.
/// If there's a single top-level directory, use that. Otherwise, use the
/// extract directory itself.
fn find_content_root(extract_dir: &Path) -> anyhow::Result<PathBuf> {
    let entries: Vec<_> = std::fs::read_dir(extract_dir)?
        .filter_map(|e| e.ok())
        .collect();

    if entries.len() == 1 && entries[0].file_type()?.is_dir() {
        Ok(entries[0].path())
    } else {
        Ok(extract_dir.to_path_buf())
    }
}

/// Execute a single install step.
fn execute_install_step(
    step: &InstallStep,
    content_root: &Path,
    keg_path: &Path,
) -> anyhow::Result<()> {
    match step {
        InstallStep::BinInstall { source, target } => {
            let dest_dir = keg_path.join("bin");
            std::fs::create_dir_all(&dest_dir)?;
            let src = content_root.join(source);
            let dest_name = target.as_deref().unwrap_or(source);
            let dest = dest_dir.join(dest_name);
            copy_file_executable(&src, &dest)?;
        }
        InstallStep::ManInstall { source, section } => {
            let dest_dir = keg_path
                .join("share")
                .join("man")
                .join(format!("man{section}"));
            std::fs::create_dir_all(&dest_dir)?;
            let src = content_root.join(source);
            let file_name = Path::new(source)
                .file_name()
                .ok_or_else(|| anyhow::anyhow!("invalid man source: {source}"))?;
            let dest = dest_dir.join(file_name);
            std::fs::copy(&src, &dest)?;
        }
        InstallStep::BashCompletion { source, target } => {
            let dest_dir = keg_path.join("etc").join("bash_completion.d");
            std::fs::create_dir_all(&dest_dir)?;
            let src = content_root.join(source);
            let dest_name = target.as_deref().unwrap_or(
                Path::new(source)
                    .file_name()
                    .and_then(|n| n.to_str())
                    .unwrap_or(source),
            );
            let dest = dest_dir.join(dest_name);
            std::fs::copy(&src, &dest)?;
        }
        InstallStep::ZshCompletion { source, target } => {
            let dest_dir = keg_path.join("share").join("zsh").join("site-functions");
            std::fs::create_dir_all(&dest_dir)?;
            let src = content_root.join(source);
            let dest_name = target.as_deref().unwrap_or(
                Path::new(source)
                    .file_name()
                    .and_then(|n| n.to_str())
                    .unwrap_or(source),
            );
            let dest = dest_dir.join(dest_name);
            std::fs::copy(&src, &dest)?;
        }
    }
    Ok(())
}

/// Copy a file and mark it executable.
fn copy_file_executable(src: &Path, dest: &Path) -> anyhow::Result<()> {
    std::fs::copy(src, dest)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(dest, std::fs::Permissions::from_mode(0o755))?;
    }
    Ok(())
}

/// Copy executable files from content_root into bin_dir.
fn copy_executables(content_root: &Path, bin_dir: &Path) -> anyhow::Result<()> {
    for entry in std::fs::read_dir(content_root)? {
        let entry = entry?;
        let path = entry.path();
        if path.is_file() {
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                let meta = std::fs::metadata(&path)?;
                let mode = meta.permissions().mode();
                if mode & 0o111 != 0 {
                    let dest = bin_dir.join(entry.file_name());
                    copy_file_executable(&path, &dest)?;
                }
            }
            #[cfg(not(unix))]
            {
                let dest = bin_dir.join(entry.file_name());
                std::fs::copy(&path, &dest)?;
            }
        }
    }
    Ok(())
}

/// Count formula files in a tap directory.
fn count_formulae(tap_dir: &Path) -> usize {
    let formula_dir = tap_dir.join("Formula");
    if !formula_dir.is_dir() {
        return 0;
    }
    std::fs::read_dir(&formula_dir)
        .map(|entries| {
            entries
                .filter_map(|e| e.ok())
                .filter(|e| e.path().extension().is_some_and(|ext| ext == "rb"))
                .count()
        })
        .unwrap_or(0)
}

/// Compute the hex-encoded SHA256 digest of data.
fn sha256_hex(data: &[u8]) -> String {
    use sha2::{Digest, Sha256};
    let mut hasher = Sha256::new();
    hasher.update(data);
    format!("{:x}", hasher.finalize())
}

/// Parse a fully-qualified tap formula reference like "user/repo/formula".
/// Returns (tap_name, formula_name) — e.g. ("user/repo", "formula").
pub fn parse_tap_formula_ref(name: &str) -> Option<(String, String)> {
    let parts: Vec<&str> = name.split('/').collect();
    if parts.len() == 3 {
        let tap_name = format!("{}/{}", parts[0], parts[1]);
        let formula_name = parts[2].to_string();
        Some((tap_name, formula_name))
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_tap_name_valid() {
        let (user, repo) = parse_tap_name("marcelocantos/tap").unwrap();
        assert_eq!(user, "marcelocantos");
        assert_eq!(repo, "tap");
    }

    #[test]
    fn test_parse_tap_name_with_homebrew_prefix() {
        let (user, repo) = parse_tap_name("marcelocantos/homebrew-tap").unwrap();
        assert_eq!(user, "marcelocantos");
        assert_eq!(repo, "homebrew-tap");
    }

    #[test]
    fn test_parse_tap_name_invalid_no_slash() {
        assert!(parse_tap_name("justname").is_err());
    }

    #[test]
    fn test_parse_tap_name_invalid_too_many_slashes() {
        assert!(parse_tap_name("a/b/c").is_err());
    }

    #[test]
    fn test_parse_tap_name_invalid_traversal() {
        assert!(parse_tap_name("../evil").is_err());
    }

    #[test]
    fn test_tap_dir_adds_homebrew_prefix() {
        let dir = tap_dir(Path::new("/home/user/.den"), "marcelocantos", "tap");
        assert_eq!(
            dir,
            PathBuf::from("/home/user/.den/taps/marcelocantos/homebrew-tap")
        );
    }

    #[test]
    fn test_tap_dir_preserves_homebrew_prefix() {
        let dir = tap_dir(
            Path::new("/home/user/.den"),
            "marcelocantos",
            "homebrew-tap",
        );
        assert_eq!(
            dir,
            PathBuf::from("/home/user/.den/taps/marcelocantos/homebrew-tap")
        );
    }

    #[test]
    fn test_parse_tap_formula_ref() {
        let (tap, formula) = parse_tap_formula_ref("marcelocantos/tap/mk").unwrap();
        assert_eq!(tap, "marcelocantos/tap");
        assert_eq!(formula, "mk");
    }

    #[test]
    fn test_parse_tap_formula_ref_not_qualified() {
        assert!(parse_tap_formula_ref("tree").is_none());
        assert!(parse_tap_formula_ref("user/repo").is_none());
    }

    #[test]
    fn test_resolve_for_platform_fallback() {
        let formula = TapFormula {
            name: "test".into(),
            desc: None,
            homepage: None,
            version: "1.0.0".into(),
            license: None,
            url: "https://example.com/default.tar.gz".into(),
            sha256: "abc123".into(),
            platform_overrides: vec![],
            dependencies: vec![],
            build_dependencies: vec![],
            install_steps: vec![],
        };
        let (url, sha) = resolve_for_platform(&formula, Arch::Arm64).unwrap();
        assert_eq!(url, "https://example.com/default.tar.gz");
        assert_eq!(sha, "abc123");
    }

    #[test]
    fn test_resolve_for_platform_with_override() {
        let formula = TapFormula {
            name: "test".into(),
            desc: None,
            homepage: None,
            version: "1.0.0".into(),
            license: None,
            url: "https://example.com/default.tar.gz".into(),
            sha256: "abc123".into(),
            platform_overrides: vec![
                PlatformOverride {
                    os: Some("macos".into()),
                    arch: Some("arm".into()),
                    url: "https://example.com/darwin-arm64.tar.gz".into(),
                    sha256: "specific123".into(),
                },
                PlatformOverride {
                    os: Some("linux".into()),
                    arch: Some("intel".into()),
                    url: "https://example.com/linux-amd64.tar.gz".into(),
                    sha256: "linux123".into(),
                },
            ],
            dependencies: vec![],
            build_dependencies: vec![],
            install_steps: vec![],
        };
        // On macOS arm64, we should get the darwin-arm64 override.
        #[cfg(target_os = "macos")]
        {
            let (url, sha) = resolve_for_platform(&formula, Arch::Arm64).unwrap();
            assert_eq!(url, "https://example.com/darwin-arm64.tar.gz");
            assert_eq!(sha, "specific123");
        }
    }
}

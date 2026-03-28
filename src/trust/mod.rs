// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::collections::BTreeMap;
use std::path::Path;

/// Read the known-good hash database.
pub fn read_known_hashes(den_home: &Path) -> BTreeMap<String, String> {
    let path = den_home.join("trust").join("known_hashes.json");
    std::fs::read_to_string(&path)
        .ok()
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or_default()
}

/// Record a verified bottle hash. Writes atomically.
pub fn record_hash(den_home: &Path, name: &str, version: &str, sha256: &str) -> anyhow::Result<()> {
    let dir = den_home.join("trust");
    std::fs::create_dir_all(&dir)?;
    let mut hashes = read_known_hashes(den_home);
    let key = format!("{name}/{version}");
    hashes.insert(key, sha256.to_string());
    let json = serde_json::to_string_pretty(&hashes)?;
    let mut tmp = tempfile::NamedTempFile::new_in(&dir)?;
    std::io::Write::write_all(&mut tmp, json.as_bytes())?;
    let persist_path = dir.join("known_hashes.json");
    tmp.persist(&persist_path)?;
    set_file_permissions_0600(&persist_path);
    Ok(())
}

/// Check if a bottle hash matches what we've seen before.
/// Returns Ok(()) if the hash is new or matches. Returns Err if it differs.
pub fn verify_hash(den_home: &Path, name: &str, version: &str, sha256: &str) -> anyhow::Result<()> {
    let hashes = read_known_hashes(den_home);
    let key = format!("{name}/{version}");
    if let Some(known) = hashes.get(&key) {
        if known != sha256 {
            anyhow::bail!(
                "SECURITY: bottle hash for {name} {version} changed!\n  \
                 Previously seen: {known}\n  \
                 Now in index:    {sha256}\n  \
                 This could indicate a compromised formula index.\n  \
                 If this is expected (e.g., a bottle rebuild), run:\n  \
                 den set trust.allow_hash_change true"
            );
        }
    }
    Ok(())
}

#[cfg(unix)]
pub fn set_file_permissions_0600(path: &Path) {
    use std::os::unix::fs::PermissionsExt;
    let _ = std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600));
}

#[cfg(not(unix))]
pub fn set_file_permissions_0600(_path: &Path) {}

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::path::{Path, PathBuf};

/// Directories within a keg that get linked into the environment.
const LINK_DIRS: &[&str] = &["bin", "sbin", "lib", "include", "share"];

/// Link a keg's contents into an environment directory.
/// Creates absolute symlinks from env_path/{bin,lib,...}/file -> keg_path/{bin,lib,...}/file.
/// Also creates an opt/ symlink: env_path/opt/{formula_name} -> keg_path.
/// Returns the list of symlinks created.
pub fn link_keg(
    keg_path: &Path,
    env_path: &Path,
    formula_name: &str,
) -> anyhow::Result<Vec<PathBuf>> {
    let mut created = Vec::new();

    for dir in LINK_DIRS {
        let keg_dir = keg_path.join(dir);
        if !keg_dir.is_dir() {
            continue;
        }

        link_tree(&keg_dir, &env_path.join(dir), &mut created)?;
    }

    // Create opt/ symlink: opt/{formula_name} -> keg_path
    let opt_dir = env_path.join("opt");
    std::fs::create_dir_all(&opt_dir)?;
    let opt_link = opt_dir.join(formula_name);
    if opt_link.symlink_metadata().is_ok() {
        std::fs::remove_file(&opt_link)?;
    }
    std::os::unix::fs::symlink(keg_path, &opt_link)?;
    created.push(opt_link);

    Ok(created)
}

/// Recursively link files from src_dir into dst_dir.
/// Directories are created; files are symlinked.
fn link_tree(src_dir: &Path, dst_dir: &Path, created: &mut Vec<PathBuf>) -> anyhow::Result<()> {
    std::fs::create_dir_all(dst_dir)?;

    for entry in std::fs::read_dir(src_dir)? {
        let entry = entry?;
        let src_path = entry.path();
        let file_name = entry.file_name();
        let dst_path = dst_dir.join(&file_name);

        if src_path.is_dir() {
            link_tree(&src_path, &dst_path, created)?;
        } else {
            // Remove existing symlink/file if present.
            if dst_path.symlink_metadata().is_ok() {
                std::fs::remove_file(&dst_path)?;
            }
            std::os::unix::fs::symlink(&src_path, &dst_path)?;
            created.push(dst_path);
        }
    }

    Ok(())
}

/// Unlink a keg's contents from an environment directory.
/// Only removes symlinks that point to the specified keg.
/// Also removes the opt/ symlink. Cleans up empty directories afterward.
#[allow(dead_code)]
pub fn unlink_keg(keg_path: &Path, env_path: &Path, formula_name: &str) -> anyhow::Result<u32> {
    let mut removed = 0u32;

    for dir in LINK_DIRS {
        let keg_dir = keg_path.join(dir);
        if !keg_dir.is_dir() {
            continue;
        }

        removed += unlink_tree(&keg_dir, &env_path.join(dir))?;
    }

    // Remove opt/ symlink if it points to this keg.
    let opt_link = env_path.join("opt").join(formula_name);
    if let Ok(target) = opt_link.read_link()
        && target == keg_path {
            std::fs::remove_file(&opt_link)?;
            removed += 1;
        }

    Ok(removed)
}

/// Recursively unlink files that point to src_dir.
#[allow(dead_code)]
fn unlink_tree(src_dir: &Path, dst_dir: &Path) -> anyhow::Result<u32> {
    let mut removed = 0u32;

    if !dst_dir.is_dir() {
        return Ok(0);
    }

    for entry in std::fs::read_dir(src_dir)? {
        let entry = entry?;
        let src_path = entry.path();
        let dst_path = dst_dir.join(entry.file_name());

        if src_path.is_dir() {
            removed += unlink_tree(&src_path, &dst_path)?;
            // Remove empty directory.
            let _ = std::fs::remove_dir(&dst_path);
        } else if let Ok(target) = dst_path.read_link()
            && target == src_path {
                std::fs::remove_file(&dst_path)?;
                removed += 1;
            }
    }

    Ok(removed)
}

/// Track which keg is linked for a formula in an environment.
/// Writes a small file: env_path/.den/linked/{formula_name} -> version
pub fn record_linked_version(
    env_path: &Path,
    formula_name: &str,
    version: &str,
) -> anyhow::Result<()> {
    let linked_dir = env_path.join(".den").join("linked");
    std::fs::create_dir_all(&linked_dir)?;
    std::fs::write(linked_dir.join(formula_name), version)?;
    Ok(())
}

/// Read which version of a formula is linked in an environment.
pub fn linked_version(env_path: &Path, formula_name: &str) -> Option<String> {
    let path = env_path.join(".den").join("linked").join(formula_name);
    std::fs::read_to_string(path).ok().map(|s| s.trim().to_string())
}

/// Remove the linked version record for a formula.
#[allow(dead_code)]
pub fn remove_linked_record(env_path: &Path, formula_name: &str) -> anyhow::Result<()> {
    let path = env_path.join(".den").join("linked").join(formula_name);
    if path.exists() {
        std::fs::remove_file(path)?;
    }
    Ok(())
}

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::path::Path;

use crate::config::Config;
use crate::{env, keg, manifest};

/// Severity of a doctor finding.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Severity {
    Warning,
    Error,
}

impl std::fmt::Display for Severity {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Severity::Warning => write!(f, "Warning"),
            Severity::Error => write!(f, "Error"),
        }
    }
}

struct Finding {
    severity: Severity,
    message: String,
}

impl Finding {
    fn warning(msg: impl Into<String>) -> Self {
        Self {
            severity: Severity::Warning,
            message: msg.into(),
        }
    }

    fn error(msg: impl Into<String>) -> Self {
        Self {
            severity: Severity::Error,
            message: msg.into(),
        }
    }
}

pub(super) fn doctor(config: &Config) -> anyhow::Result<()> {
    println!("==> Checking den system health...\n");

    let mut findings = Vec::new();

    check_den_home(config, &mut findings);
    check_cellar(config, &mut findings);
    check_manifests(config, &mut findings)?;
    check_environments(config, &mut findings)?;
    check_broken_symlinks(config, &mut findings)?;
    check_missing_kegs(config, &mut findings)?;
    check_permissions(config, &mut findings);

    if findings.is_empty() {
        println!("Your system is ready to brew.");
    } else {
        let errors = findings
            .iter()
            .filter(|f| f.severity == Severity::Error)
            .count();
        let warnings = findings
            .iter()
            .filter(|f| f.severity == Severity::Warning)
            .count();

        for finding in &findings {
            let prefix = match finding.severity {
                Severity::Error => "Error",
                Severity::Warning => "Warning",
            };
            println!("{prefix}: {}", finding.message);
        }

        println!();
        if errors > 0 {
            println!(
                "Found {} error(s) and {} warning(s).",
                errors, warnings
            );
        } else {
            println!("Found {} warning(s).", warnings);
        }
    }

    Ok(())
}

/// Check that ~/.den exists and has expected structure.
fn check_den_home(config: &Config, findings: &mut Vec<Finding>) {
    if !config.den_home.is_dir() {
        findings.push(Finding::error(format!(
            "DEN_HOME ({}) does not exist. Run `den install <formula>` or `den migrate` to initialise.",
            config.den_home.display()
        )));
        return;
    }

    let expected_dirs = ["manifests", "envs", "cache"];
    for dir in &expected_dirs {
        let path = config.den_home.join(dir);
        if !path.is_dir() {
            findings.push(Finding::warning(format!(
                "Missing directory: {}. It will be created on first use.",
                path.display()
            )));
        }
    }
}

/// Check that the Cellar exists and is accessible.
fn check_cellar(config: &Config, findings: &mut Vec<Finding>) {
    if !config.cellar.is_dir() {
        findings.push(Finding::warning(format!(
            "Cellar ({}) does not exist. No formulae are installed.",
            config.cellar.display()
        )));
    }
}

/// Check manifest integrity: valid JSON, no empty packages.
fn check_manifests(config: &Config, findings: &mut Vec<Finding>) -> anyhow::Result<()> {
    let envs = manifest::list_all(&config.den_home)?;
    if envs.is_empty() {
        findings.push(Finding::warning(
            "No environments found. Run `den install <formula>` or `den migrate` to get started.",
        ));
        return Ok(());
    }

    for env_path in &envs {
        match manifest::read_manifest(&config.den_home, env_path) {
            Ok(m) => {
                // Check for empty version strings.
                for (name, version) in &m.packages {
                    if version.is_empty() {
                        findings.push(Finding::error(format!(
                            "Manifest for '{env_path}' has empty version for package '{name}'."
                        )));
                    }
                }
                for (name, version) in &m.casks {
                    if version.is_empty() {
                        findings.push(Finding::error(format!(
                            "Manifest for '{env_path}' has empty version for cask '{name}'."
                        )));
                    }
                }
            }
            Err(e) => {
                findings.push(Finding::error(format!(
                    "Failed to read manifest for '{env_path}': {e}"
                )));
            }
        }
    }

    Ok(())
}

/// Check that materialised environment directories exist for all manifests.
fn check_environments(config: &Config, findings: &mut Vec<Finding>) -> anyhow::Result<()> {
    let envs = manifest::list_all(&config.den_home)?;
    for env_path in &envs {
        let env_dir = env::env_dir(&config.den_home, env_path);
        if !env_dir.is_dir() {
            findings.push(Finding::warning(format!(
                "Environment '{env_path}' has a manifest but no materialised directory at {}. \
                 Run `den env materialise` or install a package to fix.",
                env_dir.display()
            )));
        }
    }

    // Check the active environment is valid.
    let active = env::active_env_path(&config.den_home);
    if !manifest::manifest_exists(&config.den_home, &active) {
        // Only warn if den_home exists (i.e., den has been initialised).
        if config.den_home.is_dir() {
            findings.push(Finding::warning(format!(
                "Active environment '{active}' has no manifest. \
                 Run `den env create {active}` or `den env use /`.",
            )));
        }
    }

    Ok(())
}

/// Check for broken symlinks in materialised environments.
fn check_broken_symlinks(config: &Config, findings: &mut Vec<Finding>) -> anyhow::Result<()> {
    let envs_dir = config.den_home.join("envs");
    if !envs_dir.is_dir() {
        return Ok(());
    }

    for entry in std::fs::read_dir(&envs_dir)? {
        let entry = entry?;
        if !entry.path().is_dir() {
            continue;
        }
        let env_name = entry.file_name().to_string_lossy().into_owned();
        let broken_count = count_broken_symlinks(&entry.path())?;
        if broken_count > 0 {
            let env_path = manifest::slug_to_path(&env_name);
            findings.push(Finding::warning(format!(
                "Environment '{env_path}' has {broken_count} broken symlink(s). \
                 Run `den env materialise` to repair, or `den cleanup` to remove stale kegs."
            )));
        }
    }

    Ok(())
}

/// Count broken symlinks recursively under a directory.
fn count_broken_symlinks(dir: &Path) -> anyhow::Result<u32> {
    let mut count = 0u32;

    for entry in std::fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        let meta = std::fs::symlink_metadata(&path)?;

        if meta.file_type().is_symlink() {
            // A symlink is broken if its target doesn't exist.
            if !path.exists() {
                count += 1;
            }
        } else if meta.file_type().is_dir() {
            // Skip .den metadata directory.
            if entry.file_name().to_string_lossy() == ".den" {
                continue;
            }
            count += count_broken_symlinks(&path)?;
        }
    }

    Ok(count)
}

/// Check that every package in every manifest has a corresponding keg.
fn check_missing_kegs(config: &Config, findings: &mut Vec<Finding>) -> anyhow::Result<()> {
    let envs = manifest::list_all(&config.den_home)?;
    let installed_kegs = keg::list_installed(&config.cellar)?;
    let installed_set: std::collections::HashSet<(String, String)> = installed_kegs
        .iter()
        .map(|k| (k.name.clone(), k.version.clone()))
        .collect();

    for env_path in &envs {
        let resolved = manifest::resolve(&config.den_home, env_path)?;
        for (name, version) in &resolved {
            if !installed_set.contains(&(name.clone(), version.clone())) {
                findings.push(Finding::error(format!(
                    "Package '{name}' version '{version}' is in manifest for '{env_path}' \
                     but not found in the Cellar. Run `den install {name}` to restore."
                )));
            }
        }
    }

    Ok(())
}

/// Check permissions on sensitive files.
fn check_permissions(config: &Config, findings: &mut Vec<Finding>) {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;

        let sensitive_files = [
            config.den_home.join("trust").join("known_hashes.json"),
            config.den_home.join("active_env"),
        ];

        for path in &sensitive_files {
            if !path.exists() {
                continue;
            }
            if let Ok(meta) = std::fs::metadata(path) {
                let mode = meta.permissions().mode() & 0o777;
                if mode & 0o077 != 0 {
                    findings.push(Finding::warning(format!(
                        "{} has permissions {:04o} (should be 0600). \
                         Run: chmod 600 {}",
                        path.display(),
                        mode,
                        path.display()
                    )));
                }
            }
        }
    }

    // Suppress unused variable warning on non-Unix.
    let _ = config;
    let _ = findings;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_doctor_empty_home() {
        let tmp = tempfile::tempdir().unwrap();
        let config = Config {
            prefix: tmp.path().join("prefix"),
            cellar: tmp.path().join("Cellar"),
            caskroom: tmp.path().join("Caskroom"),
            cache: tmp.path().join("Cache"),
            taps: tmp.path().join("Taps"),
            den_home: tmp.path().join(".den"),
            arch: crate::platform::Arch::Arm64,
            macos_version: None,
        };
        // Should not panic — just finds issues.
        assert!(doctor(&config).is_ok());
    }

    #[test]
    fn test_doctor_healthy_system() {
        let tmp = tempfile::tempdir().unwrap();
        let den_home = tmp.path().join(".den");
        let cellar = tmp.path().join("Cellar");

        // Create expected directories.
        std::fs::create_dir_all(den_home.join("manifests")).unwrap();
        std::fs::create_dir_all(den_home.join("envs")).unwrap();
        std::fs::create_dir_all(den_home.join("cache")).unwrap();
        std::fs::create_dir_all(&cellar).unwrap();

        let config = Config {
            prefix: tmp.path().join("prefix"),
            cellar,
            caskroom: tmp.path().join("Caskroom"),
            cache: tmp.path().join("Cache"),
            taps: tmp.path().join("Taps"),
            den_home,
            arch: crate::platform::Arch::Arm64,
            macos_version: None,
        };

        assert!(doctor(&config).is_ok());
    }
}

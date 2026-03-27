// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use clap::Subcommand;

use crate::config::Config;
use crate::{env, manifest};

#[derive(Subcommand)]
pub(super) enum EnvCommand {
    /// Create a new environment (path-based: / is root, /ml, /work/legacy)
    Create {
        /// Environment path (e.g. /ml, /work/legacy)
        path: String,
    },
    /// List all environments
    List,
    /// Remove an environment
    Remove {
        /// Environment path
        path: String,
    },
    /// Activate an environment in the current shell
    Use {
        /// Environment path (e.g. /, /ml)
        path: String,
    },
    /// Show resolved packages for an environment
    Show {
        /// Environment path (default: active)
        path: Option<String>,
    },
    /// Export resolved environment as a lockfile
    Freeze {
        /// Environment path (default: active)
        path: Option<String>,
    },
}

pub(super) fn run_env_command(config: &Config, command: Option<EnvCommand>) -> anyhow::Result<()> {
    match command {
        Some(EnvCommand::List) | None => {
            let envs = env::list_envs(&config.den_home)?;
            if envs.is_empty() {
                // Ensure root exists.
                manifest::write_manifest(&config.den_home, "/", &manifest::Manifest::default())?;
                println!("/ (active)");
            } else {
                let active = env::active_env_path(&config.den_home);
                for env_path in &envs {
                    let marker = if *env_path == active { " (active)" } else { "" };
                    // Show inheritance.
                    let parent_info = manifest::parent_path(env_path)
                        .map(|p| format!(" (from {p})"))
                        .unwrap_or_default();
                    println!("{env_path}{parent_info}{marker}");
                }
            }
            Ok(())
        }
        Some(EnvCommand::Create { path }) => {
            let env_path = normalise_env_path(&path)?;

            // Ensure parent exists (except for root).
            if let Some(parent) = manifest::parent_path(&env_path)
                && !manifest::manifest_exists(&config.den_home, &parent)
            {
                anyhow::bail!(
                    "parent environment '{}' does not exist. Create it first.",
                    parent
                );
            }

            if manifest::manifest_exists(&config.den_home, &env_path) {
                anyhow::bail!("environment '{}' already exists", env_path);
            }

            manifest::write_manifest(&config.den_home, &env_path, &manifest::Manifest::default())?;

            // Materialise (inherits from parent).
            let links = env::materialise(&config.den_home, &config.cellar, &env_path)?;
            let slug = manifest::env_slug(&env_path);
            println!("Created environment '{env_path}' ({links} symlinks, dir: {slug})");
            Ok(())
        }
        Some(EnvCommand::Use { path }) => {
            let env_path = normalise_env_path(&path)?;

            if !manifest::manifest_exists(&config.den_home, &env_path) {
                anyhow::bail!(
                    "environment '{}' does not exist. Create it with `den env create {}`.",
                    env_path,
                    path
                );
            }

            env::set_active_env(&config.den_home, &env_path)?;
            let slug = manifest::env_slug(&env_path);
            super::shell::print_env_switch_commands(config, &slug);
            Ok(())
        }
        Some(EnvCommand::Show { path }) => {
            let env_path = path
                .map(|p| normalise_env_path(&p))
                .transpose()?
                .unwrap_or_else(|| env::active_env_path(&config.den_home));

            let resolved = manifest::resolve(&config.den_home, &env_path)?;
            if resolved.is_empty() {
                println!("Environment '{env_path}' has no packages.");
            } else {
                println!("Resolved packages for '{env_path}':");
                for (name, version) in &resolved {
                    // Show which level contributed this package.
                    let source = find_source_env(config, &env_path, name);
                    let source_info = if source != env_path {
                        format!(" (from {source})")
                    } else {
                        String::new()
                    };
                    println!("  {name} {version}{source_info}");
                }
            }
            Ok(())
        }
        Some(EnvCommand::Remove { path }) => {
            let env_path = normalise_env_path(&path)?;
            if env_path == "/" {
                anyhow::bail!("cannot remove the root environment");
            }
            if !manifest::manifest_exists(&config.den_home, &env_path) {
                anyhow::bail!("environment '{}' does not exist", env_path);
            }

            // Remove materialised env.
            let env_dir = env::env_dir(&config.den_home, &env_path);
            if env_dir.is_dir() {
                std::fs::remove_dir_all(&env_dir)?;
            }

            // Remove manifest.
            let manifest_dir = config
                .den_home
                .join("manifests")
                .join(env_path.trim_matches('/'));
            if manifest_dir.is_dir() {
                std::fs::remove_dir_all(&manifest_dir)?;
            } else {
                // Single file case.
                let _ = std::fs::remove_file(&manifest_dir);
            }

            println!("Removed environment '{env_path}'.");
            Ok(())
        }
        Some(EnvCommand::Freeze { path }) => {
            let env_path = path
                .map(|p| normalise_env_path(&p))
                .transpose()?
                .unwrap_or_else(|| env::active_env_path(&config.den_home));

            let resolved = manifest::resolve(&config.den_home, &env_path)?;
            let json = serde_json::to_string_pretty(&resolved)?;
            println!("{json}");
            Ok(())
        }
    }
}

/// Find which environment in the ancestor chain provides a given package.
fn find_source_env(config: &Config, env_path: &str, package: &str) -> String {
    let chain = manifest::ancestor_chain(env_path);
    // Walk from leaf to root to find the most specific provider.
    for ancestor in chain.iter().rev() {
        if let Ok(m) = manifest::read_manifest(&config.den_home, ancestor)
            && m.packages.contains_key(package)
        {
            return ancestor.clone();
        }
    }
    env_path.to_string()
}

pub(super) fn normalise_env_path(path: &str) -> anyhow::Result<String> {
    if path.is_empty() || path == "/" {
        return Ok("/".to_string());
    }
    // Reject path traversal.
    if path.contains("..") {
        anyhow::bail!("environment path must not contain '..'");
    }
    let p = if path.starts_with('/') {
        path.to_string()
    } else {
        format!("/{path}")
    };
    let trimmed = p.trim_end_matches('/');
    // Validate each component contains only safe characters.
    for component in trimmed.split('/').filter(|c| !c.is_empty()) {
        if !component
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '.' || c == '_' || c == '-')
        {
            anyhow::bail!(
                "environment path component '{}' contains invalid characters (allowed: a-z, A-Z, 0-9, '.', '_', '-')",
                component
            );
        }
    }
    Ok(trimmed.to_string())
}

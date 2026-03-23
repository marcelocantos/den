// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::path::{Path, PathBuf};

/// A managed service with its plist path.
#[derive(Debug)]
#[allow(dead_code)]
pub struct Service {
    pub name: String,
    pub formula: String,
    pub plist_path: PathBuf,
    pub running: bool,
}

/// Find all services defined by installed formulae.
pub fn list_services(_cellar: &Path, opt_dir: &Path) -> anyhow::Result<Vec<Service>> {
    let mut services = Vec::new();

    if !opt_dir.is_dir() {
        return Ok(services);
    }

    for entry in std::fs::read_dir(opt_dir)? {
        let entry = entry?;
        let formula_name = entry.file_name().to_string_lossy().into_owned();

        // Resolve the opt symlink to the actual keg.
        let keg_path = match std::fs::read_link(entry.path()) {
            Ok(target) => {
                if target.is_absolute() {
                    target
                } else {
                    entry.path().parent().unwrap().join(target)
                }
            }
            Err(_) => continue,
        };

        // Look for plist files.
        let plist_name = format!("homebrew.mxcl.{formula_name}.plist");
        let plist_path = keg_path.join(&plist_name);

        if plist_path.exists() {
            let running = is_service_running(&formula_name);
            services.push(Service {
                name: formula_name.clone(),
                formula: formula_name,
                plist_path,
                running,
            });
        }
    }

    services.sort_by(|a, b| a.name.cmp(&b.name));
    Ok(services)
}

/// Check if a service is currently loaded in launchctl.
fn is_service_running(name: &str) -> bool {
    let label = format!("homebrew.mxcl.{name}");
    let output = std::process::Command::new("launchctl")
        .args(["list"])
        .output();

    match output {
        Ok(out) => {
            let stdout = String::from_utf8_lossy(&out.stdout);
            stdout.lines().any(|line| line.contains(&label))
        }
        Err(_) => false,
    }
}

/// Start a service by loading its plist with launchctl.
pub fn start_service(service: &Service) -> anyhow::Result<()> {
    // Copy plist to ~/Library/LaunchAgents/ for persistence.
    let launch_agents = dirs::home_dir()
        .ok_or_else(|| anyhow::anyhow!("cannot determine home directory"))?
        .join("Library/LaunchAgents");
    std::fs::create_dir_all(&launch_agents)?;

    let dest = launch_agents.join(service.plist_path.file_name().unwrap());
    std::fs::copy(&service.plist_path, &dest)?;

    let status = std::process::Command::new("launchctl")
        .args(["load", "-w"])
        .arg(&dest)
        .status()?;

    if !status.success() {
        anyhow::bail!("failed to start service '{}'", service.name);
    }

    Ok(())
}

/// Stop a service by unloading its plist.
pub fn stop_service(service: &Service) -> anyhow::Result<()> {
    let launch_agents = dirs::home_dir()
        .ok_or_else(|| anyhow::anyhow!("cannot determine home directory"))?
        .join("Library/LaunchAgents");

    let dest = launch_agents.join(service.plist_path.file_name().unwrap());

    if dest.exists() {
        let status = std::process::Command::new("launchctl")
            .args(["unload", "-w"])
            .arg(&dest)
            .status()?;

        if !status.success() {
            anyhow::bail!("failed to stop service '{}'", service.name);
        }

        std::fs::remove_file(&dest)?;
    }

    Ok(())
}

/// Restart a service.
pub fn restart_service(service: &Service) -> anyhow::Result<()> {
    if service.running {
        stop_service(service)?;
    }
    start_service(service)
}

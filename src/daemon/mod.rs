// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::path::{Path, PathBuf};
use std::time::Duration;

use reqwest::Client;
use serde::{Deserialize, Serialize};

use crate::api::FormulaIndex;
use crate::config::Config;
use crate::{bottle, deps, env, formula, manifest, platform, tab};

/// Default interval between index refresh checks.
const DEFAULT_INTERVAL: Duration = Duration::from_secs(6 * 60 * 60); // 6 hours

/// Pending upgrade entry.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PendingUpgrade {
    pub name: String,
    pub installed: String,
    pub available: String,
}

/// Daemon state written to the filesystem for the CLI to read.
#[derive(Debug, Default, Serialize, Deserialize)]
pub struct DaemonState {
    pub last_check: Option<u64>,
    pub pending: Vec<PendingUpgrade>,
    pub last_upgrade: Option<u64>,
    #[serde(default)]
    pub errors: Vec<String>,
}

/// Read the daemon state file.
pub fn read_state(den_home: &Path) -> DaemonState {
    let path = den_home.join("daemon_state.json");
    std::fs::read_to_string(&path)
        .ok()
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or_default()
}

/// Write the daemon state file.
fn write_state(den_home: &Path, state: &DaemonState) -> anyhow::Result<()> {
    let path = den_home.join("daemon_state.json");
    let json = serde_json::to_string_pretty(state)?;
    std::fs::write(path, json)?;
    Ok(())
}

/// Read daemon settings.
#[derive(Debug, Default, Serialize, Deserialize)]
pub struct DaemonSettings {
    #[serde(default)]
    pub auto_upgrade: bool,
    /// Maintenance window, e.g. "3:00-5:00". Empty = upgrade when idle.
    #[serde(default)]
    pub upgrade_window: Option<String>,
    /// Check interval in seconds (default: 6 hours).
    #[serde(default)]
    pub interval_secs: Option<u64>,
}

#[allow(dead_code)]
pub fn read_settings(den_home: &Path) -> DaemonSettings {
    let path = den_home.join("daemon_settings.json");
    std::fs::read_to_string(&path)
        .ok()
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or_default()
}

#[allow(dead_code)]
pub fn write_settings(den_home: &Path, settings: &DaemonSettings) -> anyhow::Result<()> {
    let path = den_home.join("daemon_settings.json");
    let json = serde_json::to_string_pretty(settings)?;
    std::fs::write(path, json)?;
    Ok(())
}

/// PID file management.
fn pid_path(den_home: &Path) -> PathBuf {
    den_home.join("daemon.pid")
}

pub fn is_running(den_home: &Path) -> bool {
    let path = pid_path(den_home);
    if let Ok(pid_str) = std::fs::read_to_string(&path) {
        if let Ok(pid) = pid_str.trim().parse::<i32>() {
            // Check if process is alive.
            unsafe { libc::kill(pid, 0) == 0 }
        } else {
            false
        }
    } else {
        false
    }
}

fn write_pid(den_home: &Path) -> anyhow::Result<()> {
    let pid = std::process::id();
    std::fs::write(pid_path(den_home), pid.to_string())?;
    Ok(())
}

fn remove_pid(den_home: &Path) {
    let _ = std::fs::remove_file(pid_path(den_home));
}

/// Log a message to the daemon log.
fn log(den_home: &Path, msg: &str) {
    let log_path = den_home.join("daemon.log");
    let timestamp = chrono_timestamp();
    let line = format!("[{timestamp}] {msg}\n");
    let _ = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(log_path)
        .and_then(|mut f| std::io::Write::write_all(&mut f, line.as_bytes()));
}

fn chrono_timestamp() -> String {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    // Simple ISO-ish timestamp without chrono dependency.
    format!("{now}")
}

pub fn now_secs() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

/// Check if we're inside the configured maintenance window.
fn in_maintenance_window(window: &str) -> bool {
    // Parse "HH:MM-HH:MM".
    let parts: Vec<&str> = window.split('-').collect();
    if parts.len() != 2 {
        return false;
    }

    let parse_hm = |s: &str| -> Option<(u32, u32)> {
        let hm: Vec<&str> = s.trim().split(':').collect();
        if hm.len() != 2 {
            return None;
        }
        Some((hm[0].parse().ok()?, hm[1].parse().ok()?))
    };

    let (start_h, start_m) = match parse_hm(parts[0]) {
        Some(v) => v,
        None => return false,
    };
    let (end_h, end_m) = match parse_hm(parts[1]) {
        Some(v) => v,
        None => return false,
    };

    // Get current local time from the system.
    let output = std::process::Command::new("date")
        .args(["+%H:%M"])
        .output();

    let (cur_h, cur_m) = match output {
        Ok(out) => {
            let s = String::from_utf8_lossy(&out.stdout);
            match parse_hm(s.trim()) {
                Some(v) => v,
                None => return false,
            }
        }
        Err(_) => return false,
    };

    let start = start_h * 60 + start_m;
    let end = end_h * 60 + end_m;
    let now = cur_h * 60 + cur_m;

    if start <= end {
        now >= start && now < end
    } else {
        // Wraps midnight: e.g. 23:00-5:00
        now >= start || now < end
    }
}

/// Run the daemon main loop.
pub async fn run(config: &Config) -> anyhow::Result<()> {
    if is_running(&config.den_home) {
        anyhow::bail!("daemon is already running");
    }

    write_pid(&config.den_home)?;
    log(&config.den_home, "daemon started");

    let settings = crate::settings::read(&config.den_home);
    let interval = Duration::from_secs(
        settings.daemon.interval_secs.unwrap_or(DEFAULT_INTERVAL.as_secs()),
    );

    let client = Client::new();

    // Install signal handler for graceful shutdown.
    let running = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(true));
    let r = running.clone();
    ctrlc::set_handler(move || {
        r.store(false, std::sync::atomic::Ordering::SeqCst);
    })?;

    while running.load(std::sync::atomic::Ordering::SeqCst) {
        if let Err(e) = tick(&client, config).await {
            log(&config.den_home, &format!("error: {e}"));
        }

        // Sleep in small increments so we can respond to signals.
        let mut elapsed = Duration::ZERO;
        let sleep_step = Duration::from_secs(5);
        while elapsed < interval && running.load(std::sync::atomic::Ordering::SeqCst) {
            tokio::time::sleep(sleep_step).await;
            elapsed += sleep_step;
        }
    }

    log(&config.den_home, "daemon stopped");
    remove_pid(&config.den_home);
    Ok(())
}

/// One tick of the daemon loop: refresh index, check for upgrades,
/// optionally apply them.
async fn tick(client: &Client, config: &Config) -> anyhow::Result<()> {
    let cache_dir = config.den_home.join("cache");

    log(&config.den_home, "refreshing formula index");
    let index = FormulaIndex::refresh(client, &cache_dir).await?;
    log(
        &config.den_home,
        &format!("{} formulae indexed", index.len()),
    );

    // Check what's outdated in the root environment.
    let resolved = manifest::resolve(&config.den_home, "/")?;
    let mut pending = Vec::new();

    for (name, installed_version) in &resolved {
        if let Some(info) = index.get(name) {
            let latest = info.pkg_version();
            if latest != *installed_version {
                pending.push(PendingUpgrade {
                    name: name.clone(),
                    installed: installed_version.clone(),
                    available: latest,
                });
            }
        }
    }

    let mut state = read_state(&config.den_home);
    state.last_check = Some(now_secs());
    state.pending = pending.clone();
    state.errors.clear();

    // Write state immediately so `den daemon status` shows progress
    // even if we get killed during bottle downloads.
    write_state(&config.den_home, &state)?;

    if pending.is_empty() {
        log(&config.den_home, "all packages up to date");
        return Ok(());
    }

    log(
        &config.den_home,
        &format!("{} packages outdated", pending.len()),
    );

    // Pre-download bottles for pending upgrades.
    for upgrade in &pending {
        if let Some(info) = index.get(&upgrade.name) {
            let keg_path = config.cellar.join(&info.name).join(&upgrade.available);
            if keg_path.is_dir() {
                continue; // Already poured.
            }

            // Download bottle into cache (but don't pour yet).
            if let Some(ref bottle_spec) = info.bottle
                && let Some(ref stable) = bottle_spec.stable {
                    let macos = match config.macos_version.as_ref() {
                        Some(v) => v,
                        None => continue,
                    };
                    let tags: Vec<String> = stable.files.keys().cloned().collect();
                    if let Some(tag) = platform::best_bottle_tag(config.arch, macos, &tags) {
                        let bottle_file = &stable.files[&tag];
                        let ghcr_path = formula::ghcr_path(&info.name);
                        let bottle_cache = config.den_home.join("cache").join("bottles");

                        match bottle::fetch_bottle(client, bottle_file, &ghcr_path, &bottle_cache)
                            .await
                        {
                            Ok(data) => {
                                log(
                                    &config.den_home,
                                    &format!(
                                        "cached {} {} ({} bytes)",
                                        info.name,
                                        upgrade.available,
                                        data.len()
                                    ),
                                );
                            }
                            Err(e) => {
                                log(
                                    &config.den_home,
                                    &format!("failed to cache {}: {e}", info.name),
                                );
                            }
                        }
                    }
                }
        }
    }

    // Apply upgrades if auto-upgrade is enabled and we're in the window.
    let settings = crate::settings::read(&config.den_home);
    if settings.daemon.auto_upgrade {
        let can_upgrade = match &settings.daemon.upgrade_window {
            Some(window) => in_maintenance_window(window),
            None => true, // No window = always OK (TODO: idle detection)
        };

        if can_upgrade {
            log(&config.den_home, "applying auto-upgrades");
            if let Err(e) = apply_upgrades(client, config, &index, &pending).await {
                log(&config.den_home, &format!("upgrade error: {e}"));
                state.errors.push(format!("{e}"));
            } else {
                state.pending.clear();
                state.last_upgrade = Some(now_secs());
                log(&config.den_home, "upgrades applied");
            }
        } else {
            log(&config.den_home, "outside maintenance window, skipping upgrades");
        }
    }

    write_state(&config.den_home, &state)?;
    Ok(())
}

/// Pour pending upgrades and update the manifest.
async fn apply_upgrades(
    client: &Client,
    config: &Config,
    index: &FormulaIndex,
    pending: &[PendingUpgrade],
) -> anyhow::Result<()> {
    let active_env = "/";

    for upgrade in pending {
        let info = match index.get(&upgrade.name) {
            Some(info) => info,
            None => continue,
        };

        let pkg_version = info.pkg_version();
        let keg_path = config.cellar.join(&info.name).join(&pkg_version);

        if !keg_path.is_dir() {
            // Resolve deps and pour.
            let all = deps::resolve_install_order(index, &info.name)?;
            for dep_info in &all {
                let dep_keg = config
                    .cellar
                    .join(&dep_info.name)
                    .join(dep_info.pkg_version());
                if !dep_keg.is_dir() {
                    pour_bottle_quiet(client, config, dep_info).await?;
                }
            }
        }

        // Update manifest.
        let mut m = manifest::read_manifest(&config.den_home, active_env)?;
        m.packages.insert(info.name.clone(), pkg_version.clone());
        manifest::write_manifest(&config.den_home, active_env, &m)?;
    }

    // Re-materialise.
    env::materialise(&config.den_home, &config.cellar, active_env)?;
    Ok(())
}

/// Pour a bottle without console output (for daemon use).
async fn pour_bottle_quiet(
    client: &Client,
    config: &Config,
    info: &formula::FormulaInfo,
) -> anyhow::Result<()> {
    let macos = config
        .macos_version
        .as_ref()
        .ok_or_else(|| anyhow::anyhow!("cannot determine macOS version"))?;

    let bottle_spec = info
        .bottle
        .as_ref()
        .and_then(|b| b.stable.as_ref())
        .ok_or_else(|| anyhow::anyhow!("no bottle for {}", info.name))?;

    let tags: Vec<String> = bottle_spec.files.keys().cloned().collect();
    let tag = platform::best_bottle_tag(config.arch, macos, &tags)
        .ok_or_else(|| anyhow::anyhow!("no bottle for {} on this platform", info.name))?;

    let bottle_file = &bottle_spec.files[&tag];
    let ghcr_path = formula::ghcr_path(&info.name);
    let bottle_cache = config.den_home.join("cache").join("bottles");

    let data = bottle::fetch_bottle(client, bottle_file, &ghcr_path, &bottle_cache).await?;
    let poured = bottle::pour_bottle(&data, &config.cellar)?;
    tab::write_tab(&poured, &config.arch.to_string())?;

    log(
        &config.den_home,
        &format!("poured {} {}", info.name, info.pkg_version()),
    );
    Ok(())
}

/// Generate launchd plist content.
pub fn launchd_plist(den_binary: &Path, den_home: &Path) -> String {
    format!(
        r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>dev.den.daemon</string>
    <key>ProgramArguments</key>
    <array>
        <string>{}</string>
        <string>daemon</string>
        <string>run</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>{}/daemon.stdout.log</string>
    <key>StandardErrorPath</key>
    <string>{}/daemon.stderr.log</string>
</dict>
</plist>"#,
        den_binary.display(),
        den_home.display(),
        den_home.display(),
    )
}

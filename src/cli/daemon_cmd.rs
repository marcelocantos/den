// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use clap::Subcommand;

use crate::config::Config;
#[cfg(unix)]
use crate::daemon;
use crate::settings;

#[derive(Subcommand)]
pub(super) enum DaemonCommand {
    /// Run the daemon in the foreground (use with launchd or nohup)
    Run,
    /// Stop the running daemon
    Stop,
    /// Show daemon status and pending upgrades
    Status,
    /// Install the launchd plist for auto-start at login
    Install,
    /// Remove the launchd plist
    Uninstall,
}

#[cfg(not(unix))]
pub(super) async fn run_daemon_command(
    _config: &Config,
    _command: DaemonCommand,
) -> anyhow::Result<()> {
    anyhow::bail!("daemon is not supported on this platform")
}

#[cfg(unix)]
pub(super) async fn run_daemon_command(
    config: &Config,
    command: DaemonCommand,
) -> anyhow::Result<()> {
    match command {
        DaemonCommand::Run => daemon::run(config).await,
        DaemonCommand::Stop => {
            // Verify daemon is actually running via flock before signalling.
            if !daemon::is_running(&config.den_home) {
                // Clean up stale PID file if present.
                let pid_path = config.den_home.join("daemon.pid");
                let _ = std::fs::remove_file(&pid_path);
                anyhow::bail!("daemon is not running");
            }

            let pid_path = config.den_home.join("daemon.pid");
            let pid_str = std::fs::read_to_string(&pid_path)
                .map_err(|_| anyhow::anyhow!("daemon is not running"))?;
            let pid: i32 = pid_str.trim().parse()?;

            if pid <= 0 {
                anyhow::bail!("invalid PID in daemon.pid: {pid}");
            }

            let result = unsafe { libc::kill(pid, libc::SIGTERM) };
            if result != 0 {
                anyhow::bail!("failed to send SIGTERM to PID {pid}");
            }
            println!("Sent SIGTERM to daemon (PID {pid}).");
            Ok(())
        }
        DaemonCommand::Status => {
            if daemon::is_running(&config.den_home) {
                let pid_str =
                    std::fs::read_to_string(config.den_home.join("daemon.pid")).unwrap_or_default();
                println!("Daemon: running (PID {})", pid_str.trim());
            } else {
                println!("Daemon: not running");
            }

            let state = daemon::read_state(&config.den_home);
            if let Some(last) = state.last_check {
                let ago = daemon::now_secs().saturating_sub(last);
                println!("Last check: {}s ago", ago);
            } else {
                println!("Last check: never");
            }

            if state.pending.is_empty() {
                println!("Pending upgrades: none");
            } else {
                println!("Pending upgrades:");
                for p in &state.pending {
                    println!("  {} {} -> {}", p.name, p.installed, p.available);
                }
            }

            let s = settings::read(&config.den_home);
            println!(
                "Auto-download: {}",
                if s.daemon.auto_download { "on" } else { "off" }
            );
            println!(
                "Auto-upgrade: {}",
                if s.daemon.auto_upgrade { "on" } else { "off" }
            );
            if let Some(ref window) = s.daemon.upgrade_window {
                println!("Upgrade window: {window}");
            }

            Ok(())
        }
        DaemonCommand::Install => {
            #[cfg(not(target_os = "macos"))]
            anyhow::bail!("daemon install via launchd is only supported on macOS");
            #[cfg(target_os = "macos")]
            {
                let exe = std::env::current_exe()?;
                let plist = daemon::launchd_plist(&exe, &config.den_home);
                let plist_path = dirs::home_dir()
                    .ok_or_else(|| anyhow::anyhow!("cannot determine home directory"))?
                    .join("Library/LaunchAgents/dev.den.daemon.plist");

                let parent = plist_path
                    .parent()
                    .ok_or_else(|| anyhow::anyhow!("plist path has no parent"))?;
                std::fs::create_dir_all(parent)?;
                let mut tmp = tempfile::NamedTempFile::new_in(parent)?;
                std::io::Write::write_all(&mut tmp, plist.as_bytes())?;
                tmp.persist(&plist_path)?;

                let status = std::process::Command::new("launchctl")
                    .args(["load", "-w"])
                    .arg(&plist_path)
                    .status()?;

                if status.success() {
                    println!("Daemon installed and started.");
                    println!("  Plist: {}", plist_path.display());
                } else {
                    anyhow::bail!("failed to load launchd plist");
                }
                Ok(())
            }
        }
        DaemonCommand::Uninstall => {
            #[cfg(not(target_os = "macos"))]
            anyhow::bail!("daemon uninstall via launchd is only supported on macOS");
            #[cfg(target_os = "macos")]
            {
                let plist_path = dirs::home_dir()
                    .ok_or_else(|| anyhow::anyhow!("cannot determine home directory"))?
                    .join("Library/LaunchAgents/dev.den.daemon.plist");

                if plist_path.exists() {
                    let _ = std::process::Command::new("launchctl")
                        .args(["unload", "-w"])
                        .arg(&plist_path)
                        .status();
                    std::fs::remove_file(&plist_path)?;
                    println!("Daemon uninstalled.");
                } else {
                    println!("Daemon is not installed.");
                }
                Ok(())
            }
        }
    }
}

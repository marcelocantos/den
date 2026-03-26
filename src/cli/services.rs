// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use clap::Subcommand;

use crate::config::Config;
use crate::env;
#[cfg(target_os = "macos")]
use crate::service;

#[derive(Subcommand)]
pub(super) enum ServiceCommand {
    /// List all services
    List,
    /// Start a service
    Start {
        /// Service/formula name
        name: String,
    },
    /// Stop a service
    Stop {
        /// Service/formula name
        name: String,
    },
    /// Restart a service
    Restart {
        /// Service/formula name
        name: String,
    },
}

pub(super) fn run_services_command(
    config: &Config,
    command: Option<ServiceCommand>,
) -> anyhow::Result<()> {
    #[cfg(not(target_os = "macos"))]
    {
        let _ = (config, command);
        anyhow::bail!("service management is only supported on macOS");
    }
    #[cfg(target_os = "macos")]
    {
        let env_path = env::active_env_path(&config.den_home);
        let env_dir = env::env_dir(&config.den_home, &env_path);
        let opt_dir = env_dir.join("opt");

        match command {
            Some(ServiceCommand::List) | None => {
                let services = service::list_services(&config.cellar, &opt_dir)?;
                if services.is_empty() {
                    println!("No services found.");
                } else {
                    for svc in &services {
                        let status = if svc.running { "running" } else { "stopped" };
                        println!("{} ({})", svc.name, status);
                    }
                }
                Ok(())
            }
            Some(ServiceCommand::Start { name }) => {
                let services = service::list_services(&config.cellar, &opt_dir)?;
                let svc = services
                    .iter()
                    .find(|s| s.name == name)
                    .ok_or_else(|| anyhow::anyhow!("no service found for '{name}'"))?;
                if svc.running {
                    println!("{name} is already running.");
                    return Ok(());
                }
                service::start_service(svc)?;
                println!("==> Started {name}.");
                Ok(())
            }
            Some(ServiceCommand::Stop { name }) => {
                let services = service::list_services(&config.cellar, &opt_dir)?;
                let svc = services
                    .iter()
                    .find(|s| s.name == name)
                    .ok_or_else(|| anyhow::anyhow!("no service found for '{name}'"))?;
                if !svc.running {
                    println!("{name} is not running.");
                    return Ok(());
                }
                service::stop_service(svc)?;
                println!("==> Stopped {name}.");
                Ok(())
            }
            Some(ServiceCommand::Restart { name }) => {
                let services = service::list_services(&config.cellar, &opt_dir)?;
                let svc = services
                    .iter()
                    .find(|s| s.name == name)
                    .ok_or_else(|| anyhow::anyhow!("no service found for '{name}'"))?;
                service::restart_service(svc)?;
                println!("==> Restarted {name}.");
                Ok(())
            }
        }
    }
}

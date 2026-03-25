// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::fmt;
use std::path::PathBuf;

use crate::platform::{self, Arch, MacOsVersion};

#[derive(Debug, Clone)]
pub struct Config {
    pub prefix: PathBuf,
    pub cellar: PathBuf,
    pub caskroom: PathBuf,
    pub cache: PathBuf,
    pub taps: PathBuf,
    pub den_home: PathBuf,
    pub arch: Arch,
    pub macos_version: Option<MacOsVersion>,
}

impl Config {
    pub fn detect() -> anyhow::Result<Self> {
        let arch = platform::detect_arch()?;

        let prefix = std::env::var("HOMEBREW_PREFIX")
            .map(PathBuf::from)
            .unwrap_or_else(|_| {
                #[cfg(target_os = "linux")]
                {
                    PathBuf::from("/home/linuxbrew/.linuxbrew")
                }
                #[cfg(not(target_os = "linux"))]
                {
                    match arch {
                        Arch::Arm64 => PathBuf::from("/opt/homebrew"),
                        Arch::X86_64 => PathBuf::from("/usr/local"),
                    }
                }
            });

        let cellar = std::env::var("HOMEBREW_CELLAR")
            .map(PathBuf::from)
            .unwrap_or_else(|_| prefix.join("Cellar"));

        let caskroom = prefix.join("Caskroom");

        let cache = std::env::var("HOMEBREW_CACHE")
            .map(PathBuf::from)
            .unwrap_or_else(|_| {
                let home = dirs::home_dir().unwrap_or_else(|| PathBuf::from("/tmp"));
                #[cfg(target_os = "linux")]
                {
                    home.join(".cache/Homebrew")
                }
                #[cfg(not(target_os = "linux"))]
                {
                    home.join("Library/Caches/Homebrew")
                }
            });

        let taps = prefix.join("Library/Taps");

        let den_home = std::env::var("DEN_HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|_| {
                dirs::home_dir()
                    .unwrap_or_else(|| PathBuf::from("/tmp"))
                    .join(".den")
            });

        let macos_version = platform::detect_macos_version();

        Ok(Config {
            prefix,
            cellar,
            caskroom,
            cache,
            taps,
            den_home,
            arch,
            macos_version,
        })
    }
}

impl fmt::Display for Config {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(f, "HOMEBREW_PREFIX: {}", self.prefix.display())?;
        writeln!(f, "HOMEBREW_CELLAR: {}", self.cellar.display())?;
        writeln!(f, "HOMEBREW_CASKROOM: {}", self.caskroom.display())?;
        writeln!(f, "HOMEBREW_CACHE: {}", self.cache.display())?;
        writeln!(f, "HOMEBREW_TAPS: {}", self.taps.display())?;
        writeln!(f, "DEN_HOME: {}", self.den_home.display())?;
        writeln!(f, "ARCH: {}", self.arch)?;
        if let Some(ref ver) = self.macos_version {
            writeln!(f, "macOS: {ver}")?;
        }
        Ok(())
    }
}

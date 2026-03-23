// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::fmt;
use std::path::PathBuf;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Arch {
    Arm64,
    X86_64,
}

impl fmt::Display for Arch {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Arch::Arm64 => write!(f, "arm64"),
            Arch::X86_64 => write!(f, "x86_64"),
        }
    }
}

#[derive(Debug, Clone)]
pub struct MacOsVersion {
    pub major: u32,
    pub minor: u32,
    pub patch: u32,
}

impl fmt::Display for MacOsVersion {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}.{}.{}", self.major, self.minor, self.patch)
    }
}

#[derive(Debug, Clone)]
pub struct Config {
    pub prefix: PathBuf,
    pub cellar: PathBuf,
    pub caskroom: PathBuf,
    pub cache: PathBuf,
    pub taps: PathBuf,
    pub arch: Arch,
    pub macos_version: Option<MacOsVersion>,
}

impl Config {
    pub fn detect() -> anyhow::Result<Self> {
        let arch = detect_arch()?;

        let prefix = std::env::var("HOMEBREW_PREFIX")
            .map(PathBuf::from)
            .unwrap_or_else(|_| match arch {
                Arch::Arm64 => PathBuf::from("/opt/homebrew"),
                Arch::X86_64 => PathBuf::from("/usr/local"),
            });

        let cellar = std::env::var("HOMEBREW_CELLAR")
            .map(PathBuf::from)
            .unwrap_or_else(|_| prefix.join("Cellar"));

        let caskroom = prefix.join("Caskroom");

        let cache = std::env::var("HOMEBREW_CACHE")
            .map(PathBuf::from)
            .unwrap_or_else(|_| {
                dirs::home_dir()
                    .unwrap_or_else(|| PathBuf::from("/tmp"))
                    .join("Library/Caches/Homebrew")
            });

        let taps = prefix.join("Library/Taps");

        let macos_version = detect_macos_version();

        Ok(Config {
            prefix,
            cellar,
            caskroom,
            cache,
            taps,
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
        writeln!(f, "ARCH: {}", self.arch)?;
        if let Some(ref ver) = self.macos_version {
            writeln!(f, "macOS: {ver}")?;
        }
        Ok(())
    }
}

fn detect_arch() -> anyhow::Result<Arch> {
    match std::env::consts::ARCH {
        "aarch64" => Ok(Arch::Arm64),
        "x86_64" => Ok(Arch::X86_64),
        other => anyhow::bail!("unsupported architecture: {other}"),
    }
}

fn detect_macos_version() -> Option<MacOsVersion> {
    if std::env::consts::OS != "macos" {
        return None;
    }

    let output = std::process::Command::new("sw_vers")
        .arg("-productVersion")
        .output()
        .ok()?;

    let version_str = String::from_utf8(output.stdout).ok()?;
    let parts: Vec<u32> = version_str
        .trim()
        .split('.')
        .filter_map(|s| s.parse().ok())
        .collect();

    Some(MacOsVersion {
        major: *parts.first()?,
        minor: *parts.get(1).unwrap_or(&0),
        patch: *parts.get(2).unwrap_or(&0),
    })
}

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::fmt;

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

pub fn detect_arch() -> anyhow::Result<Arch> {
    match std::env::consts::ARCH {
        "aarch64" => Ok(Arch::Arm64),
        "x86_64" => Ok(Arch::X86_64),
        other => anyhow::bail!("unsupported architecture: {other}"),
    }
}

pub fn detect_macos_version() -> Option<MacOsVersion> {
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

/// Known macOS major version to codename mapping.
fn macos_codename(major: u32) -> Option<&'static str> {
    match major {
        26 => Some("tahoe"),
        15 => Some("sequoia"),
        14 => Some("sonoma"),
        13 => Some("ventura"),
        12 => Some("monterey"),
        _ => None,
    }
}

/// Return the bottle tag for the current platform (e.g. "arm64_tahoe").
pub fn bottle_tag(arch: Arch, macos: &MacOsVersion) -> Option<String> {
    let codename = macos_codename(macos.major)?;
    Some(match arch {
        Arch::Arm64 => format!("arm64_{codename}"),
        Arch::X86_64 => codename.to_string(),
    })
}

/// Return bottle tags to try, from current OS down to older versions.
pub fn bottle_tag_candidates(arch: Arch, macos: &MacOsVersion) -> Vec<String> {
    let codenames_by_recency: &[(u32, &str)] = &[
        (26, "tahoe"),
        (15, "sequoia"),
        (14, "sonoma"),
        (13, "ventura"),
        (12, "monterey"),
    ];

    codenames_by_recency
        .iter()
        .filter(|(major, _)| *major <= macos.major)
        .map(|(_, codename)| match arch {
            Arch::Arm64 => format!("arm64_{codename}"),
            Arch::X86_64 => codename.to_string(),
        })
        .collect()
}

/// Pick the best available bottle tag from a set of available tags.
pub fn best_bottle_tag(
    arch: Arch,
    macos: &MacOsVersion,
    available: &[String],
) -> Option<String> {
    let candidates = bottle_tag_candidates(arch, macos);
    candidates.into_iter().find(|tag| available.contains(tag))
}

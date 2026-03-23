// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::path::Path;

use serde::{Deserialize, Serialize};

/// All den settings in one file: ~/.den/config.json
#[derive(Debug, Default, Serialize, Deserialize)]
pub struct Settings {
    /// Background maintenance.
    #[serde(default)]
    pub daemon: DaemonSettings,

    /// User declined the first-run migration prompt.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub migration_declined: bool,

    /// Search.
    #[serde(default)]
    pub search: SearchSettings,
}

#[derive(Debug, Default, Serialize, Deserialize)]
pub struct DaemonSettings {
    /// Automatically apply upgrades (default: false).
    #[serde(default)]
    pub auto_upgrade: bool,
    /// Maintenance window, e.g. "3:00-5:00". None = upgrade when idle.
    #[serde(default)]
    pub upgrade_window: Option<String>,
    /// Check interval in seconds (default: 21600 = 6 hours).
    #[serde(default)]
    pub interval_secs: Option<u64>,
}

#[derive(Debug, Default, Serialize, Deserialize)]
pub struct SearchSettings {
    /// Search provider: "keyword", "embedding", or a model name.
    #[serde(default)]
    pub provider: Option<String>,
}

fn config_path(den_home: &Path) -> std::path::PathBuf {
    den_home.join("config.json")
}

/// Read settings, returning defaults if the file doesn't exist.
pub fn read(den_home: &Path) -> Settings {
    let path = config_path(den_home);
    std::fs::read_to_string(&path)
        .ok()
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or_default()
}

/// Write settings.
pub fn write(den_home: &Path, settings: &Settings) -> anyhow::Result<()> {
    std::fs::create_dir_all(den_home)?;
    let json = serde_json::to_string_pretty(settings)?;
    std::fs::write(config_path(den_home), json)?;
    Ok(())
}

/// Read a single setting by dotted key (e.g. "daemon.auto_upgrade").
#[allow(dead_code)]
pub fn get(den_home: &Path, key: &str) -> anyhow::Result<String> {
    let s = read(den_home);
    let json = serde_json::to_value(&s)?;
    let value = resolve_key(&json, key)
        .ok_or_else(|| anyhow::anyhow!("unknown setting: {key}"))?;
    Ok(format_value(value))
}

/// Set a single setting by dotted key.
pub fn set(den_home: &Path, key: &str, value: &str) -> anyhow::Result<()> {
    let mut s = read(den_home);
    let mut json = serde_json::to_value(&s)?;
    set_key(&mut json, key, value)?;
    s = serde_json::from_value(json)?;
    write(den_home, &s)
}

/// Show all settings as formatted output.
pub fn display_all(den_home: &Path) -> String {
    let s = read(den_home);
    serde_json::to_string_pretty(&s).unwrap_or_else(|_| "{}".to_string())
}

fn resolve_key<'a>(json: &'a serde_json::Value, key: &str) -> Option<&'a serde_json::Value> {
    let mut current = json;
    for part in key.split('.') {
        current = current.get(part)?;
    }
    Some(current)
}

fn set_key(json: &mut serde_json::Value, key: &str, value: &str) -> anyhow::Result<()> {
    let parts: Vec<&str> = key.split('.').collect();
    let mut current = json;

    for part in &parts[..parts.len() - 1] {
        current = current
            .get_mut(*part)
            .ok_or_else(|| anyhow::anyhow!("unknown setting group: {part}"))?;
    }

    let last = parts.last().unwrap();
    let obj = current
        .as_object_mut()
        .ok_or_else(|| anyhow::anyhow!("not an object at {key}"))?;

    if !obj.contains_key(*last) {
        anyhow::bail!("unknown setting: {key}");
    }

    // Infer type from existing value.
    let existing = &obj[*last];
    let new_value = match existing {
        serde_json::Value::Bool(_) => {
            serde_json::Value::Bool(value.parse().map_err(|_| {
                anyhow::anyhow!("expected true or false for {key}")
            })?)
        }
        serde_json::Value::Number(_) => {
            if let Ok(n) = value.parse::<u64>() {
                serde_json::Value::Number(n.into())
            } else {
                anyhow::bail!("expected a number for {key}");
            }
        }
        serde_json::Value::Null | serde_json::Value::String(_) => {
            if value == "null" || value == "none" {
                serde_json::Value::Null
            } else {
                serde_json::Value::String(value.to_string())
            }
        }
        _ => serde_json::Value::String(value.to_string()),
    };

    obj.insert(last.to_string(), new_value);
    Ok(())
}

fn format_value(v: &serde_json::Value) -> String {
    match v {
        serde_json::Value::String(s) => s.clone(),
        serde_json::Value::Bool(b) => b.to_string(),
        serde_json::Value::Number(n) => n.to_string(),
        serde_json::Value::Null => "null".to_string(),
        other => other.to_string(),
    }
}

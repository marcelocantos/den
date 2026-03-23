// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use std::path::PathBuf;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("formula not found: {0}")]
    FormulaNotFound(String),

    #[error("cask not found: {0}")]
    CaskNotFound(String),

    #[error("keg not found: {name} {version}")]
    KegNotFound { name: String, version: String },

    #[error("dependency cycle detected: {0:?}")]
    DependencyCycle(Vec<String>),

    #[error("checksum mismatch for {path}: expected {expected}, got {actual}")]
    ChecksumMismatch {
        path: PathBuf,
        expected: String,
        actual: String,
    },

    #[error("link conflict: {path} already exists (owned by {owner})")]
    LinkConflict { path: PathBuf, owner: String },

    #[error("unsupported platform: {0}")]
    UnsupportedPlatform(String),

    #[error("homebrew prefix not found")]
    PrefixNotFound,

    #[error(transparent)]
    Io(#[from] std::io::Error),

    #[error(transparent)]
    Other(#[from] anyhow::Error),
}

pub type Result<T> = std::result::Result<T, Error>;

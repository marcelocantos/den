// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Test manifest slug/path conversion logic. Since den is a binary
// crate, we replicate the logic here for testing.

#[test]
fn env_slug_root() {
    assert_eq!(slug("/"), "ROOT");
}

#[test]
fn env_slug_simple() {
    assert_eq!(slug("/ml"), "ml");
}

#[test]
fn env_slug_nested() {
    assert_eq!(slug("/work/ml"), "work--ml");
}

#[test]
fn env_slug_deep() {
    assert_eq!(slug("/work/ml/experiment"), "work--ml--experiment");
}

#[test]
fn slug_to_path_root() {
    assert_eq!(to_path("ROOT"), "/");
}

#[test]
fn slug_to_path_simple() {
    assert_eq!(to_path("ml"), "/ml");
}

#[test]
fn slug_to_path_nested() {
    assert_eq!(to_path("work--ml"), "/work/ml");
}

/// Replicate the slug logic here since we can't import from the
/// binary crate directly.
fn slug(env_path: &str) -> String {
    let trimmed = env_path.trim_matches('/');
    if trimmed.is_empty() {
        "ROOT".to_string()
    } else {
        trimmed.replace('/', "--")
    }
}

fn to_path(slug: &str) -> String {
    if slug == "ROOT" {
        "/".to_string()
    } else {
        format!("/{}", slug.replace("--", "/"))
    }
}

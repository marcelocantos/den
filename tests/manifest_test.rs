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
fn env_slug_with_dash() {
    // Dashes in components get encoded so they don't collide with the separator.
    assert_eq!(slug("/my-project"), "my%2Dproject");
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

#[test]
fn round_trip_with_double_dash_in_component() {
    // Path component contains "--" which is the slug separator.
    // Must survive a round-trip.
    let path = "/work/my--project";
    let s = slug(path);
    assert_eq!(to_path(&s), path);
}

#[test]
fn round_trip_simple() {
    let path = "/work/ml";
    assert_eq!(to_path(&slug(path)), path);
}

#[test]
fn round_trip_root() {
    let path = "/";
    assert_eq!(to_path(&slug(path)), path);
}

#[test]
fn round_trip_percent_in_component() {
    // Path component contains "%" which is the escape character.
    let path = "/work/50%off";
    let s = slug(path);
    assert_eq!(to_path(&s), path);
}

#[test]
fn round_trip_multiple_dashes() {
    let path = "/a--b/c--d";
    let s = slug(path);
    assert_eq!(to_path(&s), path);
}

#[test]
fn round_trip_dash_at_boundary() {
    // Single dashes at the start/end of components should not be
    // confused with the separator.
    let path = "/a-/b";
    let s = slug(path);
    assert_eq!(to_path(&s), path);
}

#[test]
fn round_trip_single_dash_component() {
    let path = "/-";
    let s = slug(path);
    assert_eq!(to_path(&s), path);
}

#[test]
fn round_trip_literal_percent_2d() {
    // A component that is literally "%2D" must survive round-trip.
    // This caught a bug where chained replace calls corrupted the decode.
    let path = "/work/%2D";
    let s = slug(path);
    assert_eq!(to_path(&s), path);
}

#[test]
fn round_trip_percent_25() {
    // Component containing "%25" (literal percent-two-five).
    let path = "/a/%25/b";
    let s = slug(path);
    assert_eq!(to_path(&s), path);
}

/// Encode a single path component for use in a slug.
fn encode_component(component: &str) -> String {
    component.replace('%', "%25").replace('-', "%2D")
}

/// Decode a single slug component back to the original path component.
///
/// Uses single-pass decoding to avoid chained-replace ambiguity.
fn decode_component(component: &str) -> String {
    let mut result = String::with_capacity(component.len());
    let bytes = component.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            let seq = &component[i..i + 3];
            match seq {
                "%2D" => {
                    result.push('-');
                    i += 3;
                }
                "%25" => {
                    result.push('%');
                    i += 3;
                }
                _ => {
                    result.push('%');
                    i += 1;
                }
            }
        } else {
            result.push(bytes[i] as char);
            i += 1;
        }
    }
    result
}

/// Replicate the slug logic here since we can't import from the
/// binary crate directly.
fn slug(env_path: &str) -> String {
    let trimmed = env_path.trim_matches('/');
    if trimmed.is_empty() {
        "ROOT".to_string()
    } else {
        trimmed
            .split('/')
            .map(encode_component)
            .collect::<Vec<_>>()
            .join("--")
    }
}

fn to_path(slug: &str) -> String {
    if slug == "ROOT" {
        "/".to_string()
    } else {
        let components: Vec<String> = slug.split("--").map(decode_component).collect();
        format!("/{}", components.join("/"))
    }
}

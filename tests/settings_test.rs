// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use assert_cmd::Command;

#[test]
fn set_and_get_bool() {
    let tmp = tempfile::tempdir().unwrap();
    let den_home = tmp.path();

    // Set a boolean value.
    Command::cargo_bin("den")
        .unwrap()
        .args(["set", "daemon.auto_upgrade", "true"])
        .env("DEN_HOME", den_home)
        .assert()
        .success()
        .stdout(predicates::prelude::predicate::str::contains("true"));

    // Verify it persists.
    Command::cargo_bin("den")
        .unwrap()
        .arg("settings")
        .env("DEN_HOME", den_home)
        .assert()
        .success()
        .stdout(predicates::prelude::predicate::str::contains("\"auto_upgrade\": true"));
}

#[test]
fn set_string_value() {
    let tmp = tempfile::tempdir().unwrap();
    let den_home = tmp.path();

    Command::cargo_bin("den")
        .unwrap()
        .args(["set", "daemon.upgrade_window", "3:00-5:00"])
        .env("DEN_HOME", den_home)
        .assert()
        .success();

    Command::cargo_bin("den")
        .unwrap()
        .arg("settings")
        .env("DEN_HOME", den_home)
        .assert()
        .success()
        .stdout(predicates::prelude::predicate::str::contains("3:00-5:00"));
}

#[test]
fn set_invalid_key() {
    let tmp = tempfile::tempdir().unwrap();

    Command::cargo_bin("den")
        .unwrap()
        .args(["set", "nonexistent.key", "value"])
        .env("DEN_HOME", tmp.path())
        .assert()
        .failure();
}

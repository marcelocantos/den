// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use assert_cmd::Command;
use predicates::prelude::*;

#[test]
fn version_flag() {
    Command::cargo_bin("den")
        .unwrap()
        .arg("--version")
        .assert()
        .success()
        .stdout(predicate::str::starts_with("den "));
}

#[test]
fn help_flag() {
    Command::cargo_bin("den")
        .unwrap()
        .arg("--help")
        .assert()
        .success()
        .stdout(predicate::str::contains(
            "universal development environment manager",
        ));
}

#[test]
fn config_command() {
    Command::cargo_bin("den")
        .unwrap()
        .arg("config")
        .assert()
        .success()
        .stdout(predicate::str::contains("HOMEBREW_CELLAR"))
        .stdout(predicate::str::contains("DEN_HOME"))
        .stdout(predicate::str::contains("ARCH"));
}

#[test]
fn env_list_with_fresh_home() {
    let tmp = tempfile::tempdir().unwrap();
    Command::cargo_bin("den")
        .unwrap()
        .args(["env", "list"])
        .env("DEN_HOME", tmp.path())
        .assert()
        .success();
}

#[test]
fn settings_with_fresh_home() {
    let tmp = tempfile::tempdir().unwrap();
    Command::cargo_bin("den")
        .unwrap()
        .arg("settings")
        .env("DEN_HOME", tmp.path())
        .assert()
        .success()
        .stdout(predicate::str::contains("daemon"))
        .stdout(predicate::str::contains("auto_upgrade"));
}

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

use assert_cmd::Command;
use predicates::prelude::*;

#[test]
fn env_create_and_list() {
    let tmp = tempfile::tempdir().unwrap();
    let den_home = tmp.path();

    // Create root.
    Command::cargo_bin("den")
        .unwrap()
        .args(["env", "create", "/"])
        .env("DEN_HOME", den_home)
        .assert()
        .success()
        .stdout(predicate::str::contains("Created environment '/'"));

    // Create child.
    Command::cargo_bin("den")
        .unwrap()
        .args(["env", "create", "/ml"])
        .env("DEN_HOME", den_home)
        .assert()
        .success()
        .stdout(predicate::str::contains("Created environment '/ml'"));

    // List should show both.
    Command::cargo_bin("den")
        .unwrap()
        .args(["env", "list"])
        .env("DEN_HOME", den_home)
        .assert()
        .success()
        .stdout(predicate::str::contains("/"))
        .stdout(predicate::str::contains("/ml"));
}

#[test]
fn env_create_child_without_parent_fails() {
    let tmp = tempfile::tempdir().unwrap();

    Command::cargo_bin("den")
        .unwrap()
        .args(["env", "create", "/work/ml"])
        .env("DEN_HOME", tmp.path())
        .assert()
        .failure()
        .stderr(predicate::str::contains("parent environment"));
}

#[test]
fn env_remove_root_fails() {
    let tmp = tempfile::tempdir().unwrap();
    let den_home = tmp.path();

    // Create root first.
    Command::cargo_bin("den")
        .unwrap()
        .args(["env", "create", "/"])
        .env("DEN_HOME", den_home)
        .assert()
        .success();

    // Try to remove root.
    Command::cargo_bin("den")
        .unwrap()
        .args(["env", "remove", "/"])
        .env("DEN_HOME", den_home)
        .assert()
        .failure()
        .stderr(predicate::str::contains("cannot remove the root"));
}

#[test]
fn env_show_empty() {
    let tmp = tempfile::tempdir().unwrap();
    let den_home = tmp.path();

    Command::cargo_bin("den")
        .unwrap()
        .args(["env", "create", "/"])
        .env("DEN_HOME", den_home)
        .assert()
        .success();

    Command::cargo_bin("den")
        .unwrap()
        .args(["env", "show", "/"])
        .env("DEN_HOME", den_home)
        .assert()
        .success()
        .stdout(predicate::str::contains("no packages"));
}

#[test]
fn init_outputs_shell_integration() {
    let tmp = tempfile::tempdir().unwrap();

    Command::cargo_bin("den")
        .unwrap()
        .arg("init")
        .env("DEN_HOME", tmp.path())
        .assert()
        .success()
        .stdout(predicate::str::contains("DEN_HOME"))
        .stdout(predicate::str::contains("DEN_ENV"))
        .stdout(predicate::str::contains("LIBRARY_PATH"))
        .stdout(predicate::str::contains("PKG_CONFIG_PATH"))
        .stdout(predicate::str::contains("den()"));
}

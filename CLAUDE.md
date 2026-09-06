# den

den: a C++ universal development environment manager that installs Homebrew (and pip, npm, go, cargo) packages into named, multi-version environments with background upgrades.

@AGENTS.md

## Delivery

delivery: merged to master, CI green

## Gates

profile: cli

## Release

homebrew_tap: disabled

den replaces Homebrew, so distributing den via a Homebrew tap is
circular. Binaries ship via GitHub Releases and the `install.sh`
installer. The `/release` skill honours this directive and skips all
tap-related phases.

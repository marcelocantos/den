// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

// Package pinning (brew pin / unpin parity).
//
// A pin holds a package at the version that was active when it was pinned:
// `den upgrade` (all or named) and the daemon's auto-upgrade pass skip it
// until it is unpinned. Pins are global to den_home — they are keyed by
// package name, like upgrades — and live in den_home/pins.json:
//
//   {"schema_version": 1, "pins": {"jq": {"version": "1.7.1"}}}
//
// Reads fail closed: a corrupt pins.json throws rather than reading as "no
// pins", so a damaged file can never silently release every pin into an
// upgrade.

/// name → pinned version.
using PinMap = std::map<std::string, std::string>;

fs::path pins_path(const fs::path& den_home);

/// Read all pins. A missing file is an empty map; a malformed one throws
/// UserError.
PinMap read_pins(const fs::path& den_home);

/// Atomically replace the pin file with `pins`.
void write_pins(const fs::path& den_home, const PinMap& pins);

enum class PinOutcome { Pinned, AlreadyPinned };

/// Pin `name` at `version`. Re-pinning an already pinned package keeps the
/// original version (brew: "already pinned") and reports AlreadyPinned.
PinOutcome pin_package(const fs::path& den_home, const std::string& name,
                       const std::string& version);

/// Remove the pin for `name`. Returns false when it was not pinned.
bool unpin_package(const fs::path& den_home, const std::string& name);

/// One candidate upgrade: name, current version, available version.
struct UpgradeCandidate {
    std::string name;
    std::string installed;
    std::string available;
};

struct PinPartition {
    std::vector<UpgradeCandidate> upgrade; // not pinned — proceed
    std::vector<UpgradeCandidate> held;    // pinned — skip
};

/// Split candidates into those to upgrade and those held back by a pin.
PinPartition partition_pinned(const std::vector<UpgradeCandidate>& candidates, const PinMap& pins);

} // namespace den

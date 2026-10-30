// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"
#include "../index/package.h"
#include "../provider/package_provider.h"

#include <string>
#include <vector>

namespace den {

/// The refusal (or, for one already up to date, the warning) for a named
/// formula or cask the index marks disabled, e.g. "'rye' is disabled
/// upstream: it is not maintained upstream (disabled on 2026-08-18;
/// replacement: uv)". The reason, date and replacement come from the
/// index's disable! fields and are left out when it has none. Homebrew's
/// reason symbols read as brew words them (its formula or cask list, by the
/// package's kind); a custom reason is quoted as-is.
std::string disabled_package_message(const Package& pkg);

/// Whether the index entry is a cask (an app bundle or cask artifacts).
bool is_cask_package(const Package& pkg);

/// "'<dependent>' needs '<dep>', which is disabled upstream: it <reason>
/// (disabled on <date>; replacement: <name>)", from disabled_package_message.
std::string disabled_dependency_message(const std::string& dependent, const Package& dep);

/// The packages in `name`'s runtime dependency closure (per `idx`, `name`
/// itself left out) that the index marks disabled and `provider` does not
/// have installed at the index version, i.e. the disabled packages
/// installing `name` would pour. One already installed at the index version
/// is satisfied and not listed, as brew skips it.
std::vector<const Package*> disabled_dependencies(const Config& config,
                                                  const PackageProvider& provider,
                                                  const PackageIndex& idx, const std::string& name);

/// A disabled package that pouring a name at its index version would pour,
/// with the message that refuses it.
struct DisabledPour {
    const Package* pkg = nullptr; // the disabled package (the name or a dependency)
    std::string message;          // disabled_package_message or disabled_dependency_message
};

/// The one disabled-package check `den install`, `den upgrade` (named
/// packages' dependencies and revision-bumped dependents) and the daemon's
/// auto-upgrade share: the disabled packages pouring `name` at its index
/// version would pour. With `include_self`, `name` itself comes first when
/// the index marks it disabled; then each of disabled_dependencies(name).
/// Empty when nothing disabled would be poured. Callers decide what a hit
/// means (the CLI's --force lets a formula through; the daemon has no
/// bypass).
std::vector<DisabledPour> disabled_pours(const Config& config, const PackageProvider& provider,
                                         const PackageIndex& idx, const std::string& name,
                                         bool include_self);

} // namespace den

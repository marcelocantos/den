// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "package.h"

#include <filesystem>
#include <string>

namespace den {

/// The Cellar Homebrew builds fixed-cellar bottles for, by bottle tag:
/// /opt/homebrew/Cellar (macOS arm64_*), /home/linuxbrew/.linuxbrew/Cellar
/// (*_linux), /usr/local/Cellar (other macOS tags).
std::string standard_bottle_cellar(const std::string& tag);

/// The Cellar a bottle must be poured into, or "" when it can be poured
/// anywhere (`cellar` :any or :any_skip_relocation). For a fixed-cellar
/// bottle this is Homebrew's `cellar` path, or the standard Cellar for the
/// tag when the index entry predates den recording it.
std::string required_bottle_cellar(const std::string& tag, const Package::Archive& archive);

/// True when the bottle can be poured into `store` (den's Cellar).
bool bottle_pours_into(const std::string& tag, const Package::Archive& archive,
                       const std::filesystem::path& store);

/// The error den reports when a fixed-cellar bottle doesn't match its Cellar,
/// following Homebrew: such a bottle can't be relocated, so the only options
/// are a source build or a prefix that matches the bottle.
std::string fixed_cellar_refusal(const Package& pkg, const std::string& tag,
                                 const std::string& required, const std::filesystem::path& store);

} // namespace den

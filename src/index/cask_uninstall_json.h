// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "package.h"

#include <nlohmann/json.hpp>

namespace den {

/// Merge one Homebrew `uninstall` directive object (as found in cask.json
/// `artifacts[].uninstall[]`) — or den's own serialised form — into `out`.
/// Unknown/unsupported directives (script, early_script, kext, login_item,
/// …) are recorded by name in `out.unsupported`, never dropped.
void merge_uninstall_directives(const nlohmann::json& obj, CaskUninstall& out);

/// Merge every `uninstall` stanza found in a Homebrew cask `artifacts` array.
void parse_cask_uninstall(const nlohmann::json& artifacts, CaskUninstall& out);

/// Serialise to den's form (same shape as Homebrew's, plus "unsupported").
nlohmann::json uninstall_to_json(const CaskUninstall& u);

} // namespace den

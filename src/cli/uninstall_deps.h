// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"
#include "../index/package.h"

#include <map>
#include <string>
#include <vector>

namespace den {

/// An installed Homebrew package that needs another package at runtime.
struct RuntimeDependent {
    std::string env;  // environment whose manifest lists it
    std::string name; // the dependent package
    bool operator<(const RuntimeDependent& o) const {
        return env != o.env ? env < o.env : name < o.name;
    }
    bool operator==(const RuntimeDependent& o) const { return env == o.env && name == o.name; }
};

/// brew parity for `uninstall`: for each of `names` (being removed from
/// `active_env`), the installed Homebrew packages in any environment whose
/// runtime dependency closure includes it. Runtime dependencies come from
/// the index (`dependencies`, never `build_dependencies`), falling back to
/// the keg's INSTALL_RECEIPT.json `runtime_dependencies` for packages the
/// index doesn't know. Packages removed in the same command don't count,
/// nor does a dependent whose environment still gets the package from
/// somewhere other than `active_env` (its own manifest or another
/// ancestor). Names with no dependents are omitted from the result.
std::map<std::string, std::vector<RuntimeDependent>>
find_runtime_dependents(const Config& config, const PackageIndex& idx,
                        const std::string& active_env, const std::vector<std::string>& names);

/// Human-readable refusal, modelled on brew's "Refusing to uninstall"
/// message, ending with the `--ignore-dependencies` override hint.
std::string
format_dependents_refusal(const std::map<std::string, std::vector<RuntimeDependent>>& dependents);

} // namespace den

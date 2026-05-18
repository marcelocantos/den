// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "package_provider.h"

namespace den {

bool PackageProvider::is_installed(const Config& config, std::string_view name,
                                   std::string_view version) const {
    for (const auto& pkg : list_installed(config)) {
        if (pkg.name == name && pkg.version == version) {
            return true;
        }
    }
    return false;
}

}  // namespace den

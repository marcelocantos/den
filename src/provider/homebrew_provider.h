// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../index/package.h"
#include "package_provider.h"

namespace den {

/// Homebrew implementation of `PackageProvider`. Wraps the bottle-pour
/// install/uninstall/list flow that den has used since v0.1.
///
/// `install` and any code path that needs to look up package metadata
/// require an index; `list_installed` and `binary_paths` only touch the
/// on-disk store and ignore the index. Pass `nullptr` when constructing
/// a provider that will only be used for read-only queries.
///
/// Lifetime: the provider stores a borrowed pointer to the index — the
/// caller must keep the index alive for as long as the provider is used.
class HomebrewProvider : public PackageProvider {
  public:
    explicit HomebrewProvider(const PackageIndex* idx);

    std::string_view provider_name() const override;
    bool accepts(std::string_view name) const override;
    InstallResult install(const Config& config, std::string_view name,
                          std::string_view version_hint) override;
    void uninstall(const Config& config, std::string_view name) override;
    std::vector<InstalledPackage> list_installed(const Config& config) const override;
    std::vector<fs::path> binary_paths(const Config& config,
                                       const InstalledPackage& pkg) const override;

  private:
    const PackageIndex* idx_;
};

} // namespace den

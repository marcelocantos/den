// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "package_provider.h"

namespace den {

/// npm package provider (🎯T39). Each package is installed into a private
/// prefix under `<den_home>/providers/npm/<name>/<version>/`, populating
/// `<root>/node_modules/` and `<root>/node_modules/.bin/`. The provider then
/// materialises a `<root>/bin/` directory of stable symlinks to each
/// installed CLI, which `binary_paths()` surfaces to the env exactly like a
/// Homebrew keg's `bin/`.
///
/// Installs are per-environment isolated — there is no global node_modules.
/// `accepts()` returns false (npm's namespace overlaps Homebrew's), so npm is
/// selected only via `--provider npm` or a manifest hint.
class NodeProvider : public PackageProvider {
  public:
    NodeProvider();

    std::string_view provider_name() const override;
    bool accepts(std::string_view name) const override;
    InstallResult install(const Config& config, std::string_view name,
                          std::string_view version_hint) override;
    void uninstall(const Config& config, std::string_view name) override;
    std::vector<InstalledPackage> list_installed(const Config& config) const override;
    fs::path package_root(const Config& config, std::string_view name,
                          std::string_view version) const override;
    std::vector<fs::path> binary_paths(const Config& config,
                                       const InstalledPackage& pkg) const override;
};

} // namespace den

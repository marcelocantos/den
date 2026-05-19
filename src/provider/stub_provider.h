// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "package_provider.h"

namespace den {

/// A deliberately minimal `PackageProvider` whose only job is to prove
/// that the multi-provider seams (registration, resolution dispatch,
/// per-provider manifest entries, env composition, uninstall) work for a
/// non-Homebrew provider — and that adding a new provider does not
/// require touching CLI, manifest, or env-composition code.
///
/// Storage layout: `<config.den_home>/providers/stub/<name>/<version>/bin/<name>`.
/// `install` writes a tiny executable shell script at that path.
/// `uninstall` removes the package's version directory.
///
/// `accepts(name)` is intentionally `false` for all names: the stub
/// never auto-claims a package and is only invoked when the user passes
/// `--provider stub` explicitly. This keeps it out of the way for
/// real-world installs.
class StubProvider : public PackageProvider {
  public:
    StubProvider();

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

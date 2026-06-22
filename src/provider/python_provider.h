// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "package_provider.h"

namespace den {

/// PyPI package provider (🎯T38). Each installed package lives in its own
/// self-contained virtual environment under
/// `<den_home>/providers/pip/<name>/<version>/`, created with `uv` when
/// available and falling back to the host `python3 -m venv` + `pip`.
///
/// Console-entry scripts land in the venv's `bin/` directory, which
/// `binary_paths()` surfaces to the env materialiser exactly like a Homebrew
/// keg's `bin/`. The venv is fully isolated per environment — installing
/// `black` here never touches `~/.local` or any global site-packages.
///
/// `accepts()` returns false: pip never auto-claims a bare name (PyPI's
/// namespace overlaps heavily with Homebrew formulae), so the provider is
/// selected only via `--provider pip` or a manifest hint.
class PythonProvider : public PackageProvider {
  public:
    PythonProvider();

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

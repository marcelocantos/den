// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "package_provider.h"

namespace den {

/// Cargo (crates.io) package provider (🎯T41). Installs crate binaries with
/// `cargo install --root <root>` into
/// `<den_home>/providers/cargo/<name>/<version>/`, which populates
/// `<root>/bin/`. Rust binaries are statically linked and self-contained, so
/// `binary_paths()` surfaces `bin/` to the env like a Homebrew keg.
///
/// `CARGO_HOME` is redirected into den's cache so the registry index and build
/// artifacts never touch `~/.cargo`, and binaries never land in
/// `~/.cargo/bin`. The crate name may differ from the binary name (crate
/// `ripgrep` ships `rg`); the env picks up whatever binaries cargo installed.
///
/// `accepts()` returns false: cargo is selected only via `--provider cargo`
/// or a manifest hint.
class CargoProvider : public PackageProvider {
  public:
    CargoProvider();

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

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "package_provider.h"

namespace den {

/// Go package provider (🎯T40). Installs command binaries with `go install`
/// into an env-local GOBIN under
/// `<den_home>/providers/go/<name>/<version>/bin/`. Go binaries are
/// statically linked and self-contained, so no further wiring is needed —
/// `binary_paths()` surfaces `bin/` to the env like a Homebrew keg.
///
/// Naming: a name containing `/` (a module path such as
/// `mvdan.cc/gofumpt`) is installed verbatim; a bare name is resolved
/// through a small table of well-known Go tools. `GOPATH`/`GOMODCACHE` are
/// redirected into den's cache so installs never touch `~/go`.
///
/// `accepts()` returns false: Go is selected only via `--provider go` or a
/// manifest hint.
class GoProvider : public PackageProvider {
  public:
    GoProvider();

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

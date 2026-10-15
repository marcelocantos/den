// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../index/package.h"
#include "package_provider.h"

namespace den {

/// Locate the keg directory inside an extracted bottle: <root>/<name>/<version>,
/// or the single <root>/<name>/<version>_<rev> for revisioned/rebuilt bottles.
/// Empty if absent or ambiguous.
fs::path find_bottle_keg_dir(const fs::path& extract_root, const std::string& name,
                             const std::string& version);

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
    fs::path package_root(const Config& config, std::string_view name,
                          std::string_view version) const override;
    std::vector<fs::path> binary_paths(const Config& config,
                                       const InstalledPackage& pkg) const override;

  private:
    const PackageIndex* idx_;
};

/// Download (and hash-verify) the bottle for `pkg` into the archive cache
/// without extracting or pouring it. Returns the cached path, or nothing for
/// casks or packages with no archive for this platform. The trust
/// cross-check still runs at install time, before extraction.
std::optional<fs::path> prefetch_bottle(const Config& config, const Package& pkg);

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "stub_provider.h"

#include "../core/config.h"
#include "../core/error.h"

#include <fstream>
#include <string>

namespace den {

namespace {

constexpr std::string_view kStubProviderName = "stub";
constexpr std::string_view kStubVersion = "0.1.0";

fs::path stub_root(const Config& config) {
    return config.den_home / "providers" / "stub";
}

fs::path stub_package_root(const Config& config, std::string_view name, std::string_view version) {
    return stub_root(config) / std::string(name) / std::string(version);
}

} // namespace

StubProvider::StubProvider() = default;

std::string_view StubProvider::provider_name() const {
    return kStubProviderName;
}

bool StubProvider::accepts(std::string_view /*name*/) const {
    // Never auto-claim — only `--provider stub` selects this provider.
    return false;
}

InstallResult StubProvider::install(const Config& config, std::string_view name,
                                    std::string_view version_hint) {
    if (name.empty()) {
        throw UserError("stub provider: package name must not be empty");
    }

    std::string resolved_version =
        version_hint.empty() ? std::string(kStubVersion) : std::string(version_hint);
    auto root = stub_package_root(config, name, resolved_version);
    auto bin = root / "bin";
    fs::create_directories(bin);

    auto script = bin / std::string(name);
    std::ofstream out(script);
    if (!out) {
        throw InternalError("stub provider: failed to write " + script.string());
    }
    out << "#!/bin/sh\necho 'stub package " << name << " v" << resolved_version << "'\n";
    out.close();

    // Make the script executable. POSIX permissions are fine here — the stub
    // intentionally has no Windows path today.
    std::error_code ec;
    fs::permissions(script,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                        fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace, ec);

    InstallResult result;
    result.resolved_version = std::move(resolved_version);
    return result;
}

void StubProvider::uninstall(const Config& config, std::string_view name) {
    auto pkg_root = stub_root(config) / std::string(name);
    std::error_code ec;
    fs::remove_all(pkg_root, ec);
}

std::vector<InstalledPackage> StubProvider::list_installed(const Config& config) const {
    std::vector<InstalledPackage> result;
    auto root = stub_root(config);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return result;
    }
    for (const auto& name_entry : fs::directory_iterator(root, ec)) {
        if (!name_entry.is_directory(ec))
            continue;
        for (const auto& ver_entry : fs::directory_iterator(name_entry.path(), ec)) {
            if (!ver_entry.is_directory(ec))
                continue;
            result.push_back({name_entry.path().filename().string(),
                              ver_entry.path().filename().string(), ver_entry.path()});
        }
    }
    return result;
}

fs::path StubProvider::package_root(const Config& config, std::string_view name,
                                    std::string_view version) const {
    return stub_package_root(config, name, version);
}

std::vector<fs::path> StubProvider::binary_paths(const Config& /*config*/,
                                                 const InstalledPackage& pkg) const {
    std::vector<fs::path> paths;
    auto bin = pkg.path / "bin";
    if (fs::is_directory(bin)) {
        paths.push_back(bin);
    }
    return paths;
}

} // namespace den

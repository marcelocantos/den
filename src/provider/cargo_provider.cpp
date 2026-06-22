// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "cargo_provider.h"

#include "../core/config.h"
#include "../core/error.h"
#include "exec.h"

#include <cctype>

#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>
#include <string>

namespace den {

namespace {

constexpr std::string_view kProviderName = "cargo";

fs::path provider_root(const Config& config) {
    return config.den_home / "providers" / "cargo";
}

fs::path pkg_versioned_root(const Config& config, std::string_view name, std::string_view version) {
    return provider_root(config) / std::string(name) / std::string(version);
}

// crates.io names: letters, digits, '-' and '_'. Reject anything else.
bool valid_crate_name(std::string_view name) {
    if (name.empty() || name.front() == '-') {
        return false;
    }
    for (char c : name) {
        bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// Parse the installed version from <root>/.crates.toml, whose [v1] section has
// keys like: "ripgrep 14.1.0 (registry+https://...)" = ["rg"].
std::string read_installed_version(const fs::path& root, std::string_view crate) {
    std::ifstream in(root / ".crates.toml");
    if (!in) {
        return {};
    }
    std::string line;
    std::string prefix = "\"" + std::string(crate) + " ";
    while (std::getline(in, line)) {
        if (!line.starts_with(prefix)) {
            continue;
        }
        // After the crate name and a space comes "<version> (registry...".
        auto rest = line.substr(prefix.size());
        std::istringstream iss(rest);
        std::string version;
        iss >> version;
        return version;
    }
    return {};
}

} // namespace

CargoProvider::CargoProvider() = default;

std::string_view CargoProvider::provider_name() const {
    return kProviderName;
}

bool CargoProvider::accepts(std::string_view /*name*/) const {
    return false;
}

InstallResult CargoProvider::install(const Config& config, std::string_view name,
                                     std::string_view version_hint) {
    if (!valid_crate_name(name)) {
        throw UserError("cargo provider: invalid crate name '" + std::string(name) + "'");
    }
    if (!tool_available("cargo")) {
        throw UserError("cargo provider: `cargo` not found on PATH. Install the Rust toolchain "
                        "to install crates, or use a different provider.");
    }

    auto staging = provider_root(config) / std::string(name) / ".staging";
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging);

    // Redirect CARGO_HOME into den's cache so the registry index/artifacts and
    // installed binaries never touch the user's ~/.cargo.
    auto cargo_home = config.cache / "cargo";
    fs::create_directories(cargo_home, ec);

    std::map<std::string, std::string> env = {{"CARGO_HOME", cargo_home.string()}};

    std::vector<std::string> argv = {"cargo", "install", "--root", staging.string(), "--locked"};
    if (!version_hint.empty()) {
        argv.push_back("--version");
        argv.push_back(std::string(version_hint));
    }
    argv.push_back(std::string(name));

    auto inst = run_tool(argv, env);
    if (inst.exit_code != 0) {
        // --locked can fail for crates without a checked-in Cargo.lock; retry
        // once without it before giving up.
        SPDLOG_DEBUG("cargo provider: --locked install failed, retrying unlocked");
        std::vector<std::string> retry = {"cargo", "install", "--root", staging.string()};
        if (!version_hint.empty()) {
            retry.push_back("--version");
            retry.push_back(std::string(version_hint));
        }
        retry.push_back(std::string(name));
        inst = run_tool(retry, env);
        if (inst.exit_code != 0) {
            fs::remove_all(staging, ec);
            throw UserError("cargo provider: failed to install '" + std::string(name) + "':\n" +
                            inst.output);
        }
    }

    std::string resolved = read_installed_version(staging, name);
    if (resolved.empty()) {
        fs::remove_all(staging, ec);
        throw UserError("cargo provider: installed '" + std::string(name) +
                        "' but could not determine its version");
    }

    auto dest = pkg_versioned_root(config, name, resolved);
    fs::remove_all(dest, ec);
    fs::create_directories(dest.parent_path());
    fs::rename(staging, dest, ec);
    if (ec) {
        fs::remove_all(staging, ec);
        throw InternalError("cargo provider: failed to move install into place: " + ec.message());
    }

    InstallResult result;
    result.resolved_version = resolved;
    return result;
}

void CargoProvider::uninstall(const Config& config, std::string_view name) {
    auto pkg_root = provider_root(config) / std::string(name);
    std::error_code ec;
    fs::remove_all(pkg_root, ec);
}

std::vector<InstalledPackage> CargoProvider::list_installed(const Config& config) const {
    std::vector<InstalledPackage> result;
    auto root = provider_root(config);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return result;
    }
    for (const auto& name_entry : fs::directory_iterator(root, ec)) {
        if (!name_entry.is_directory(ec)) {
            continue;
        }
        for (const auto& ver_entry : fs::directory_iterator(name_entry.path(), ec)) {
            if (!ver_entry.is_directory(ec)) {
                continue;
            }
            auto version = ver_entry.path().filename().string();
            if (version.starts_with('.')) {
                continue;
            }
            result.push_back({name_entry.path().filename().string(), version, ver_entry.path()});
        }
    }
    return result;
}

fs::path CargoProvider::package_root(const Config& config, std::string_view name,
                                     std::string_view version) const {
    return pkg_versioned_root(config, name, version);
}

std::vector<fs::path> CargoProvider::binary_paths(const Config& /*config*/,
                                                  const InstalledPackage& pkg) const {
    std::vector<fs::path> paths;
    auto bin = pkg.path / "bin";
    if (fs::is_directory(bin)) {
        paths.push_back(bin);
    }
    return paths;
}

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "python_provider.h"

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

constexpr std::string_view kProviderName = "pip";

fs::path provider_root(const Config& config) {
    return config.den_home / "providers" / "pip";
}

fs::path pkg_versioned_root(const Config& config, std::string_view name, std::string_view version) {
    return provider_root(config) / std::string(name) / std::string(version);
}

// PyPI distribution names: letters, digits, and any of -_. (PEP 503 normalises
// runs of those, but we accept the unnormalised form a user would type). A
// leading separator or "." traversal component is rejected outright.
bool valid_pypi_name(std::string_view name) {
    if (name.empty() || name.front() == '-' || name.front() == '.') {
        return false;
    }
    if (name.find("..") != std::string_view::npos) {
        return false;
    }
    for (char c : name) {
        bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// Extract the value of "Version:" from `uv pip show` / `pip show` output.
std::string parse_show_version(const std::string& show_output) {
    constexpr std::string_view kKey = "Version:";
    auto pos = show_output.find(kKey);
    if (pos == std::string::npos) {
        return {};
    }
    pos += kKey.size();
    auto end = show_output.find('\n', pos);
    std::string value = show_output.substr(pos, end == std::string::npos ? end : end - pos);
    // Trim surrounding whitespace.
    auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return {};
    }
    auto last = value.find_last_not_of(" \t\r");
    return value.substr(first, last - first + 1);
}

// Rewrite shebang lines in `bin_dir`'s scripts that point at `old_prefix`,
// repointing them at `new_prefix`. venv console scripts hardcode the absolute
// interpreter path, so a venv moved on disk would otherwise have dead
// interpreters. (uv's --relocatable venvs use a /bin/sh shim and are
// unaffected; this is the safety net for the plain `python -m venv` path.)
void relocate_shebangs(const fs::path& bin_dir, const std::string& old_prefix,
                       const std::string& new_prefix) {
    std::error_code ec;
    if (!fs::is_directory(bin_dir, ec)) {
        return;
    }
    for (const auto& entry : fs::directory_iterator(bin_dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in) {
            continue;
        }
        std::string first;
        std::getline(in, first);
        if (first.size() < 2 || first[0] != '#' || first[1] != '!' ||
            first.find(old_prefix) == std::string::npos) {
            continue;
        }
        std::stringstream rest;
        rest << in.rdbuf();
        in.close();

        std::string::size_type pos = 0;
        while ((pos = first.find(old_prefix, pos)) != std::string::npos) {
            first.replace(pos, old_prefix.size(), new_prefix);
            pos += new_prefix.size();
        }

        auto perms = fs::status(entry.path(), ec).permissions();
        std::ofstream out(entry.path(), std::ios::binary | std::ios::trunc);
        if (!out) {
            SPDLOG_WARN("pip provider: cannot rewrite shebang in {}", entry.path().string());
            continue;
        }
        out << first << '\n' << rest.str();
        out.close();
        fs::permissions(entry.path(), perms, fs::perm_options::replace, ec);
    }
}

// Run a venv creation + install + version query using uv when present,
// otherwise the host python3 + pip. Returns the resolved version on success;
// throws UserError when no Python toolchain is available or a step fails.
std::string create_venv_and_install(const fs::path& venv, std::string_view name,
                                    std::string_view version_hint) {
    std::string spec(name);
    if (!version_hint.empty()) {
        spec += "==";
        spec += version_hint;
    }
    auto python = (venv / "bin" / "python").string();

    if (tool_available("uv")) {
        // --relocatable makes the venv's console scripts resolve their
        // interpreter relative to their own location, so the venv survives the
        // staging→versioned-path rename below.
        auto venv_r = run_tool({"uv", "venv", "--relocatable", venv.string()});
        if (!venv_r.spawned || venv_r.exit_code != 0) {
            throw UserError("pip provider: `uv venv` failed:\n" + venv_r.output);
        }
        auto inst = run_tool({"uv", "pip", "install", "--python", python, spec});
        if (inst.exit_code != 0) {
            throw UserError("pip provider: failed to install '" + std::string(name) + "':\n" +
                            inst.output);
        }
        auto show = run_tool({"uv", "pip", "show", "--python", python, std::string(name)});
        return parse_show_version(show.output);
    }

    if (tool_available("python3")) {
        auto venv_r = run_tool({"python3", "-m", "venv", venv.string()});
        if (!venv_r.spawned || venv_r.exit_code != 0) {
            throw UserError("pip provider: `python3 -m venv` failed:\n" + venv_r.output);
        }
        auto inst = run_tool({python, "-m", "pip", "install", "--disable-pip-version-check", spec});
        if (inst.exit_code != 0) {
            throw UserError("pip provider: failed to install '" + std::string(name) + "':\n" +
                            inst.output);
        }
        auto show = run_tool({python, "-m", "pip", "show", std::string(name)});
        return parse_show_version(show.output);
    }

    throw UserError("pip provider: no Python toolchain found. Install `uv` or `python3` "
                    "to install PyPI packages, or use a different provider.");
}

} // namespace

PythonProvider::PythonProvider() = default;

std::string_view PythonProvider::provider_name() const {
    return kProviderName;
}

bool PythonProvider::accepts(std::string_view /*name*/) const {
    // Never auto-claim — only `--provider pip` or a manifest hint selects pip.
    return false;
}

InstallResult PythonProvider::install(const Config& config, std::string_view name,
                                      std::string_view version_hint) {
    if (!valid_pypi_name(name)) {
        throw UserError("pip provider: invalid package name '" + std::string(name) + "'");
    }

    // Install into a staging venv first — the concrete version is unknown until
    // pip resolves it, and the final path is version-scoped.
    auto staging = provider_root(config) / std::string(name) / ".staging";
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging.parent_path());

    std::string resolved = create_venv_and_install(staging, name, version_hint);
    if (resolved.empty()) {
        fs::remove_all(staging, ec);
        throw UserError("pip provider: installed '" + std::string(name) +
                        "' but could not determine its version");
    }

    auto dest = pkg_versioned_root(config, name, resolved);
    fs::remove_all(dest, ec);
    fs::create_directories(dest.parent_path());
    fs::rename(staging, dest, ec);
    if (ec) {
        fs::remove_all(staging, ec);
        throw InternalError("pip provider: failed to move venv into place: " + ec.message());
    }

    // Repoint any console-script shebangs that still reference the staging path.
    relocate_shebangs(dest / "bin", staging.string(), dest.string());

    InstallResult result;
    result.resolved_version = resolved;
    return result;
}

void PythonProvider::uninstall(const Config& config, std::string_view name) {
    // pip packages are self-contained venvs — reclaim immediately.
    auto pkg_root = provider_root(config) / std::string(name);
    std::error_code ec;
    fs::remove_all(pkg_root, ec);
}

std::vector<InstalledPackage> PythonProvider::list_installed(const Config& config) const {
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
                continue; // skip .staging and other dotfiles
            }
            result.push_back({name_entry.path().filename().string(), version, ver_entry.path()});
        }
    }
    return result;
}

fs::path PythonProvider::package_root(const Config& config, std::string_view name,
                                      std::string_view version) const {
    return pkg_versioned_root(config, name, version);
}

std::vector<fs::path> PythonProvider::binary_paths(const Config& /*config*/,
                                                   const InstalledPackage& pkg) const {
    std::vector<fs::path> paths;
    auto bin = pkg.path / "bin";
    if (fs::is_directory(bin)) {
        paths.push_back(bin);
    }
    return paths;
}

} // namespace den

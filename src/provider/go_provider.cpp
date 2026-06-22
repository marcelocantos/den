// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "go_provider.h"

#include "../core/config.h"
#include "../core/error.h"
#include "exec.h"

#include <cctype>

#include <spdlog/spdlog.h>

#include <array>
#include <string>

namespace den {

namespace {

constexpr std::string_view kProviderName = "go";

// Curated bare-name → module-path map for popular Go tools. Users may always
// pass a full module path instead; this table just spares them the typing for
// the common cases.
struct GoAlias {
    std::string_view name;
    std::string_view module_path;
};
constexpr std::array<GoAlias, 6> kAliases = {{
    {"gofumpt", "mvdan.cc/gofumpt"},
    {"golangci-lint", "github.com/golangci/golangci-lint/v2/cmd/golangci-lint"},
    {"gopls", "golang.org/x/tools/gopls"},
    {"staticcheck", "honnef.co/go/tools/cmd/staticcheck"},
    {"stringer", "golang.org/x/tools/cmd/stringer"},
    {"goimports", "golang.org/x/tools/cmd/goimports"},
}};

fs::path provider_root(const Config& config) {
    return config.den_home / "providers" / "go";
}

fs::path pkg_versioned_root(const Config& config, std::string_view name, std::string_view version) {
    return provider_root(config) / std::string(name) / std::string(version);
}

bool valid_name(std::string_view name) {
    if (name.empty() || name.front() == '-') {
        return false;
    }
    if (name.find("..") != std::string_view::npos) {
        return false;
    }
    // Module paths use a restricted set; '@' is not part of the path (we add
    // the version suffix ourselves).
    for (char c : name) {
        bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' ||
                  c == '/';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// Map the user-typed name to the module path go install understands.
std::string resolve_module_path(std::string_view name) {
    if (name.find('/') != std::string_view::npos) {
        return std::string(name); // already a module path
    }
    for (const auto& a : kAliases) {
        if (a.name == name) {
            return std::string(a.module_path);
        }
    }
    // Bare, unknown name — go install almost certainly won't resolve it, but
    // let it try and surface go's own error rather than guessing.
    return std::string(name);
}

// Parse the module version from `go version -m <binary>` output, which lists a
// line of the form "\tmod\t<path>\t<version>\t<hash>".
std::string parse_module_version(const std::string& out) {
    std::string::size_type pos = 0;
    while (pos < out.size()) {
        auto eol = out.find('\n', pos);
        std::string line = out.substr(pos, eol == std::string::npos ? eol : eol - pos);
        pos = (eol == std::string::npos) ? out.size() : eol + 1;

        // Look for a "mod" field (tab-delimited, leading tab).
        auto mod = line.find("\tmod\t");
        if (mod == std::string::npos) {
            continue;
        }
        // Fields after "\tmod\t": <path>\t<version>\t...
        auto rest = line.substr(mod + 5);
        auto tab1 = rest.find('\t');
        if (tab1 == std::string::npos) {
            continue;
        }
        auto after_path = rest.substr(tab1 + 1);
        auto tab2 = after_path.find('\t');
        return after_path.substr(0, tab2);
    }
    return {};
}

} // namespace

GoProvider::GoProvider() = default;

std::string_view GoProvider::provider_name() const {
    return kProviderName;
}

bool GoProvider::accepts(std::string_view /*name*/) const {
    return false;
}

InstallResult GoProvider::install(const Config& config, std::string_view name,
                                  std::string_view version_hint) {
    if (!valid_name(name)) {
        throw UserError("go provider: invalid package name '" + std::string(name) + "'");
    }
    if (!tool_available("go")) {
        throw UserError("go provider: `go` not found on PATH. Install the Go toolchain to "
                        "install Go packages, or use a different provider.");
    }

    auto module_path = resolve_module_path(name);
    std::string version = version_hint.empty() ? "latest" : std::string(version_hint);
    std::string target = module_path + "@" + version;

    // Binary name = last path component of the module (sans any /vN suffix is
    // handled by go itself; we use the user's bare name as the manifest key).
    auto slash = module_path.find_last_of('/');
    std::string binary = slash == std::string::npos ? module_path : module_path.substr(slash + 1);

    auto staging = provider_root(config) / std::string(name) / ".staging";
    auto staging_bin = staging / "bin";
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging_bin);

    // Redirect Go's caches into den's cache so installs never touch ~/go.
    auto gopath = config.cache / "go" / "gopath";
    auto gomodcache = config.cache / "go" / "modcache";
    fs::create_directories(gopath, ec);
    fs::create_directories(gomodcache, ec);

    std::map<std::string, std::string> env = {
        {"GOBIN", staging_bin.string()},
        {"GOPATH", gopath.string()},
        {"GOMODCACHE", gomodcache.string()},
        {"GOFLAGS", "-mod=mod"},
    };

    auto inst = run_tool({"go", "install", target}, env);
    if (inst.exit_code != 0) {
        fs::remove_all(staging, ec);
        throw UserError("go provider: `go install " + target + "` failed:\n" + inst.output);
    }

    auto installed_bin = staging_bin / binary;
    if (!fs::exists(installed_bin)) {
        // go installed something under a different binary name — pick the first
        // executable it produced so the manifest/run can still find it.
        for (const auto& e : fs::directory_iterator(staging_bin, ec)) {
            if (e.is_regular_file(ec)) {
                installed_bin = e.path();
                break;
            }
        }
    }

    std::string resolved;
    if (fs::exists(installed_bin)) {
        auto ver = run_tool({"go", "version", "-m", installed_bin.string()}, env);
        resolved = parse_module_version(ver.output);
    }
    if (resolved.empty()) {
        resolved = version; // fall back to the requested ref ("latest" etc.)
    }

    auto dest = pkg_versioned_root(config, name, resolved);
    fs::remove_all(dest, ec);
    fs::create_directories(dest.parent_path());
    fs::rename(staging, dest, ec);
    if (ec) {
        fs::remove_all(staging, ec);
        throw InternalError("go provider: failed to move install into place: " + ec.message());
    }

    InstallResult result;
    result.resolved_version = resolved;
    return result;
}

void GoProvider::uninstall(const Config& config, std::string_view name) {
    auto pkg_root = provider_root(config) / std::string(name);
    std::error_code ec;
    fs::remove_all(pkg_root, ec);
}

std::vector<InstalledPackage> GoProvider::list_installed(const Config& config) const {
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

fs::path GoProvider::package_root(const Config& config, std::string_view name,
                                  std::string_view version) const {
    return pkg_versioned_root(config, name, version);
}

std::vector<fs::path> GoProvider::binary_paths(const Config& /*config*/,
                                               const InstalledPackage& pkg) const {
    std::vector<fs::path> paths;
    auto bin = pkg.path / "bin";
    if (fs::is_directory(bin)) {
        paths.push_back(bin);
    }
    return paths;
}

} // namespace den

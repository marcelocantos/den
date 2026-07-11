// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "manifest.h"

#include "../core/error.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace den {

using json = nlohmann::json;

namespace {

/// Percent-encode a character.
std::string percent_encode(char c) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result = "%";
    result += hex[static_cast<unsigned char>(c) >> 4];
    result += hex[static_cast<unsigned char>(c) & 0x0F];
    return result;
}

/// Percent-decode a two-character hex sequence.
char percent_decode(char hi, char lo) {
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return -1;
    };
    int h = hex_val(hi);
    int l = hex_val(lo);
    if (h < 0 || l < 0) {
        throw UserError(std::string("invalid percent encoding: %") + hi + lo);
    }
    return static_cast<char>((h << 4) | l);
}

fs::path manifest_file(const fs::path& den_home, const std::string& env_path) {
    return den_home / "manifests" / env_slug(env_path) / "manifest.json";
}

} // namespace

std::string env_slug(const std::string& env_path) {
    if (env_path == "/") {
        return "ROOT";
    }

    // Strip leading slash.
    std::string path = env_path;
    if (!path.empty() && path[0] == '/') {
        path = path.substr(1);
    }

    // Percent-encode `/`, `-`, `%`, and `.` so no slug can act as a filesystem
    // navigation component. Leaving `.` unencoded made env_slug("..") == ".."
    // and `den env remove ..` resolve to den_home itself (Fable F3).
    std::string result;
    for (size_t i = 0; i < path.size(); ++i) {
        char c = path[i];
        if (c == '/') {
            result += "%2F";
        } else if (c == '-') {
            result += "%2D";
        } else if (c == '.') {
            result += "%2E";
        } else if (c == '%') {
            result += "%25";
        } else {
            result += c;
        }
    }

    return result;
}

std::string slug_to_path(const std::string& slug) {
    if (slug == "ROOT") {
        return "/";
    }

    std::string result = "/";
    for (size_t i = 0; i < slug.size(); ++i) {
        if (slug[i] == '%' && i + 2 < slug.size()) {
            char decoded = percent_decode(slug[i + 1], slug[i + 2]);
            if (decoded == '/') {
                result += '/';
            } else {
                result += decoded;
            }
            i += 2;
        } else {
            result += slug[i];
        }
    }

    return result;
}

Manifest read_manifest(const fs::path& den_home, const std::string& env_path) {
    auto path = manifest_file(den_home, env_path);
    Manifest m;

    std::ifstream in(path);
    if (!in) {
        return m; // Empty manifest if file doesn't exist.
    }

    try {
        json j = json::parse(in);

        if (j.contains("packages") && j["packages"].is_object()) {
            for (auto& [k, v] : j["packages"].items()) {
                if (v.is_object()) {
                    // New nested shape: k is the provider name, v is name->version.
                    for (auto& [name, ver] : v.items()) {
                        m.packages[k][name] = ver.get<std::string>();
                    }
                } else if (v.is_string()) {
                    // Legacy flat shape: k is the package name, v is the version.
                    // Promote under "homebrew" for backward compatibility.
                    m.packages["homebrew"][k] = v.get<std::string>();
                }
            }
        }

        if (j.contains("auto_deps")) {
            const auto& ad = j["auto_deps"];
            if (ad.is_object()) {
                for (auto& [provider, deps] : ad.items()) {
                    if (deps.is_array()) {
                        for (auto& v : deps) {
                            m.auto_deps[provider].insert(v.get<std::string>());
                        }
                    }
                }
            } else if (ad.is_array()) {
                // Legacy flat shape: all entries are homebrew auto-deps.
                for (auto& v : ad) {
                    m.auto_deps["homebrew"].insert(v.get<std::string>());
                }
            }
        }

        if (j.contains("origin") && j["origin"].is_string()) {
            m.origin = j["origin"].get<std::string>();
        }
    } catch (const json::exception& e) {
        // Fail closed: a present-but-corrupt file must not look like an empty
        // manifest. Callers (with_manifest read-modify-write) would otherwise
        // clobber every package on the next mutation (Fable F5).
        throw UserError("corrupt manifest " + path.string() + ": " + e.what() +
                        " — refusing to treat as empty (fix or restore the file before "
                        "mutating packages)");
    }

    return m;
}

void write_manifest(const fs::path& den_home, const std::string& env_path, const Manifest& m) {
    auto path = manifest_file(den_home, env_path);
    fs::create_directories(path.parent_path());

    json j;
    j["packages"] = json::object();
    for (const auto& [provider, pkgs] : m.packages) {
        j["packages"][provider] = pkgs;
    }
    j["auto_deps"] = json::object();
    for (const auto& [provider, deps] : m.auto_deps) {
        j["auto_deps"][provider] = deps;
    }
    if (m.origin) {
        j["origin"] = *m.origin;
    }

    // Atomic write: write to temp file, then rename.
    auto tmp_path = path.string() + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out) {
            throw InternalError("failed to open temp manifest: " + tmp_path);
        }
        out << j.dump(2) << '\n';
        out.flush();
        if (!out) {
            throw InternalError("failed to write temp manifest: " + tmp_path);
        }
    }

    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        throw InternalError("failed to rename manifest: " + std::string(std::strerror(errno)));
    }
}

std::map<std::string, std::string> resolve(const fs::path& den_home, const std::string& env_path) {
    std::map<std::string, std::string> result;
    std::set<std::string> visited;

    // Walk the parent chain, collecting packages. Child overrides parent.
    std::string current = env_path;
    std::vector<Manifest> chain;

    while (!current.empty() && visited.find(current) == visited.end()) {
        visited.insert(current);
        auto m = read_manifest(den_home, current);
        chain.push_back(std::move(m));
        current = chain.back().origin.value_or("");
    }

    // Apply in reverse order (root first, then children override).
    // Flatten across providers: returned map is name -> version. If the same
    // package name appears under two providers within a single manifest, the
    // last-iterated provider wins — name collisions across providers are a
    // separate concern handled at install time.
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        for (const auto& [provider, pkgs] : it->packages) {
            for (const auto& [name, version] : pkgs) {
                result[name] = version;
            }
        }
    }

    return result;
}

std::map<std::string, std::map<std::string, std::string>>
resolve_per_provider(const fs::path& den_home, const std::string& env_path) {
    std::map<std::string, std::map<std::string, std::string>> result;
    std::set<std::string> visited;

    // Walk the parent chain, collecting per-provider entries.
    std::string current = env_path;
    std::vector<Manifest> chain;
    while (!current.empty() && visited.find(current) == visited.end()) {
        visited.insert(current);
        auto m = read_manifest(den_home, current);
        chain.push_back(std::move(m));
        current = chain.back().origin.value_or("");
    }

    // Apply in reverse order (root first, then children override).
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        for (const auto& [provider, pkgs] : it->packages) {
            for (const auto& [name, version] : pkgs) {
                result[provider][name] = version;
            }
        }
    }

    return result;
}

std::vector<std::string> list_all(const fs::path& den_home) {
    std::vector<std::string> result;
    auto manifests_dir = den_home / "manifests";

    if (!fs::exists(manifests_dir)) {
        return result;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(manifests_dir, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        auto manifest_path = entry.path() / "manifest.json";
        if (fs::exists(manifest_path)) {
            result.push_back(slug_to_path(entry.path().filename().string()));
        }
    }

    return result;
}

} // namespace den

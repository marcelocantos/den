// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "uninstall_deps.h"

#include "../env/manifest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <deque>
#include <fstream>
#include <set>
#include <sstream>

namespace den {

namespace fs = std::filesystem;

namespace {

constexpr const char* kHomebrew = "homebrew";

// "user/tap/name" and "name" refer to the same package for matching.
std::string short_name(const std::string& n) {
    auto pos = n.rfind('/');
    return pos == std::string::npos ? n : n.substr(pos + 1);
}

bool same_package(const std::string& a, const std::string& b) {
    return a == b || short_name(a) == short_name(b);
}

// Runtime deps recorded in the keg's receipt (already transitive in
// Homebrew's receipts). Used only when the index has no entry.
std::vector<std::string> receipt_runtime_deps(const Config& config, const std::string& name,
                                              const std::string& version) {
    std::vector<std::string> deps;
    std::ifstream f(config.store / name / version / "INSTALL_RECEIPT.json");
    if (!f) {
        return deps;
    }
    try {
        const auto j = nlohmann::json::parse(f);
        if (j.contains("runtime_dependencies") && j["runtime_dependencies"].is_array()) {
            for (const auto& d : j["runtime_dependencies"]) {
                for (const char* key : {"full_name", "name"}) {
                    if (d.contains(key) && d[key].is_string()) {
                        deps.push_back(d[key].get<std::string>());
                        break;
                    }
                }
            }
        }
    } catch (const nlohmann::json::exception&) {
    }
    return deps;
}

// Transitive runtime closure of `name` (excluding itself). Build-only
// dependencies are neither counted nor followed.
std::set<std::string> runtime_closure(const Config& config, const PackageIndex& idx,
                                      const std::string& name, const std::string& version) {
    std::set<std::string> seen;
    std::deque<std::string> queue;
    auto direct = [&](const std::string& n, const std::string* ver) {
        if (const auto* pkg = idx.find(n)) {
            return pkg->dependencies;
        }
        return ver ? receipt_runtime_deps(config, n, *ver) : std::vector<std::string>{};
    };
    for (const auto& d : direct(name, &version)) {
        if (seen.insert(d).second) {
            queue.push_back(d);
        }
    }
    while (!queue.empty()) {
        auto n = queue.front();
        queue.pop_front();
        for (const auto& d : direct(n, nullptr)) {
            if (seen.insert(d).second) {
                queue.push_back(d);
            }
        }
    }
    seen.erase(name);
    return seen;
}

bool lists(const Manifest& m, const std::string& name) {
    auto it = m.packages.find(kHomebrew);
    if (it == m.packages.end()) {
        return false;
    }
    return std::any_of(it->second.begin(), it->second.end(),
                       [&](const auto& kv) { return same_package(kv.first, name); });
}

} // namespace

std::map<std::string, std::vector<RuntimeDependent>>
find_runtime_dependents(const Config& config, const PackageIndex& idx,
                        const std::string& active_env, const std::vector<std::string>& names) {
    std::map<std::string, std::vector<RuntimeDependent>> out;
    if (names.empty()) {
        return out;
    }
    std::map<std::string, Manifest> manifests;
    auto manifest_of = [&](const std::string& env) -> const Manifest& {
        auto it = manifests.find(env);
        if (it == manifests.end()) {
            it = manifests.emplace(env, read_manifest(config.den_home, env)).first;
        }
        return it->second;
    };

    auto envs = list_all(config.den_home);
    if (std::find(envs.begin(), envs.end(), active_env) == envs.end()) {
        envs.push_back(active_env);
    }

    for (const auto& env : envs) {
        const auto& m = manifest_of(env);
        auto bit = m.packages.find(kHomebrew);
        if (bit == m.packages.end()) {
            continue;
        }
        for (const auto& [pkg, version] : bit->second) {
            const bool removed_with_target =
                env == active_env && std::any_of(names.begin(), names.end(), [&](const auto& n) {
                    return same_package(n, pkg);
                });
            if (removed_with_target) {
                continue;
            }
            const auto closure = runtime_closure(config, idx, pkg, version);
            for (const auto& target : names) {
                if (same_package(target, pkg)) {
                    continue;
                }
                const bool needs = std::any_of(closure.begin(), closure.end(), [&](const auto& d) {
                    return same_package(d, target);
                });
                if (!needs) {
                    continue;
                }
                // Still satisfied after the uninstall if this env (or an
                // ancestor other than the one being edited) lists it.
                bool still_provided = false;
                if (env != active_env) {
                    for (const auto& anc : ancestor_chain(config.den_home, env)) {
                        if (anc != active_env && lists(manifest_of(anc), target)) {
                            still_provided = true;
                            break;
                        }
                    }
                }
                if (!still_provided) {
                    out[target].push_back({env, pkg});
                }
            }
        }
    }
    for (auto& [_, deps] : out) {
        std::sort(deps.begin(), deps.end());
        deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    }
    return out;
}

std::string
format_dependents_refusal(const std::map<std::string, std::vector<RuntimeDependent>>& dependents) {
    std::ostringstream os;
    std::vector<std::string> targets;
    for (const auto& [target, deps] : dependents) {
        targets.push_back(target);
        os << "Error: Refusing to uninstall " << target << "\nbecause it is required by ";
        for (size_t i = 0; i < deps.size(); ++i) {
            if (i) {
                os << ", ";
            }
            os << deps[i].name << " (environment " << deps[i].env << ")";
        }
        os << (deps.size() == 1 ? ", which is" : ", which are") << " currently installed.\n";
    }
    os << "You can override this and force removal with:\n  den uninstall --ignore-dependencies";
    for (const auto& t : targets) {
        os << " " << t;
    }
    os << "\n";
    return os.str();
}

} // namespace den

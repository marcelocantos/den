// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "uninstall_deps.h"

#include "../env/manifest.h"
#include "../store/store.h"
#include "../tap/tap.h"

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

size_t slashes(const std::string& n) {
    return static_cast<size_t>(std::count(n.begin(), n.end(), '/'));
}

// Tap formulae carry an absolute path to their .rb; core ones a relative one.
const std::string* tap_source(const Package& pkg) {
    if (pkg.ruby_source_path && fs::path(*pkg.ruby_source_path).is_absolute()) {
        return &*pkg.ruby_source_path;
    }
    return nullptr;
}

// Runtime deps recorded in the keg's receipt (already transitive in
// Homebrew's receipts). Used only when the index has no entry.
std::vector<std::string> receipt_runtime_deps(const Config& config, const std::string& name,
                                              const std::string& version) {
    std::vector<std::string> deps;
    std::ifstream f(config.store / keg_name(name) / version / "INSTALL_RECEIPT.json");
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

bool lists(const PackageIdentity& ids, const Manifest& m, const std::string& name) {
    auto it = m.packages.find(kHomebrew);
    if (it == m.packages.end()) {
        return false;
    }
    return std::any_of(it->second.begin(), it->second.end(),
                       [&](const auto& kv) { return ids.same(kv.first, name); });
}

} // namespace

PackageIdentity::PackageIdentity(const PackageIndex& idx) : idx_(idx) {
    for (const auto& [key, pkg] : idx.packages) {
        if (const auto* src = tap_source(pkg); src && slashes(key) == 2) {
            tap_by_source_.emplace(*src, key);
        }
    }
}

std::string PackageIdentity::id(const std::string& name) const {
    auto c = canonical_formula_ref(name);
    if (slashes(c) == 2) {
        return c;
    }
    if (const auto* pkg = idx_.find(c); pkg && tap_source(*pkg)) {
        if (auto it = tap_by_source_.find(*tap_source(*pkg)); it != tap_by_source_.end()) {
            return it->second;
        }
    }
    return c;
}

bool PackageIdentity::same(const std::string& a, const std::string& b) const {
    if (a == b) {
        return true;
    }
    const auto ia = id(a);
    const auto ib = id(b);
    if (ia == ib) {
        return true;
    }
    // A bare name the index knows nothing about (e.g. a tap that has since
    // been removed) can't be told apart from a tap formula of the same keg
    // name, so match it on the keg name rather than miss a dependent.
    auto unknown_bare = [&](const std::string& i) { return slashes(i) == 0 && !idx_.find(i); };
    if ((unknown_bare(ia) && slashes(ib) == 2) || (unknown_bare(ib) && slashes(ia) == 2)) {
        return keg_name(ia) == keg_name(ib);
    }
    return false;
}

std::string resolve_uninstall_name(const PackageIndex& idx, const Manifest& manifest,
                                   const std::string& requested) {
    for (const auto& [_, bucket] : manifest.packages) {
        if (bucket.count(requested)) {
            return requested;
        }
    }
    auto it = manifest.packages.find(kHomebrew);
    if (it == manifest.packages.end()) {
        return requested;
    }
    const PackageIdentity ids(idx);
    for (const auto& [key, _] : it->second) {
        if (ids.same(key, requested)) {
            return key;
        }
    }
    if (slashes(requested) == 0) {
        std::string found;
        for (const auto& [key, _] : it->second) {
            if (keg_name(key) == requested) {
                if (!found.empty()) {
                    return requested; // ambiguous
                }
                found = key;
            }
        }
        if (!found.empty()) {
            return found;
        }
    }
    return requested;
}

std::map<std::string, std::vector<RuntimeDependent>>
find_runtime_dependents(const Config& config, const PackageIndex& idx,
                        const std::string& active_env, const std::vector<std::string>& names) {
    std::map<std::string, std::vector<RuntimeDependent>> out;
    if (names.empty()) {
        return out;
    }
    const PackageIdentity ids(idx);
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
        for (const auto& entry : bit->second) {
            // Named copies: C++17 lambdas can't capture structured bindings.
            const std::string& pkg = entry.first;
            const std::string& version = entry.second;
            const bool removed_with_target =
                env == active_env && std::any_of(names.begin(), names.end(),
                                                 [&](const auto& n) { return ids.same(n, pkg); });
            if (removed_with_target) {
                continue;
            }
            const auto closure = runtime_closure(config, idx, pkg, version);
            for (const auto& target : names) {
                if (ids.same(target, pkg)) {
                    continue;
                }
                const bool needs = std::any_of(closure.begin(), closure.end(),
                                               [&](const auto& d) { return ids.same(d, target); });
                if (!needs) {
                    continue;
                }
                // Still satisfied after the uninstall if this env (or an
                // ancestor other than the one being edited) lists it.
                bool still_provided = false;
                if (env != active_env) {
                    for (const auto& anc : ancestor_chain(config.den_home, env)) {
                        if (anc != active_env && lists(ids, manifest_of(anc), target)) {
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

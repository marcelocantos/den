// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "registry.h"

#include "../core/error.h"
#include "../env/manifest.h"
#include "homebrew_provider.h"

#include <utility>

namespace den {

void ProviderRegistry::add(std::shared_ptr<PackageProvider> provider) {
    if (!provider) {
        return;
    }
    std::string name(provider->provider_name());
    auto* raw = provider.get();
    by_name_[name] = std::move(provider);
    ordered_.push_back(raw);
    if (default_.empty()) {
        default_ = std::move(name);
    }
}

void ProviderRegistry::set_default(std::string_view name) {
    std::string key(name);
    if (by_name_.find(key) == by_name_.end()) {
        throw InternalError("set_default: provider '" + key + "' is not registered");
    }
    default_ = std::move(key);
}

PackageProvider* ProviderRegistry::find(std::string_view name) const {
    auto it = by_name_.find(std::string(name));
    return it == by_name_.end() ? nullptr : it->second.get();
}

PackageProvider* ProviderRegistry::default_provider() const {
    return default_.empty() ? nullptr : find(default_);
}

std::vector<PackageProvider*> ProviderRegistry::all() const {
    return ordered_;
}

ProviderRegistry make_default_registry(const PackageIndex* idx) {
    ProviderRegistry r;
    r.add(std::make_shared<HomebrewProvider>(idx));
    return r;
}

PackageProvider& resolve_provider(const ProviderRegistry& registry, std::string_view name,
                                  const Manifest& manifest, std::string_view explicit_provider) {
    // 1. Explicit --provider takes priority.
    if (!explicit_provider.empty()) {
        if (auto* p = registry.find(explicit_provider)) {
            return *p;
        }
        throw UserError("unknown provider '" + std::string(explicit_provider) +
                        "'. Pass a registered provider name to --provider.");
    }

    // 2. Manifest hint — the package is already owned by some provider.
    std::string name_str(name);
    for (const auto& [provider_name, pkgs] : manifest.packages) {
        if (pkgs.count(name_str)) {
            if (auto* p = registry.find(provider_name)) {
                return *p;
            }
            throw UserError("package '" + name_str + "' is recorded under provider '" +
                            provider_name + "' but no provider with that name is registered.");
        }
    }

    // 3. Name-pattern hint — the first non-default provider that claims this name.
    auto* default_provider = registry.default_provider();
    for (auto* p : registry.all()) {
        if (p == default_provider) {
            continue;
        }
        if (p->accepts(name)) {
            return *p;
        }
    }

    // 4. Default fallback.
    if (default_provider) {
        return *default_provider;
    }
    throw InternalError("no providers registered");
}

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "package_provider.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace den {

struct Manifest;
struct PackageIndex;

/// Holds the set of providers available in this process. Lookup is by
/// short provider name (e.g. "homebrew", "pip"); the registry also
/// remembers which provider is the default fallback.
class ProviderRegistry {
  public:
    /// Register a provider. The first call's provider becomes the
    /// default fallback; override with `set_default`.
    void add(std::shared_ptr<PackageProvider> provider);

    /// Designate `name` as the default fallback. Throws `InternalError`
    /// if no provider with that name is registered.
    void set_default(std::string_view name);

    PackageProvider* find(std::string_view name) const;
    PackageProvider* default_provider() const;

    /// Providers in registration order.
    std::vector<PackageProvider*> all() const;

  private:
    std::unordered_map<std::string, std::shared_ptr<PackageProvider>> by_name_;
    std::vector<PackageProvider*> ordered_;
    std::string default_;
};

/// Build the registry den's CLI uses by default. Today this is just
/// the Homebrew provider (set as default). Adding a new provider is a
/// one-line change here.
ProviderRegistry make_default_registry(const PackageIndex* idx);

/// Resolve which provider should service a `name` operation.
///
/// Resolution order:
///   1. `explicit_provider` (e.g. from `--provider <name>`), if non-empty.
///      The named provider must be registered; otherwise `UserError`.
///   2. Manifest hint — if any provider's bucket in `manifest.packages`
///      already owns `name`, return that provider.
///   3. Name-pattern hint — the first non-default provider whose
///      `accepts(name)` returns true.
///   4. Default fallback (typically Homebrew).
///
/// Throws `UserError` for an unknown explicit provider or a manifest
/// entry under an unregistered provider. Throws `InternalError` only
/// when the registry has no providers at all.
PackageProvider& resolve_provider(const ProviderRegistry& registry, std::string_view name,
                                  const Manifest& manifest,
                                  std::string_view explicit_provider = {});

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "types.h"

#include <filesystem>
#include <optional>

namespace den {

namespace fs = std::filesystem;

// Where den's install prefix (and therefore its Cellar) came from. See
// Config::detect() and README "Environment precedence" for the order.
enum class PrefixSource {
    EnvDenPrefix, // DEN_PREFIX environment variable
    Settings,     // "store.prefix" in <den_home>/config.json (den migrate pins this)
    LegacyShared, // pre-isolation den home whose manifest packages live in brew's Cellar
    DenHome,      // default: <den_home>/prefix
};

const char* to_string(PrefixSource s);

struct Config {
    // Den's own paths.
    fs::path den_home; // ~/.den/
    fs::path prefix;   // den's install prefix; default ~/.den/prefix
    fs::path store;    // den's Cellar: always prefix/Cellar
    fs::path cache;    // ~/.den/cache/
    PrefixSource prefix_source = PrefixSource::DenHome;

    // Homebrew paths. These describe where *Homebrew* lives (from
    // HOMEBREW_PREFIX / HOMEBREW_CELLAR / HOMEBREW_CASKROOM or the platform
    // default). They are read-only inputs for `den migrate` and diagnostics;
    // they never choose where den installs. `brew shellenv` exports them in
    // every shell, so honouring them for den's store would defeat isolation.
    fs::path homebrew_prefix;   // /opt/homebrew
    fs::path homebrew_cellar;   // /opt/homebrew/Cellar
    fs::path homebrew_caskroom; // /opt/homebrew/Caskroom
    fs::path homebrew_taps;     // /opt/homebrew/Library/Taps

    // Cask install destinations (macOS). den stages under den_home/caskroom
    // and never writes Homebrew's Caskroom. appdir defaults to /Applications
    // and is overridable via DEN_APPDIR for tests / alternate layouts.
    fs::path appdir; // /Applications

    // Platform.
    Arch arch;
    std::optional<MacOsVersion> macos_version;

    // Detect configuration from environment. den's prefix is resolved as:
    //   1. DEN_PREFIX                      (explicit env override)
    //   2. store.prefix in config.json     (explicit setting; den migrate pins
    //                                       it to Homebrew's prefix)
    //   3. <den_home>/prefix, if its Cellar already exists
    //   4. Homebrew's prefix, if this den home predates isolation (a manifest
    //      lists a homebrew keg that exists in Homebrew's Cellar and nothing
    //      above applies) — read-only detection, keeps old installs working
    //   5. <den_home>/prefix               (default)
    // HOMEBREW_* never participates in steps 1-3 or 5.
    static Config detect();

    // Point den at an explicit prefix (store = prefix/Cellar).
    void set_prefix(const fs::path& p, PrefixSource source);

    // True when den's store is Homebrew's Cellar (legacy/migrated layout).
    bool shares_homebrew_cellar() const;
};

} // namespace den

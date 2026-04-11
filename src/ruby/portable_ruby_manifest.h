// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Pinned manifest for Homebrew Portable Ruby.
//
// Used on Linux to lazy-download a Ruby interpreter on first source build
// (🎯T47). macOS uses the embedded bundle in `bundle_data.c` instead.
//
// To upgrade: bump `kPortableRubyVersion`, update the SHA-256s by running
//
//     gh api repos/Homebrew/homebrew-portable-ruby/releases/latest \
//       --jq '.assets[] | "\(.name) \(.digest)"'
//
// and adjust the constants below. Intentional upgrades only — no drift.

namespace den {

inline constexpr const char* kPortableRubyVersion = "3.4.5";

inline constexpr const char* kPortableRubyArm64LinuxUrl =
    "https://github.com/Homebrew/homebrew-portable-ruby/releases/download/3.4.5/"
    "portable-ruby-3.4.5.arm64_linux.bottle.tar.gz";
inline constexpr const char* kPortableRubyArm64LinuxSha256 =
    "58ab194fb0513e8d3f5a3b9a8658cb9909438b405308bf898508eed2b83afc7d";

inline constexpr const char* kPortableRubyX8664LinuxUrl =
    "https://github.com/Homebrew/homebrew-portable-ruby/releases/download/3.4.5/"
    "portable-ruby-3.4.5.x86_64_linux.bottle.tar.gz";
inline constexpr const char* kPortableRubyX8664LinuxSha256 =
    "5ea0e3a30feef0743da5a2924c8a71baead1b9f1d571afbd675bc556e59705f5";

} // namespace den

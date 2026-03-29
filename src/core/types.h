// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>
#include <string>

namespace den {

enum class Arch { Arm64, X86_64 };

inline std::string to_string(Arch a) {
    switch (a) {
    case Arch::Arm64:
        return "aarch64";
    case Arch::X86_64:
        return "x86_64";
    }
    return "unknown";
}

struct MacOsVersion {
    int major;
    int minor;

    std::string to_string() const { return std::to_string(major) + "." + std::to_string(minor); }
};

// Artifact type replaces the formula/cask distinction.
// Both are just packages — this controls install behaviour.
enum class ArtifactType {
    Binary, // CLI tool/library — extracted into store, symlinked into env
    App,    // macOS .app bundle — placed in /Applications (or den-managed)
};

// How relocatable a pre-built archive is.
enum class RelocationType {
    Portable,          // :any_skip_relocation — works anywhere
    NeedsSubstitution, // :any — text placeholders to rewrite
    HardcodedPrefix,   // compiled-in prefix path
};

} // namespace den

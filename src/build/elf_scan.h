// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Read-only ELF dynamic-section scan: the libraries a binary needs and the
// directories it searches for them. Parses the headers directly (64-bit
// little-endian, which is every Linux bottle den pours), so it is cheap
// enough to run over a keg before an upgrade switches and works on any host.

#include <filesystem>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

struct ElfDynamic {
    bool is_elf = false;
    std::vector<std::string> needed; ///< DT_NEEDED, in order
    /// DT_RPATH and DT_RUNPATH entries, split on ':', in order. `$ORIGIN` is
    /// left as written.
    std::vector<std::string> search_paths;
};

/// Parse `file`'s dynamic section. `is_elf` is false when it is not a
/// readable 64-bit little-endian ELF file; a static binary is ELF with
/// nothing needed.
ElfDynamic elf_dynamic(const fs::path& file);

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

/// Relocation: replace path prefixes in text and binary files.
/// Used for:
/// 1. `:any` bottle relocation (@@HOMEBREW_PREFIX@@ → den's paths)
/// 2. Ruby binary relocation (hardcoded path → den's paths)

/// Expand @@HOMEBREW_PREFIX@@ / @@HOMEBREW_CELLAR@@ / @@HOMEBREW_REPOSITORY@@
/// / @@HOMEBREW_LIBRARY@@ tokens in a path string.
std::string expand_homebrew_placeholders(std::string path, const std::string& prefix,
                                         const std::string& cellar);

/// Order Homebrew version strings segment-wise ("1.9.10" > "1.9.7",
/// "1.9.7_2" > "1.9.7"). Returns <0, 0, >0.
int compare_keg_versions(const std::string& a, const std::string& b);

/// Pick the keg directory den should bind a dependency to: the highest
/// version under <cellar>/<dep>/ that looks like a real keg (has content
/// other than a single nested directory). Empty if none.
fs::path select_dep_keg(const fs::path& cellar, const std::string& dep);

/// Rewrite a Mach-O install name / rpath that points through Homebrew's
/// shared opt link (`@@HOMEBREW_PREFIX@@/opt/<dep>/...`) to the exact keg
/// den binds instead (`<cellar>/<dep>/<version>/...`). The package's own name
/// maps to `self_dir`. Den never depends on Homebrew's opt links (which
/// Homebrew may repoint to an ABI-incompatible version) for Mach-O load
/// commands; it maintains them only for text references such as shebangs, and
/// never repoints one Homebrew owns (ensure_prefix_opt_link). Returns the input unchanged when no
/// keg can be found.
std::string pin_opt_reference(const std::string& name, const fs::path& cellar,
                              const std::string& self_name, const fs::path& self_dir);

/// Resolve a Mach-O tool (otool, install_name_tool). Uses the PATH shim when
/// it works; otherwise falls back to /Library/Developer/CommandLineTools,
/// because a stale Xcode.app selection makes the /usr/bin shims fail.
std::string macho_tool(const std::string& tool);

/// Path Homebrew substitutes for @@HOMEBREW_PERL@@ when pouring `name`
/// with direct dependencies `deps` (Keg#prepare_relocation_to_locations).
/// Brewed perl (a declared `perl` dependency, or perl itself) maps to
/// <prefix>/opt/perl/bin/perl. On macOS anything else gets the system perl
/// (/usr/bin/perl5.34 on Sonoma+, /usr/bin/perl5.30 earlier, /usr/bin/perl
/// as a last resort); on Linux brew always uses <prefix>/opt/perl/bin/perl.
std::string homebrew_perl_path(const std::string& prefix, const std::string& name,
                               const std::vector<std::string>& deps);

/// Path Homebrew substitutes for @@HOMEBREW_JAVA@@: the openjdk(@N)
/// dependency's libexec (macOS: libexec/openjdk.jdk/Contents/Home) under
/// <prefix>/opt. Brew leaves the token unexpanded when no openjdk dep exists;
/// den falls back to plain `openjdk` so the keg still points at a JDK.
std::string homebrew_java_path(const std::string& prefix, const std::vector<std::string>& deps);

/// Replace @@HOMEBREW_PREFIX@@, @@HOMEBREW_CELLAR@@, and similar
/// placeholders in all text files under a directory tree. @@HOMEBREW_JAVA@@
/// and @@HOMEBREW_PERL@@ are replaced with `java` / `perl` when non-empty.
/// Returns the number of files modified.
uint32_t relocate_text_placeholders(const fs::path& dir, const std::string& prefix,
                                    const std::string& cellar, const std::string& java = {},
                                    const std::string& perl = {});

/// Replace a hardcoded path string in binary files. The replacement
/// must be <= the original length; the remainder is null-padded.
/// Returns the number of files modified.
uint32_t relocate_binary_paths(const fs::path& dir, const std::string& old_path,
                               const std::string& new_path);

/// Legacy no-op retained for ABI stability of the header surface.
/// Bottle relocation uses install_name_tool placeholder expansion instead.
uint32_t fix_dylib_paths(const fs::path& dir, const fs::path& prefix);

/// True when install_name_tool's output says a rewritten load command doesn't
/// fit in the Mach-O header padding. relocate_bottle turns that into a hard
/// UserError (the caller removes the partly relocated keg).
bool install_name_tool_out_of_header_space(const std::string& output);

/// Per-pour context for relocate_bottle.
struct RelocationOptions {
    /// The package's direct runtime dependencies. Resolves @@HOMEBREW_JAVA@@
    /// (openjdk dep) and @@HOMEBREW_PERL@@ (brewed perl) like Homebrew does.
    std::vector<std::string> dependencies;
    /// Portable (:any_skip_relocation) bottles: Homebrew still replaces text
    /// placeholders on pour but skips dynamic-linkage (Mach-O/ELF) rewriting.
    bool skip_linkage = false;
};

/// Full relocation pass for a poured bottle.
/// Text placeholders + Mach-O install-name/rpath expansion + ad-hoc re-sign
/// (Apple) + ELF interpreter/RPATH expansion via patchelf (Linux). With
/// `options.skip_linkage` only the text pass runs.
void relocate_bottle(const fs::path& package_dir, const std::string& name,
                     const std::string& version, const fs::path& store,
                     const RelocationOptions& options = {});

/// Relocate the bundled Portable Ruby binary to work from a new prefix.
void relocate_ruby(const fs::path& ruby_dir, const std::string& original_prefix);

/// True if the first bytes of a file are a Mach-O (thin or fat) header.
/// Distinguishes fat Mach-O from Java .class files, which share 0xCAFEBABE.
bool looks_like_macho_header(const unsigned char* header, std::size_t size);

} // namespace den

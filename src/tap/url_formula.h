// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"
#include "../index/package.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

// Native install of tap formulae that ship a prebuilt download instead of a
// bottle (GoReleaser / Homebrew Releaser style):
//
//   on_macos do
//     on_arm do
//       url "https://…/tool-1.0-darwin-arm64.tar.gz"
//       sha256 "…"
//     end
//   end
//   def install
//     bin.install "tool"
//   end
//
// den does not evaluate Ruby here. It reads the formula with a small
// line-level scanner that follows on_macos/on_linux/on_arm/on_intel blocks and
// `if OS.mac?` / `Hardware::CPU.arm?|intel?|is_64_bit?` conditionals, takes
// the url/sha256/version/depends_on that apply to this platform (last
// assignment wins, as in Ruby), and accepts only a fixed whitelist of
// `def install` steps. Anything else is refused with the offending line.

struct TapPlatform {
    bool mac = true;
    bool arm = true;
};

// The platform den is running on.
TapPlatform host_tap_platform(const Config& config);

struct ParsedTapFormula {
    std::string version;                   // explicit `version`, else empty
    std::vector<std::string> dependencies; // runtime depends_on for this platform
    std::optional<Package::UrlInstall> url_install;
};

// Scan a formula's source for the values that apply on `platform`.
ParsedTapFormula parse_tap_formula(const std::string& src, const TapPlatform& platform);

// One whitelisted `def install` step.
struct InstallStep {
    enum class Kind { Install, Symlink, Write, Chmod, Mkdir };
    Kind kind = Kind::Install;
    // Stage-relative working directory the step runs in ("" = the stage
    // root). Set by enclosing `mkdir "d" do ... end` / `cd "d" do ... end`
    // blocks; Install sources, globs and if_exists resolve against it.
    std::string cwd;
    std::string dir;                                        // keg-relative target dir
    std::vector<std::pair<std::string, std::string>> items; // Install: src -> new name ("" keeps)
    bool glob = false;                                      // Install: items[0].first is a glob
    std::string if_exists; // Install: only when this staged path exists
    std::string target;    // Symlink: keg-relative path the link points at
    std::string name;      // Symlink/Write/Chmod: file name inside dir
    std::string content;   // Write: heredoc body (interpolations not yet expanded)
    unsigned mode = 0;     // Chmod
    bool create = false;   // Mkdir: create cwd/name (mkdir) or require it (cd)
};

// Parse the joined `def install` statements. On the first statement outside
// the whitelist, returns nullopt and sets `refusal` to the reason.
//
// `mkdir "d" do ... end` (and `cd "d" do ... end`) blocks are flattened: the
// block emits a Mkdir step (name = "d", relative to the enclosing cwd), and
// every step inside gets cwd = enclosing cwd + "/d". Blocks may nest; "d"
// must be a plain relative path without "..".
std::optional<std::vector<InstallStep>> parse_install_steps(const std::vector<std::string>& lines,
                                                            const std::string& formula_name,
                                                            std::string& refusal);

// Values substituted into `write` heredocs.
struct InstallPaths {
    fs::path keg;             // <Cellar>/<name>/<version>, as named in heredocs
    fs::path staging_keg;     // where files are placed (empty: keg)
    fs::path homebrew_prefix; // the prefix the Cellar belongs to
    std::string name;
    std::string version;
};

// Run `steps` with `stage` (the unpacked download) as the working directory,
// installing into paths.keg. Throws UserError on a missing source, a path that
// escapes the stage or keg, or an unsupported interpolation.
void apply_install_steps(const std::vector<InstallStep>& steps, const fs::path& stage,
                         const InstallPaths& paths);

// Download (sha256-verified), unpack and install `pkg` into config.store,
// then write den's keg receipt and the opt link, as for a bottle. Returns the
// keg path. Throws UserError with a clear reason when the formula is refused.
fs::path install_url_formula(const Config& config, const Package& pkg);

} // namespace den

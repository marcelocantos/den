// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Opt-prefix source builds (docs/source-builds.md).
//
// A source build configures the package with its stable opt path,
// `<prefix>/opt/<name>`, so every path the build bakes in (install names,
// pkg-config files, wrapper scripts, a compiled-in sysconfdir) stays valid
// across upgrades -- the same paths a Homebrew bottle carries. The files
// themselves go into the versioned keg, `<prefix>/Cellar/<name>/<version>`:
// install steps run with DESTDIR pointing at a staging directory, and the
// staged `<stage><opt path>` tree is merged into the keg. The live opt link is
// never written during a build.

#include <filesystem>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

/// `<prefix>/opt/<name>` for a keg in `store` (`<prefix>/Cellar`), matching
/// ensure_prefix_opt_link.
fs::path opt_prefix_for(const fs::path& store, const std::string& name);

/// The archive suffix of a source URL (".tar.gz", ".tar.xz", ".tar.bz2",
/// ".zip", ...), ignoring any query string or fragment. Unknown or missing
/// suffixes give ".src": extraction detects the format from content, so the
/// suffix only keeps cached sources honestly named.
std::string source_archive_suffix(const std::string& url);

/// `<cache>/sources/<name>-<version><suffix>` for a source URL.
fs::path source_archive_path(const fs::path& cache, const std::string& name,
                             const std::string& version, const std::string& url);

/// True when a shell build command is an install step that honours DESTDIR:
/// `make`/`gmake` with an `install*` target, `cmake --install`,
/// `cmake --build ... --target install`, `ninja install`, `meson install`.
bool is_install_step(const std::string& shell_cmd);

/// `shell_cmd` with DESTDIR=<stage> applied: appended as a make variable
/// (which overrides a Makefile's own `DESTDIR =`, as in OpenSSL) or exported
/// for cmake/ninja/meson. Commands that are not install steps are returned
/// unchanged.
std::string with_destdir(const std::string& shell_cmd, const fs::path& stage);

/// `shell_cmd` with every mention of `opt_prefix` (as a whole path or a path
/// prefix) replaced by `staged_keg`: the install step of a keg-staged retry
/// (docs/source-builds.md), which installs straight into the staging keg
/// rather than through DESTDIR.
std::string with_staged_keg(const std::string& shell_cmd, const fs::path& opt_prefix,
                            const fs::path& staged_keg);

/// Files an install step staged under `stage` outside `<stage><opt_prefix>`,
/// `<stage><prefix>/etc` and `<stage><prefix>/var` (paths shown relative to
/// the stage, with a leading '/'). Empty when the stage is missing.
std::vector<std::string> staged_strays(const fs::path& stage, const fs::path& opt_prefix,
                                       const fs::path& prefix);

/// After a keg-staged retry, rewrite mentions of `staged_keg` to `opt_prefix`
/// in the text files and symlinks under `staged_keg`. A binary file that
/// mentions it can't be relocated safely: UserError names it.
void relocate_staged_keg(const fs::path& staged_keg, const fs::path& opt_prefix);

/// Move what an install step staged under `stage` into place:
///   * `<stage><opt_prefix>/...` merges into `keg` (a staged file replaces an
///     existing one -- the install step is the newer writer);
///   * `<stage><prefix>/etc/...` and `<stage><prefix>/var/...` go into the
///     shared `<prefix>/etc` and `<prefix>/var`, never replacing a file that
///     is already there (user configuration wins, as with Homebrew).
/// Anything else staged is a build writing outside its keg: the stage is
/// removed and UserError names the stray paths. `stage` is removed on
/// success. A missing stage is a no-op.
void merge_destdir_stage(const fs::path& stage, const fs::path& opt_prefix, const fs::path& keg,
                         const fs::path& prefix);

/// Snapshot of the live `<prefix>/opt/<name>` before a build, so the build
/// can be checked for having written there directly (an install step that
/// ignores DESTDIR). Records whether the path existed, its link target, and
/// the files reachable through it. A link that already names `own_keg` (the
/// keg this build produces, e.g. left behind when that keg was removed) is
/// checked by target only: den itself fills that keg during the build.
class OptPathGuard {
  public:
    explicit OptPathGuard(fs::path opt_path, const fs::path& own_keg = {});

    /// Throws UserError if the opt path changed during the build. A real
    /// directory the build created at the opt path is removed first, so a
    /// failed build never leaves one in place of the link.
    void check() const;

    /// True when the opt path is exactly as it was before the build.
    bool unchanged() const;

    /// For a keg-staged retry: true when the opt path is as it was before the
    /// build, after removing a real directory the build created there (the
    /// state check() would accept or clean up). False when the build changed
    /// a pre-existing opt path, which check() then reports.
    bool settle() const;

    const fs::path& path() const { return opt_path_; }

  private:
    fs::path opt_path_;
    bool existed_ = false;
    bool was_symlink_ = false;
    bool own_keg_link_ = false;
    fs::path link_target_;
    std::vector<std::string> listing_;
};

} // namespace den

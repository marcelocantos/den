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
/// the files reachable through it.
class OptPathGuard {
  public:
    explicit OptPathGuard(fs::path opt_path);

    /// Throws UserError if the opt path changed during the build. A real
    /// directory the build created at the opt path is removed first, so a
    /// failed build never leaves one in place of the link.
    void check() const;

    const fs::path& path() const { return opt_path_; }

  private:
    fs::path opt_path_;
    bool existed_ = false;
    bool was_symlink_ = false;
    fs::path link_target_;
    std::vector<std::string> listing_;
};

} // namespace den

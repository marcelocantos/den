// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Homebrew's declarative post_install steps, the symlink subset (🎯T27-safe:
// den writes the links itself and runs no formula code).
//
// homebrew-core now publishes most post_install logic as data in the API
// ("post_install_steps"). openssl@3, openssl@4, gnutls, libressl and quictls
// all declare exactly one step:
//
//   {"type": "symlink", "force": true,
//    "source": {"path": "{{etc}}/ca-certificates/cert.pem"},
//    "target": {"path": "{{pkgetc}}/cert.pem"}}
//
// i.e. <prefix>/etc/openssl@3/cert.pem -> <prefix>/etc/ca-certificates/cert.pem,
// which is where OpenSSL's OPENSSLDIR looks for its CA bundle. Without it
// TLS verification through den's openssl has no trust store.
//
// den runs a formula's steps only when every step is a plain symlink whose
// paths expand to locations inside the prefix and whose link lands under
// <prefix>/etc or <prefix>/var. Anything else (run, copy, guards, globs,
// sudo, ...) leaves the whole formula's steps unrun, so a formula is never
// half post-installed, and the reason is reported.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

/// One symlink to create: `link` will point at `points_to` (absolute, as
/// Homebrew writes it). `force` replaces an existing file or link.
struct PostInstallSymlink {
    fs::path link;
    fs::path points_to;
    bool force = false;
};

/// What den would do for one formula's post_install_steps.
struct PostInstallPlan {
    std::vector<PostInstallSymlink> links;
    /// Non-empty: den does not run this formula's steps, and why. `links` is
    /// then empty.
    std::string unsupported;
    bool empty() const { return links.empty() && unsupported.empty(); }
};

/// Where the step placeholders point for one keg.
struct PostInstallContext {
    std::string name; // formula (keg) name
    fs::path prefix;  // <prefix> (HOMEBREW_PREFIX)
    fs::path keg;     // <prefix>/Cellar/<name>/<version>
};

/// Expand Homebrew's "{{token}}" placeholders in `path` for `ctx`. Returns
/// false (and leaves `out` alone) for a token den does not know.
bool expand_post_install_path(const std::string& path, const PostInstallContext& ctx, fs::path& out,
                              std::string* unknown_token = nullptr);

/// Plan `steps_json` (the formula's "post_install_steps" array, as JSON
/// text). An empty string plans nothing.
PostInstallPlan plan_post_install(const std::string& steps_json, const PostInstallContext& ctx);

/// What applying a plan did.
struct PostInstallResult {
    uint32_t created = 0;
    /// Links left alone, with why (an existing file without `force`, a real
    /// directory in the way, a symlinked parent directory, ...).
    std::vector<std::string> skipped;
};

/// Create the plan's links. With `replace` false only missing links are
/// created (an existing file or link is never touched); with `replace` true a
/// `force` step replaces an existing file or link, as `brew postinstall`
/// does on install and upgrade. A link already pointing at the right place
/// is left as is either way. Never replaces a real directory and never
/// writes through a parent directory that is a symlink (that could write
/// into a keg). Errors are reported in `skipped`, not thrown.
PostInstallResult apply_post_install(const PostInstallPlan& plan, const fs::path& prefix,
                                     bool replace);

// ---------------------------------------------------------------------------
// ca-certificates
//
// ca-certificates publishes no symlink steps: its post_install runs a script
// that writes <prefix>/etc/ca-certificates/cert.pem, the bundle every
// openssl cert.pem link above points at. den does the portable part natively:
// it copies the keg's share/ca-certificates/cacert.pem (Mozilla's bundle) to
// that path. brew on macOS also merges certificates from the system keychains;
// den does not yet (docs/den-vs-brew.md).
// ---------------------------------------------------------------------------

/// What writing the ca-certificates bundle did.
struct CaBundleResult {
    bool written = false;
    /// Non-empty: nothing was written, and why.
    std::string skipped;
};

/// Write <prefix>/etc/ca-certificates/cert.pem from `keg`'s
/// share/ca-certificates/cacert.pem. The bundle is always a real file, copied
/// to a temporary file in the same directory and renamed into place, so a
/// reader never sees a partial bundle; a symlink at the path is replaced by
/// the file. With `replace` false (a re-materialise with no version change)
/// only a missing bundle is written; with `replace` true (install, upgrade,
/// switch) it is always rewritten. Never writes through a symlinked parent
/// directory or over a directory. Errors are reported, not thrown.
CaBundleResult write_ca_certificates_bundle(const fs::path& prefix, const fs::path& keg,
                                            bool replace);

/// `<prefix>/etc/<name>/cert.pem` symlinks (openssl's post_install links)
/// whose target does not exist, as "<link> -> <target>", sorted.
std::vector<std::string> dangling_cert_pem_links(const fs::path& prefix);

} // namespace den

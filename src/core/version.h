// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>

namespace den {

/// Compare two Homebrew package versions (pkg_version: upstream version plus
/// an optional "_<revision>" suffix, e.g. "3.6.3_1"). Returns <0, 0 or >0.
///
/// The upstream part is split into numeric and alphabetic runs (separators
/// '.', '-', '+', '_' only delimit). Numeric runs compare numerically;
/// alphabetic runs lexicographically, and a numeric run beats an alphabetic
/// one at the same position. A missing component counts as 0, except that a
/// pre-release marker (alpha, beta, pre, rc, dev) sorts below a missing one:
/// "1.0rc1" < "1.0" < "1.0.1" and "1.1.1" < "1.1.1w". Revisions break ties:
/// "3.6.3" < "3.6.3_1" < "3.6.4".
int compare_pkg_versions(const std::string& a, const std::string& b);

/// How an index version relates to the installed one.
enum class VersionDelta {
    Newer, // index is newer: an upgrade
    Same,  // equal (or equivalent, e.g. "1.0" vs "1.0.0")
    Older, // index is older: never applied by an upgrade
    // index has a higher version_scheme but a lower version (openssl@3
    // 3.6.3 -> 3.5.9 LTS, scheme 0 -> 1): brew would call it an upgrade,
    // den holds it. Never applied by an upgrade; only an explicit
    // `den use <name> <version>` moves the keg down.
    Held,
};

VersionDelta version_delta(const std::string& installed, const std::string& available);

/// Compare Homebrew version tuples (version_scheme, version, revision), the
/// way brew orders a formula against an installed keg: a higher
/// version_scheme wins regardless of the version (homebrew-core bumps the
/// scheme when it deliberately moves a formula to a lower version, e.g.
/// openssl@3 3.6.x -> 3.5.9 LTS with version_scheme 1), and within one scheme
/// compare_pkg_versions decides. `a` and `b` are pkg_versions ("3.6.3_1").
int compare_versioned(int scheme_a, const std::string& a, int scheme_b, const std::string& b);

/// Scheme-aware version_delta. Equal pkg_versions are Same whatever the
/// schemes (same keg; brew does not reinstall it either). An upgrade never
/// moves to a lower pkg_version: a lower version with a higher scheme is
/// Held, and a lower (version_scheme, version, revision) tuple is Older.
/// Otherwise a higher tuple is Newer (a scheme bump to a higher version, or
/// a higher version within one scheme).
VersionDelta version_delta(const std::string& installed, int installed_scheme,
                           const std::string& available, int available_scheme);

/// Why a VersionDelta::Held package is not upgraded, e.g. "held: index pins
/// 3.5.9 (version_scheme 1), below installed 3.6.3".
std::string version_hold_reason(const std::string& installed, const std::string& available,
                                int available_scheme);

} // namespace den

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
};

VersionDelta version_delta(const std::string& installed, const std::string& available);

} // namespace den

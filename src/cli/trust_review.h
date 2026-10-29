// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// `den trust review` / `den trust accept`: what the unattended trust policy
// held back, and letting one exact version through.

#include "../trust/degraded_installs.h"
#include "../trust/unattended.h"

#include <optional>
#include <ostream>
#include <string>
#include <utility>

namespace den {

/// Split "<formula>@<version>" at its last '@' ("openssl@3@3.6.0" →
/// {"openssl@3", "3.6.0"}). Nothing when either side is empty.
std::optional<std::pair<std::string, std::string>> parse_formula_at_version(const std::string& s);

/// The full ledger: held back / deferred, accepted, degraded.
void format_trust_review(std::ostream& out, const DegradedInstallSummary& summary);

/// "Trust: N refused, M degraded since …" plus the policy in force, for
/// `den daemon status` and `den doctor`.
void format_trust_status(std::ostream& out, const DegradedInstallSummary& summary,
                         const TrustPolicy& policy);

} // namespace den

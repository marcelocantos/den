// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "trust_review.h"

#include "../core/local_time.h"

namespace den {

namespace {

std::string local_time(long long ts) {
    if (ts <= 0)
        return "?";
    return format_local_time(static_cast<uint64_t>(ts));
}

} // namespace

std::optional<std::pair<std::string, std::string>> parse_formula_at_version(const std::string& s) {
    auto at = s.rfind('@');
    if (at == std::string::npos || at == 0 || at + 1 == s.size())
        return std::nullopt;
    return std::make_pair(s.substr(0, at), s.substr(at + 1));
}

void format_trust_review(std::ostream& out, const DegradedInstallSummary& summary) {
    if (summary.ledger_error) {
        out << "Trust ledger unreadable: " << *summary.ledger_error << "\n";
        return;
    }
    out << "Trust: " << format_trust_ledger(summary) << "\n";
    if (!summary.refused_installs.empty()) {
        out << "Held back or deferred (not installed):\n";
        for (const auto& d : summary.refused_installs) {
            out << "  " << d.name << "@" << d.version << "  [" << d.reason;
            if (!d.status.empty())
                out << "; " << d.status;
            out << "]  " << local_time(d.recorded_at) << "\n";
        }
        out << "  Accept one exact version with `den trust accept <formula>@<version>`.\n";
    }
    if (!summary.accepted.empty()) {
        out << "Accepted, waiting to install:\n";
        for (const auto& d : summary.accepted)
            out << "  " << d.name << "@" << d.version << "\n";
    }
    if (!summary.installs.empty()) {
        out << "Installed on a single hash source (degraded):\n";
        for (const auto& d : summary.installs)
            out << "  " << d.name << "@" << d.version << "  [" << d.reason << "]  "
                << local_time(d.recorded_at) << "\n";
    }
}

void format_trust_status(std::ostream& out, const DegradedInstallSummary& summary,
                         const TrustPolicy& policy) {
    out << "Trust: " << format_trust_ledger(summary) << "\n";
    out << "Unattended trust policy: trust.unattended="
        << (policy.mode == UnattendedMode::Warn ? "warn" : "refuse") << ", notify "
        << (policy.notify ? "on" : "off") << ", source builds "
        << (policy.source_builds ? "allowlist" : "off") << "\n";
}

} // namespace den

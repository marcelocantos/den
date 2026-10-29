// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "unattended.h"

#include "../settings/settings.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <mutex>

namespace den {

namespace {

std::mutex g_mutex;
std::optional<std::string> g_override;

} // namespace

UnattendedMode parse_unattended_mode(const std::string& value) {
    if (value == "warn")
        return UnattendedMode::Warn;
    if (value != "refuse")
        SPDLOG_WARN("trust.unattended: unknown value '{}', treating it as 'refuse'", value);
    return UnattendedMode::Refuse;
}

std::vector<std::string> trust_settings_problems(const TrustSettings& settings) {
    std::vector<std::string> out;
    if (settings.unattended != "warn" && settings.unattended != "refuse")
        out.push_back("trust.unattended is '" + settings.unattended +
                      "', which is not warn or refuse: unattended runs treat it as refuse. Fix "
                      "with `den set trust.unattended warn` (or refuse)");
    if (settings.unattended_source_builds != "off" &&
        settings.unattended_source_builds != "allowlist")
        out.push_back("trust.unattended_source_builds is '" + settings.unattended_source_builds +
                      "', which is not off or allowlist: unattended runs treat it as off. Fix "
                      "with `den set trust.unattended_source_builds off` (or allowlist)");
    return out;
}

TrustPolicy make_trust_policy(const TrustSettings& settings,
                              std::optional<std::string> unattended) {
    TrustPolicy p;
    p.unattended = std::move(unattended);
    p.mode = parse_unattended_mode(settings.unattended);
    p.notify = settings.notify;
    if (settings.unattended_source_builds == "allowlist") {
        p.source_builds = true;
    } else if (settings.unattended_source_builds != "off") {
        SPDLOG_WARN("trust.unattended_source_builds: unknown value '{}', treating it as 'off'",
                    settings.unattended_source_builds);
    }
    p.source_build_allowlist.insert(settings.source_build_allowlist.begin(),
                                    settings.source_build_allowlist.end());
    return p;
}

std::optional<std::string> detect_unattended(const std::optional<std::string>& override_reason,
                                             const char* xpc_service_name) {
    if (override_reason)
        return override_reason;
    if (xpc_service_name && std::string(xpc_service_name) == kDaemonLaunchdLabel)
        return std::string("launchd job ") + kDaemonLaunchdLabel;
    return std::nullopt;
}

void set_unattended(std::optional<std::string> reason) {
    std::lock_guard lock(g_mutex);
    g_override = std::move(reason);
}

std::optional<std::string> unattended_reason() {
    std::optional<std::string> override_reason;
    {
        std::lock_guard lock(g_mutex);
        override_reason = g_override;
    }
    return detect_unattended(override_reason, std::getenv("XPC_SERVICE_NAME"));
}

TrustPolicy current_trust_policy(const Config& config) {
    return make_trust_policy(read_settings(config.den_home).trust, unattended_reason());
}

std::string hold_reason(TrustDecision decision, const std::string& status_tag) {
    if (decision == TrustDecision::WarnProceed)
        return "single-source";
    if (status_tag == "found")
        return "hash-mismatch";
    if (status_tag == "replica-unreadable")
        return "replica-unreadable";
    return "unverifiable";
}

bool hold_is_acceptable(const std::string& reason) {
    return reason == "single-source" || reason == "unattended-source-build";
}

BottleAction bottle_action(const TrustPolicy& policy, TrustDecision decision, bool accepted) {
    switch (decision) {
    case TrustDecision::Proceed:
        return BottleAction::Install;
    case TrustDecision::Refuse:
        return BottleAction::Refuse;
    case TrustDecision::WarnProceed:
        if (policy.unattended && policy.mode == UnattendedMode::Refuse && !accepted)
            return BottleAction::HoldBack;
        return BottleAction::InstallDegraded;
    }
    return BottleAction::Refuse;
}

SourceBuildAction source_build_action(const TrustPolicy& policy, const std::string& name,
                                      TrustDecision source_decision, bool accepted) {
    if (source_decision == TrustDecision::Refuse)
        return SourceBuildAction::Refuse;
    if (!policy.unattended || accepted)
        return SourceBuildAction::Build;
    if (!policy.source_builds || !policy.source_build_allowlist.count(name))
        return SourceBuildAction::Defer;
    return source_decision == TrustDecision::Proceed ? SourceBuildAction::Build
                                                     : SourceBuildAction::Defer;
}

} // namespace den

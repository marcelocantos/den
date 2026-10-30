// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "daemon.h"
#include "../activity/activity.h"
#include "../core/error.h"
#include "../core/version.h"
#include "../env/disabled.h"
#include "../env/environment.h"
#include "../env/manifest.h"
#include "../env/version_change.h"
#include "../index/index.h"
#include "../provider/homebrew_provider.h"
#include "../provider/registry.h"
#include "../store/pins.h"
#include "../store/store.h"
#include "../trust/degraded_installs.h"
#include "../trust/unattended.h"
#include "inuse.h"
#include "plist_refresh.h"
#include "trust_screen.h"
#include "upgrade_pass.h"

#include <algorithm>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <csignal>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <tuple>
#include <unistd.h>

namespace den {

// ---- Constants -------------------------------------------------------------

static constexpr uint64_t DEFAULT_INTERVAL_SECS = 21600; // 6 hours

// ---- Internal state --------------------------------------------------------

namespace {

// Volatile flag set by the SIGTERM handler.
volatile sig_atomic_t g_terminate = 0;

void sigterm_handler(int /*sig*/) {
    g_terminate = 1;
}

// Paths.
fs::path pid_path(const fs::path& den_home) {
    return den_home / "daemon.pid";
}

fs::path state_path(const fs::path& den_home) {
    return den_home / "daemon_state.json";
}

fs::path log_path(const fs::path& den_home) {
    return den_home / "daemon.log";
}

// Read daemon settings from den_home/config.json (falls back to defaults).
struct DaemonSettings {
    bool auto_download = true;
    bool auto_upgrade = false;
    std::string upgrade_window; // "HH:MM-HH:MM" or empty
    uint64_t interval_secs = DEFAULT_INTERVAL_SECS;
};

DaemonSettings read_daemon_settings(const fs::path& den_home) {
    DaemonSettings s;
    fs::path cfg = den_home / "config.json";
    try {
        std::ifstream f(cfg);
        if (!f.is_open())
            return s;
        nlohmann::json j;
        f >> j;
        if (j.contains("daemon")) {
            const auto& d = j["daemon"];
            auto as_bool = [](const nlohmann::json& v, bool* out) {
                if (v.is_boolean()) {
                    *out = v.get<bool>();
                    return true;
                }
                if (v.is_string()) {
                    if (v.get<std::string>() == "true") {
                        *out = true;
                        return true;
                    }
                    if (v.get<std::string>() == "false") {
                        *out = false;
                        return true;
                    }
                }
                return false;
            };
            if (d.contains("auto_download"))
                as_bool(d["auto_download"], &s.auto_download);
            if (d.contains("auto_upgrade"))
                as_bool(d["auto_upgrade"], &s.auto_upgrade);
            if (d.contains("upgrade_window") && d["upgrade_window"].is_string())
                s.upgrade_window = d["upgrade_window"].get<std::string>();
            if (d.contains("interval_secs") && d["interval_secs"].is_number_unsigned())
                s.interval_secs = d["interval_secs"].get<uint64_t>();
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("daemon: failed to read settings from {}: {}", cfg.string(), e.what());
    }
    return s;
}

} // anonymous namespace

// ---- Utilities -------------------------------------------------------------

uint64_t now_secs() {
    return static_cast<uint64_t>(std::time(nullptr));
}

bool in_maintenance_window(const std::string& window) {
    if (window.empty())
        return true;

    // Parse "HH:MM-HH:MM".
    int start_h = -1, start_m = -1, end_h = -1, end_m = -1;
    char sep1 = 0, sep2 = 0, sep3 = 0;
    std::istringstream ss(window);
    ss >> start_h >> sep1 >> start_m >> sep2 >> end_h >> sep3 >> end_m;
    if (ss.fail() || sep1 != ':' || sep2 != '-' || sep3 != ':') {
        SPDLOG_WARN("daemon: invalid maintenance window '{}', treating as always-open", window);
        return true;
    }
    if (start_h < 0 || start_h > 23 || start_m < 0 || start_m > 59 || end_h < 0 || end_h > 23 ||
        end_m < 0 || end_m > 59) {
        SPDLOG_WARN("daemon: out-of-range maintenance window '{}', treating as always-open",
                    window);
        return true;
    }

    std::time_t t = std::time(nullptr);
    std::tm* lt = std::localtime(&t);
    if (!lt)
        return true;

    int now_mins = lt->tm_hour * 60 + lt->tm_min;
    int start_mins = start_h * 60 + start_m;
    int end_mins = end_h * 60 + end_m;

    if (start_mins <= end_mins) {
        return now_mins >= start_mins && now_mins < end_mins;
    } else {
        // Window wraps midnight.
        return now_mins >= start_mins || now_mins < end_mins;
    }
}

void log_daemon(const fs::path& den_home, const std::string& msg) {
    std::time_t t = std::time(nullptr);
    std::tm* lt = std::localtime(&t);
    char buf[32] = {};
    if (lt)
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", lt);

    std::ofstream f(log_path(den_home), std::ios::app);
    if (f) {
        f << '[' << buf << "] " << msg << '\n';
    }
}

// ---- State I/O -------------------------------------------------------------

DaemonState read_daemon_state(const fs::path& den_home) {
    DaemonState state;
    fs::path path = state_path(den_home);

    try {
        std::ifstream f(path);
        if (!f.is_open())
            return state;
        nlohmann::json j;
        f >> j;

        if (j.contains("last_check") && j["last_check"].is_number_unsigned())
            state.last_check = j["last_check"].get<uint64_t>();

        if (j.contains("last_upgrade") && j["last_upgrade"].is_number_unsigned())
            state.last_upgrade = j["last_upgrade"].get<uint64_t>();

        if (j.contains("errors") && j["errors"].is_array()) {
            for (const auto& e : j["errors"]) {
                if (e.is_string())
                    state.errors.push_back(e.get<std::string>());
            }
        }

        if (j.contains("pending") && j["pending"].is_array()) {
            for (const auto& p : j["pending"]) {
                if (!p.is_object())
                    continue;
                PendingUpgrade pu;
                if (p.contains("name") && p["name"].is_string())
                    pu.name = p["name"].get<std::string>();
                if (p.contains("installed") && p["installed"].is_string())
                    pu.installed = p["installed"].get<std::string>();
                if (p.contains("available") && p["available"].is_string())
                    pu.available = p["available"].get<std::string>();
                if (p.contains("installed_scheme") && p["installed_scheme"].is_number_integer())
                    pu.installed_scheme = p["installed_scheme"].get<int>();
                if (p.contains("available_scheme") && p["available_scheme"].is_number_integer())
                    pu.available_scheme = p["available_scheme"].get<int>();
                state.pending.push_back(std::move(pu));
            }
        }

        if (j.contains("index_older") && j["index_older"].is_array()) {
            for (const auto& p : j["index_older"]) {
                if (!p.is_object())
                    continue;
                PendingUpgrade pu;
                if (p.contains("name") && p["name"].is_string())
                    pu.name = p["name"].get<std::string>();
                if (p.contains("installed") && p["installed"].is_string())
                    pu.installed = p["installed"].get<std::string>();
                if (p.contains("available") && p["available"].is_string())
                    pu.available = p["available"].get<std::string>();
                if (p.contains("installed_scheme") && p["installed_scheme"].is_number_integer())
                    pu.installed_scheme = p["installed_scheme"].get<int>();
                if (p.contains("available_scheme") && p["available_scheme"].is_number_integer())
                    pu.available_scheme = p["available_scheme"].get<int>();
                state.index_older.push_back(std::move(pu));
            }
        }

        if (j.contains("version_held") && j["version_held"].is_array()) {
            for (const auto& d : j["version_held"]) {
                if (!d.is_object())
                    continue;
                DeferredUpgrade du;
                du.name = d.value("name", std::string());
                du.installed = d.value("installed", std::string());
                du.available = d.value("available", std::string());
                du.reason = d.value("reason", std::string());
                state.version_held.push_back(std::move(du));
            }
        }

        if (j.contains("deferred") && j["deferred"].is_array()) {
            for (const auto& d : j["deferred"]) {
                if (!d.is_object())
                    continue;
                DeferredUpgrade du;
                if (d.contains("name") && d["name"].is_string())
                    du.name = d["name"].get<std::string>();
                if (d.contains("installed") && d["installed"].is_string())
                    du.installed = d["installed"].get<std::string>();
                if (d.contains("available") && d["available"].is_string())
                    du.available = d["available"].get<std::string>();
                if (d.contains("reason") && d["reason"].is_string())
                    du.reason = d["reason"].get<std::string>();
                state.deferred.push_back(std::move(du));
            }
        }

        if (j.contains("trust_held") && j["trust_held"].is_array()) {
            for (const auto& d : j["trust_held"]) {
                if (!d.is_object())
                    continue;
                DeferredUpgrade du;
                du.name = d.value("name", std::string());
                du.installed = d.value("installed", std::string());
                du.available = d.value("available", std::string());
                du.reason = d.value("reason", std::string());
                state.trust_held.push_back(std::move(du));
            }
        }
        if (j.contains("last_trust_notification") &&
            j["last_trust_notification"].is_number_unsigned())
            state.last_trust_notification = j["last_trust_notification"].get<uint64_t>();

        if (j.contains("disabled_blocked") && j["disabled_blocked"].is_array()) {
            for (const auto& d : j["disabled_blocked"]) {
                if (!d.is_object())
                    continue;
                DisabledBlocked b;
                b.name = d.value("name", std::string());
                b.installed = d.value("installed", std::string());
                b.available = d.value("available", std::string());
                if (d.contains("blockers") && d["blockers"].is_array()) {
                    for (const auto& x : d["blockers"])
                        if (x.is_object())
                            b.blockers.push_back({x.value("disabled", std::string()),
                                                  x.value("reason", std::string())});
                } else if (d.contains("disabled") && d["disabled"].is_string()) {
                    // The single-blocker form an older den wrote.
                    b.blockers.push_back(
                        {d["disabled"].get<std::string>(), d.value("reason", std::string())});
                }
                if (d.contains("recorded") && d["recorded"].is_number_unsigned())
                    b.recorded = d["recorded"].get<uint64_t>();
                else
                    b.recorded = state.last_check;
                state.disabled_blocked.push_back(std::move(b));
            }
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("daemon: failed to read state from {}: {}", path.string(), e.what());
    }
    return state;
}

void write_daemon_state(const fs::path& den_home, const DaemonState& state) {
    nlohmann::json j;
    if (state.last_check)
        j["last_check"] = *state.last_check;
    if (state.last_upgrade)
        j["last_upgrade"] = *state.last_upgrade;

    j["errors"] = state.errors;

    nlohmann::json pending = nlohmann::json::array();
    for (const auto& p : state.pending) {
        pending.push_back({{"name", p.name},
                           {"installed", p.installed},
                           {"available", p.available},
                           {"installed_scheme", p.installed_scheme},
                           {"available_scheme", p.available_scheme}});
    }
    j["pending"] = pending;

    nlohmann::json older = nlohmann::json::array();
    for (const auto& p : state.index_older) {
        older.push_back({{"name", p.name},
                         {"installed", p.installed},
                         {"available", p.available},
                         {"installed_scheme", p.installed_scheme},
                         {"available_scheme", p.available_scheme}});
    }
    j["index_older"] = older;

    nlohmann::json vheld = nlohmann::json::array();
    for (const auto& h : state.version_held) {
        vheld.push_back({{"name", h.name},
                         {"installed", h.installed},
                         {"available", h.available},
                         {"reason", h.reason}});
    }
    j["version_held"] = vheld;

    nlohmann::json deferred = nlohmann::json::array();
    for (const auto& d : state.deferred) {
        deferred.push_back({{"name", d.name},
                            {"installed", d.installed},
                            {"available", d.available},
                            {"reason", d.reason}});
    }
    j["deferred"] = deferred;

    nlohmann::json trust_held = nlohmann::json::array();
    for (const auto& d : state.trust_held) {
        trust_held.push_back({{"name", d.name},
                              {"installed", d.installed},
                              {"available", d.available},
                              {"reason", d.reason}});
    }
    j["trust_held"] = trust_held;
    if (state.last_trust_notification)
        j["last_trust_notification"] = *state.last_trust_notification;

    nlohmann::json blocked = nlohmann::json::array();
    for (const auto& b : state.disabled_blocked) {
        nlohmann::json blockers = nlohmann::json::array();
        for (const auto& x : b.blockers)
            blockers.push_back({{"disabled", x.disabled}, {"reason", x.reason}});
        std::string reasons;
        for (const auto& x : unique_disabled_blockers(b))
            reasons += (reasons.empty() ? "" : "; ") + x.reason;
        nlohmann::json e = {{"name", b.name},
                            {"installed", b.installed},
                            {"available", b.available},
                            {"blockers", blockers},
                            // The single-blocker form older den reads.
                            {"disabled", b.blockers.empty() ? "" : b.blockers.front().disabled},
                            {"reason", reasons}};
        if (b.recorded)
            e["recorded"] = *b.recorded;
        blocked.push_back(std::move(e));
    }
    j["disabled_blocked"] = blocked;

    // Atomic write: write to .tmp then rename.
    fs::path tmp = state_path(den_home).string() + ".tmp";
    try {
        fs::create_directories(den_home);
        {
            std::ofstream f(tmp);
            if (!f)
                throw UserError("daemon: cannot open " + tmp.string() + " for writing");
            f << j.dump(2) << '\n';
        }
        fs::rename(tmp, state_path(den_home));
    } catch (const std::exception& e) {
        SPDLOG_ERROR("daemon: failed to write state: {}", e.what());
        std::error_code ec;
        fs::remove(tmp, ec);
    }
}

// ---- Lifecycle -------------------------------------------------------------

// ---- Maintenance cycle (🎯T13) ---------------------------------------------

UpgradeSelection select_upgrade(const std::string& name, const std::string& version,
                                const Package* pkg, const InstalledSchemeFn& installed_scheme) {
    UpgradeSelection sel;
    if (!pkg || pkg->version.empty()) {
        sel.skip = UpgradeSkip::NotInIndex;
        return sel;
    }
    if (pkg->version != version) {
        sel.installed_scheme = installed_scheme ? installed_scheme(name, version) : 0;
        sel.delta = version_delta(version, sel.installed_scheme, pkg->version, pkg->version_scheme);
    }
    if (pkg->disabled)
        sel.skip = UpgradeSkip::Disabled; // formula or cask: never offered
    else if (pkg->artifact_type == ArtifactType::App || !pkg->cask_artifacts.empty())
        sel.skip = UpgradeSkip::Cask; // cask upgrades stay manual
    return sel;
}

std::vector<PendingUpgrade>
compute_pending(const std::map<std::string, std::string>& installed, const PackageIndex& idx,
                std::vector<PendingUpgrade>* index_older, const InstalledSchemeFn& installed_scheme,
                std::vector<DeferredUpgrade>* version_held, std::vector<PendingUpgrade>* casks,
                std::vector<PendingUpgrade>* disabled) {
    std::vector<PendingUpgrade> out;
    if (index_older)
        index_older->clear();
    if (version_held)
        version_held->clear();
    if (casks)
        casks->clear();
    if (disabled)
        disabled->clear();
    for (const auto& [name, version] : installed) {
        const Package* pkg = idx.find(name);
        const auto sel = select_upgrade(name, version, pkg, installed_scheme);
        if (sel.skip == UpgradeSkip::NotInIndex || sel.delta == VersionDelta::Same)
            continue;
        const PendingUpgrade pu{name, version, pkg->version, sel.installed_scheme,
                                pkg->version_scheme};
        if (sel.skip == UpgradeSkip::Disabled) {
            // Never offered; a formula the daemon would otherwise upgrade is
            // reported as blocked (a cask would not be upgraded anyway).
            if (disabled && sel.delta == VersionDelta::Newer && !is_cask_package(*pkg))
                disabled->push_back(pu);
            continue;
        }
        if (sel.skip == UpgradeSkip::Cask) {
            if (casks && sel.delta == VersionDelta::Newer)
                casks->push_back(pu);
            continue;
        }
        switch (sel.delta) {
        case VersionDelta::Newer:
            out.push_back(pu);
            break;
        case VersionDelta::Held:
            // A version_scheme bump to a lower version (openssl@3 3.6.x ->
            // 3.5.9 LTS, scheme 0 -> 1). brew would upgrade; den holds it.
            if (version_held)
                version_held->push_back(
                    {name, version, pkg->version,
                     version_hold_reason(version, pkg->version, pkg->version_scheme)});
            break;
        case VersionDelta::Older:
            // Never downgrade from an upgrade path: the index can lag what is
            // installed (a stale cache or mirror, or an index that predates a
            // version_scheme bump). Report it.
            if (index_older)
                index_older->push_back(pu);
            break;
        case VersionDelta::Same:
            break;
        }
    }
    return out; // std::map iteration → already sorted by name
}

OutdatedPlan outdated_plan(const Config& config, const PackageIndex& idx,
                           std::string_view tracked) {
    OutdatedPlan plan;
    const auto per = resolve_per_provider(config.den_home, active_env_path(config.den_home));
    for (const auto& [provider, pkgs] : per) {
        if (provider == tracked) {
            plan.homebrew = pkgs.size();
            plan.pending = compute_pending(
                pkgs, idx, &plan.index_older,
                [&config](const std::string& name, const std::string& version) {
                    return installed_version_scheme(config.store / keg_name(name) / version);
                },
                &plan.held, &plan.casks);
            continue;
        }
        for (const auto& [name, version] : pkgs)
            plan.untracked.push_back({name, version, provider});
    }
    std::sort(plan.untracked.begin(), plan.untracked.end(),
              [](const UntrackedPackage& a, const UntrackedPackage& b) {
                  return std::tie(a.name, a.provider) < std::tie(b.name, b.provider);
              });
    return plan;
}

std::string index_older_message(const PendingUpgrade& p) {
    return p.name + " " + p.installed + ": index older than installed (index has " + p.available +
           "); not downgrading. To downgrade explicitly: den use " + p.name + " " + p.available;
}

std::vector<DisabledBlocker> unique_disabled_blockers(const DisabledBlocked& b) {
    std::vector<DisabledBlocker> out;
    for (const auto& x : b.blockers)
        if (std::none_of(out.begin(), out.end(),
                         [&](const DisabledBlocker& o) { return o.disabled == x.disabled; }))
            out.push_back(x);
    return out;
}

std::string disabled_blocked_message(const DisabledBlocked& b) {
    std::string reasons;
    for (const auto& x : unique_disabled_blockers(b))
        reasons += (reasons.empty() ? "" : "; ") + x.reason;
    return b.name + " " + b.installed + " -> " + b.available + ": " + reasons;
}

std::string disabled_blocked_report(const DisabledBlocked& b) {
    return disabled_blocked_message(b) + "  [recorded " +
           (b.recorded ? format_local_time(*b.recorded) : std::string("?")) + "]";
}

std::string disabled_blocker_names(const DisabledBlocked& b) {
    std::string out;
    for (const auto& x : unique_disabled_blockers(b))
        out += (out.empty() ? "" : ", ") + x.disabled;
    return out;
}

std::string version_held_message(const DeferredUpgrade& h) {
    return h.name + " " + h.installed + ": " + h.reason +
           "; not downgrading. To downgrade explicitly: den use " + h.name + " " + h.available;
}

MaintenanceReport run_maintenance_cycle(const fs::path& cellar, const fs::path& den_home,
                                        const MaintenanceSettings& settings, DaemonState& state,
                                        const MaintenanceHooks& hooks) {
    MaintenanceReport report;
    state.errors.clear();
    state.last_check = now_secs();

    PackageIndex idx;
    try {
        idx = hooks.refresh_index();
        report.index_refreshed = true;
    } catch (const std::exception& e) {
        state.errors.push_back(std::string("index refresh: ") + e.what());
        log_daemon(den_home, std::string("index refresh failed: ") + e.what());
        report.pending = state.pending.size();
        return report; // keep the previous pending list; retry next cycle
    }

    std::vector<PendingUpgrade> self_disabled;
    try {
        state.pending =
            compute_pending(hooks.installed(), idx, &state.index_older, hooks.installed_scheme,
                            &state.version_held, nullptr, &self_disabled);
    } catch (const std::exception& e) {
        state.errors.push_back(std::string("outdated check: ") + e.what());
        report.pending = state.pending.size();
        return report;
    }
    // A deferral only makes sense while its package is still pending.
    std::erase_if(state.deferred, [&](const DeferredUpgrade& d) {
        return std::none_of(state.pending.begin(), state.pending.end(),
                            [&](const PendingUpgrade& p) { return p.name == d.name; });
    });
    // An upgrade that would pour a package disabled upstream is never
    // applied. One whose package is itself disabled is never pending: it is
    // re-derived from this check every cycle. One the upgrade pass found
    // (a disabled dependency or dependent) holds while its upgrade is still
    // pending at the same version; the next pass re-checks it.
    std::erase_if(state.disabled_blocked, [&](const DisabledBlocked& b) {
        return std::any_of(b.blockers.begin(), b.blockers.end(),
                           [&](const DisabledBlocker& x) { return x.disabled == b.name; }) ||
               std::none_of(state.pending.begin(), state.pending.end(),
                            [&](const PendingUpgrade& p) {
                                return p.name == b.name && p.available == b.available;
                            });
    });
    for (const auto& pu : self_disabled) {
        DisabledBlocked b{pu.name,
                          pu.installed,
                          pu.available,
                          {{pu.name, disabled_package_message(*idx.find(pu.name))}},
                          state.last_check};
        log_daemon(den_home, "not upgrading " + disabled_blocked_message(b));
        state.disabled_blocked.push_back(std::move(b));
    }
    std::sort(state.disabled_blocked.begin(), state.disabled_blocked.end(),
              [](const DisabledBlocked& x, const DisabledBlocked& y) { return x.name < y.name; });
    report.disabled_blocked = state.disabled_blocked.size();
    report.pending = state.pending.size();
    report.index_older = state.index_older.size();
    log_daemon(den_home, std::to_string(report.pending) + " package(s) outdated");
    for (const auto& o : state.index_older)
        log_daemon(den_home, index_older_message(o));
    report.version_held = state.version_held.size();
    for (const auto& h : state.version_held)
        log_daemon(den_home, version_held_message(h));

    if (settings.auto_download && hooks.prefetch) {
        // A pinned package is never upgraded (the upgrade pass and `den
        // upgrade` both hold it), so pre-downloading its bottle only wastes
        // bandwidth and cache space. Pins are read like the upgrade pass
        // reads them: an unreadable pins.json fails closed and skips every
        // pre-download rather than treating everything as unpinned.
        PinMap pins;
        bool pins_ok = true;
        try {
            pins = read_pins(den_home);
        } catch (const std::exception& e) {
            pins_ok = false;
            state.errors.push_back(
                std::string("download: pins unreadable, skipping pre-download: ") + e.what());
            log_daemon(den_home,
                       std::string("pins unreadable, skipping pre-download: ") + e.what());
        }
        std::size_t wanted = 0;
        for (const auto& pu : state.pending) {
            if (!pins_ok || pins.count(pu.name)) {
                ++report.skipped_pinned;
                continue;
            }
            ++wanted;
            try {
                hooks.prefetch(pu);
                ++report.downloaded;
            } catch (const std::exception& e) {
                state.errors.push_back("download " + pu.name + ": " + e.what());
            }
        }
        if (!state.pending.empty()) {
            std::string msg = "pre-downloaded " + std::to_string(report.downloaded) + "/" +
                              std::to_string(wanted) + " upgrade(s)";
            if (report.skipped_pinned > 0)
                msg += ", skipped " + std::to_string(report.skipped_pinned) + " pinned";
            log_daemon(den_home, msg);
        }
    }

    if (settings.auto_upgrade && !state.pending.empty()) {
        bool in_window = settings.upgrade_window.empty() ||
                         (hooks.in_window ? hooks.in_window(settings.upgrade_window)
                                          : in_maintenance_window(settings.upgrade_window));
        if (in_window) {
            // Screen against the unattended trust policy first: a held-back
            // upgrade (and every pending upgrade that would install it) sits
            // out the pass and stays pending.
            std::map<std::string, std::string> held;
            if (hooks.trust_screen) {
                try {
                    held = hooks.trust_screen(state.pending);
                } catch (const std::exception& e) {
                    // Fail closed: an unscreened pass would install unchecked.
                    state.errors.push_back(std::string("trust screen: ") + e.what());
                    log_daemon(den_home,
                               std::string("trust screen failed, not upgrading: ") + e.what());
                    report.pending = state.pending.size();
                    return report;
                }
            }
            std::vector<PendingUpgrade> held_pending;
            std::vector<DeferredUpgrade> trust_held;
            std::vector<std::string> newly_held;
            std::erase_if(state.pending, [&](const PendingUpgrade& pu) {
                auto it = held.find(pu.name);
                if (it == held.end())
                    return false;
                held_pending.push_back(pu);
                trust_held.push_back({pu.name, pu.installed, pu.available, it->second});
                bool before =
                    std::any_of(state.trust_held.begin(), state.trust_held.end(),
                                [&](const DeferredUpgrade& d) {
                                    return d.name == pu.name && d.available == pu.available;
                                });
                if (!before)
                    newly_held.push_back(pu.name);
                return true;
            });
            UpgradePassResult res = run_upgrade_pass(cellar, den_home, state, hooks.apply);
            for (auto& pu : held_pending)
                state.pending.push_back(std::move(pu));
            std::sort(
                state.pending.begin(), state.pending.end(),
                [](const PendingUpgrade& a, const PendingUpgrade& b) { return a.name < b.name; });
            state.trust_held = std::move(trust_held);
            report.applied = res.applied.size();
            report.deferred = res.deferred.size();
            report.trust_held = state.trust_held.size();
            report.disabled_blocked = state.disabled_blocked.size();
            for (const auto& h : state.trust_held)
                log_daemon(den_home, "held back " + h.name + " " + h.available + ": " + h.reason);
            if (!newly_held.empty() && hooks.notify_held) {
                try {
                    if (hooks.notify_held(newly_held))
                        state.last_trust_notification = now_secs();
                } catch (const std::exception& e) {
                    state.errors.push_back(std::string("notify: ") + e.what());
                }
            }
            log_daemon(den_home, "upgrade pass: applied " + std::to_string(report.applied) +
                                     ", deferred " + std::to_string(report.deferred) +
                                     ", held back " + std::to_string(report.trust_held) +
                                     ", blocked (disabled upstream) " +
                                     std::to_string(res.disabled_blocked.size()));
        } else {
            log_daemon(den_home,
                       "outside upgrade window '" + settings.upgrade_window + "', not upgrading");
        }
    }
    return report;
}

void apply_daemon_upgrade(const Config& config, PackageProvider& provider, const PackageIndex& idx,
                          const PendingUpgrade& pu) {
    // Install, record and link as one unit: a version that cannot be linked
    // is rolled back (manifest, links, opt link, poured keg) and reported as
    // a failure, so the pass keeps it pending and logs no activity.
    // Revision-bumped dependents move in the same batch (and are rolled back
    // with it). Their own pending entries, if any, then find them current;
    // the pass logs activity for those as usual.
    // A dependent that would move under a running process defers the whole
    // batch (the pass already checked pu's own keg).
    // A dependent is screened by the unattended trust policy like any
    // pending upgrade; a held one stays put (and is in the trust ledger).
    // Nothing disabled upstream is poured: not the package, a dependency it
    // would pour, or a revision-bumped dependent that is disabled or would
    // pour one (the check `den upgrade` refuses with, minus --force). A
    // refused dependent holds the whole batch, so the chain is not split.
    // Every disabled package in the way is reported, not just the first: the
    // package's own pours, then each refused dependent's. When the package's
    // own pours already block it the batch is only planned (plan_only, no
    // trust screen), to find the dependents that would block it too.
    std::vector<DisabledBlocker> blockers;
    for (const auto& p : disabled_pours(config, provider, idx, pu.name, /*include_self=*/true))
        blockers.push_back({p.pkg->name, p.message});
    std::map<std::string, std::vector<DisabledPour>> refused_by; // dependent -> its pours
    VersionChangeOptions opts;
    opts.plan_only = !blockers.empty();
    opts.refuse_dependent = [&](const VersionChange& c) -> std::optional<std::string> {
        auto pours = disabled_pours(config, provider, idx, c.name, /*include_self=*/true);
        if (pours.empty())
            return std::nullopt;
        auto why = pours.front().message;
        refused_by[c.name] = std::move(pours);
        return why;
    };
    opts.refused_dependent_holds_batch = true;
    opts.upgrade_dependents = true;
    opts.keg_in_use = [](const fs::path& keg) { return !files_in_use_under(keg).empty(); };
    if (const TrustPolicy policy = current_trust_policy(config);
        policy.unattended && !opts.plan_only) {
        opts.screen_dependents = [&config, &idx, policy](const std::vector<VersionChange>& cs) {
            std::vector<PendingUpgrade> pending;
            for (const auto& c : cs)
                pending.push_back({c.name, c.from, c.to});
            return screen_unattended_upgrades(config, idx, pending, policy);
        };
    }
    auto res = apply_version_changes(config, provider, &idx,
                                     {{pu.name, pu.installed, pu.available}}, opts);
    for (const auto& [dependent, why] : res.refused_dependents) {
        const auto prefix = "dependent " + dependent.name + " " + dependent.from + " (" +
                            dependent.to + " available): ";
        for (const auto& p : refused_by[dependent.name])
            blockers.push_back({p.pkg->name, prefix + p.message});
    }
    if (!blockers.empty())
        throw UpgradeDisabled(std::move(blockers));
    if (!res.deferred_in_use.empty()) {
        std::string held;
        for (const auto& c : res.deferred_in_use) {
            held += (held.empty() ? "" : ", ") + c.name + " " + c.from;
        }
        throw UpgradeDeferred("dependent in use: " + held);
    }
    if (!res.rolled_back.empty()) {
        throw UserError(
            describe_rollback(config.store.parent_path(), res.rolled_back.front(), "auto-upgrade"));
    }
    if (!res.conflicts.empty()) {
        throw UserError(describe_conflicts(config.store.parent_path(), res.conflicts));
    }
}

namespace {

// Post a macOS notification without waiting for it (osascript is detached;
// the shell exits at once). Elsewhere there is nothing to post to: the hold
// is in the daemon log, `den daemon status` and `den doctor`.
bool post_desktop_notification(const std::string& title, const std::string& body) {
#ifdef __APPLE__
    auto quote = [](const std::string& s) {
        // AppleScript string inside a single-quoted shell word.
        std::string out = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\')
                out += '\\';
            if (c == '\'')
                out += "'\\''";
            else
                out += c;
        }
        return out + "\"";
    };
    std::string cmd = "/usr/bin/osascript -e 'display notification " + quote(body) +
                      " with title " + quote(title) + "' >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0;
#else
    (void)title;
    (void)body;
    return false;
#endif
}

// Real hooks for the running daemon. Only den-managed homebrew packages in
// the active env are considered — Homebrew's own kegs and shared opt links
// are never touched (pours bind to exact kegs, see relocate.cpp / #64).
MaintenanceHooks make_live_hooks(const Config& config, PackageIndex& idx, bool& applied_any) {
    MaintenanceHooks h;
    h.refresh_index = [&config, &idx] {
        idx = fetch_and_transform(config);
        save_index(idx, config.cache / "index.json");
        return idx;
    };
    h.installed = [&config] {
        auto per = resolve_per_provider(config.den_home, active_env_path(config.den_home));
        auto it = per.find("homebrew");
        return it == per.end() ? std::map<std::string, std::string>{} : it->second;
    };
    h.installed_scheme = [&config](const std::string& name, const std::string& version) {
        return installed_version_scheme(config.store / keg_name(name) / version);
    };
    h.prefetch = [&config, &idx](const PendingUpgrade& pu) {
        if (const Package* pkg = idx.find(pu.name))
            prefetch_bottle(config, *pkg);
    };
    h.apply = [&config, &idx, &applied_any](const PendingUpgrade& pu) {
        auto registry = make_default_registry(&idx);
        auto* provider = registry.find("homebrew");
        if (!provider)
            throw UserError("homebrew provider not registered");
        apply_daemon_upgrade(config, *provider, idx, pu);
        applied_any = true;
    };
    h.trust_screen = live_trust_screen(config, idx);
    h.notify_held = [&config](const std::vector<std::string>& names) {
        if (!current_trust_policy(config).notify)
            return false;
        return post_desktop_notification("den", held_back_notification(names));
    };
    return h;
}

} // namespace

void run_daemon(const Config& config, bool once) {
    const fs::path& den_home = config.den_home;
    fs::create_directories(den_home);

    // ---- PID file + exclusive flock ----------------------------------------
    fs::path pid_file = pid_path(den_home);
    int pid_fd = ::open(pid_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (pid_fd < 0) {
        throw UserError("daemon: cannot open PID file " + pid_file.string());
    }
    if (::flock(pid_fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(pid_fd);
        throw UserError("daemon: another instance is already running (flock failed)");
    }

    // Write PID.
    std::string pid_str = std::to_string(::getpid()) + "\n";
    (void)::write(pid_fd, pid_str.c_str(), pid_str.size());
    // Keep pid_fd open (holds the lock) until we exit.

    // ---- SIGTERM handler ---------------------------------------------------
    struct sigaction sa {};
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGTERM, &sa, nullptr);

    // Nobody watches the daemon's installs: the unattended trust policy
    // applies to everything it does (trust/unattended.h).
    set_unattended(std::string("daemon"));

    SPDLOG_INFO("daemon: started (pid {})", ::getpid());
    log_daemon(den_home, std::string("daemon started (pid ") + std::to_string(::getpid()) +
                             ", den " + DEN_VERSION + ")");

    // ---- LaunchAgent PATH --------------------------------------------------
    // The same refresh `den self-update` does, so a daemon restarted by an
    // older den (which never refreshed it) still corrects its plist, and runs
    // this time with the corrected PATH rather than the stale one launchd
    // gave it.
    refresh_daemon_plist_at_startup(config);

    // ---- Main loop ---------------------------------------------------------
    while (!g_terminate) {
        DaemonSettings settings = read_daemon_settings(den_home);
        uint64_t interval =
            settings.interval_secs > 0 ? settings.interval_secs : DEFAULT_INTERVAL_SECS;

        SPDLOG_DEBUG("daemon: sleeping {}s", interval);

        // Sleep in 1-second ticks so we wake quickly on SIGTERM.
        for (uint64_t i = 0; !once && i < interval && !g_terminate; ++i) {
            ::sleep(1);
        }
        if (g_terminate)
            break;

        // Re-read settings after the sleep (they may have changed).
        settings = read_daemon_settings(den_home);

        SPDLOG_INFO("daemon: running maintenance cycle");
        log_daemon(den_home, "maintenance cycle start");

        DaemonState state = read_daemon_state(den_home);
        MaintenanceSettings ms{settings.auto_download, settings.auto_upgrade,
                               settings.upgrade_window};
        PackageIndex idx;
        bool applied_any = false;
        MaintenanceReport rep = run_maintenance_cycle(config.store, den_home, ms, state,
                                                      make_live_hooks(config, idx, applied_any));
        if (applied_any) {
            try {
                materialise(den_home, config.store, active_env_path(den_home), &idx);
            } catch (const std::exception& e) {
                state.errors.push_back(std::string("materialise: ") + e.what());
            }
        }
        SPDLOG_INFO("daemon: cycle done (pending {}, downloaded {}, skipped pinned {}, applied {}, "
                    "deferred {})",
                    rep.pending, rep.downloaded, rep.skipped_pinned, rep.applied, rep.deferred);

        write_daemon_state(den_home, state);
        log_daemon(den_home, "maintenance cycle complete");
        if (once)
            break;
    }

    SPDLOG_INFO("daemon: shutting down");
    log_daemon(den_home, "daemon shutting down");

    // Release lock and clean up PID file.
    ::flock(pid_fd, LOCK_UN);
    ::close(pid_fd);
    std::error_code ec;
    fs::remove(pid_file, ec);
}

void stop_daemon(const Config& config) {
    fs::path pid_file = pid_path(config.den_home);

    std::ifstream f(pid_file);
    if (!f.is_open()) {
        throw UserError("daemon: PID file not found — is the daemon running?");
    }

    pid_t pid = 0;
    f >> pid;
    if (pid <= 0) {
        throw UserError("daemon: invalid PID in " + pid_file.string());
    }

    if (::kill(pid, SIGTERM) != 0) {
        throw UserError("daemon: failed to signal process " + std::to_string(pid) +
                        " — it may have already exited");
    }
    SPDLOG_INFO("daemon: sent SIGTERM to pid {}", pid);
}

bool is_daemon_running(const Config& config) {
    fs::path pid_file = pid_path(config.den_home);

    int fd = ::open(pid_file.c_str(), O_WRONLY);
    if (fd < 0)
        return false; // No PID file → not running.

    // If we can acquire an exclusive lock, the daemon is not holding one.
    bool locked = (::flock(fd, LOCK_EX | LOCK_NB) == 0);
    if (locked)
        ::flock(fd, LOCK_UN);
    ::close(fd);

    return !locked; // Daemon is running iff we could NOT get the lock.
}

} // namespace den

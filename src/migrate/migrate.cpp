// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "migrate.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <unordered_set>

namespace den {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Return true if path looks like a version directory (starts with a digit).
bool looks_like_version(const std::string& name) {
    return !name.empty() && (std::isdigit(static_cast<unsigned char>(name[0])) || name[0] == 'v');
}

/// Run a command and capture combined stdout/stderr. Returns {exit_code, output}.
/// exit_code is -1 if the command could not be launched or did not exit
/// normally. Read-only by contract — callers must only pass query commands.
std::pair<int, std::string> run_capture(const std::string& cmd) {
    std::string output;
    std::array<char, 4096> buf{};
    FILE* pipe = ::popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!pipe) {
        return {-1, ""};
    }
    while (::fgets(buf.data(), buf.size(), pipe) != nullptr) {
        output += buf.data();
    }
    const int status = ::pclose(pipe);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
}

/// Load den's root manifest (JSON at den_home/manifests/ROOT.json).
nlohmann::json load_root_manifest(const fs::path& manifest_path) {
    nlohmann::json manifest;
    if (fs::is_regular_file(manifest_path)) {
        std::ifstream f(manifest_path);
        if (f) {
            try {
                manifest = nlohmann::json::parse(f);
            } catch (const nlohmann::json::exception& e) {
                SPDLOG_WARN("Could not parse existing root manifest: {}", e.what());
            }
        }
    }
    if (!manifest.contains("packages") || !manifest["packages"].is_object()) {
        manifest["packages"] = nlohmann::json::object();
    }
    if (!manifest.contains("auto") || !manifest["auto"].is_array()) {
        manifest["auto"] = nlohmann::json::array();
    }
    if (!manifest.contains("casks") || !manifest["casks"].is_object()) {
        manifest["casks"] = nlohmann::json::object();
    }
    if (!manifest.contains("taps") || !manifest["taps"].is_array()) {
        manifest["taps"] = nlohmann::json::array();
    }
    if (!manifest.contains("services") || !manifest["services"].is_array()) {
        manifest["services"] = nlohmann::json::array();
    }
    return manifest;
}

} // namespace

// ---------------------------------------------------------------------------
// Cellar scanning (formulae)
// ---------------------------------------------------------------------------

std::vector<HomebrewKeg> scan_homebrew_cellar(const fs::path& cellar) {
    std::vector<HomebrewKeg> kegs;

    if (!fs::is_directory(cellar)) {
        SPDLOG_DEBUG("Cellar not found or not a directory: {}", cellar.string());
        return kegs;
    }

    std::error_code ec;
    for (const auto& formula_entry : fs::directory_iterator(cellar, ec)) {
        if (!formula_entry.is_directory()) {
            continue;
        }
        const std::string name = formula_entry.path().filename().string();

        std::error_code vec;
        for (const auto& version_entry : fs::directory_iterator(formula_entry.path(), vec)) {
            if (!version_entry.is_directory()) {
                continue;
            }
            const std::string version = version_entry.path().filename().string();
            if (!looks_like_version(version)) {
                continue;
            }
            kegs.push_back({name, version, version_entry.path()});
        }
    }

    // Stable sort: by name, then version descending (newest first).
    std::sort(kegs.begin(), kegs.end(), [](const HomebrewKeg& a, const HomebrewKeg& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.version > b.version; // newest first within same name
    });

    return kegs;
}

std::optional<Tab> read_tab(const fs::path& keg_path) {
    const fs::path receipt = keg_path / "INSTALL_RECEIPT.json";
    if (!fs::is_regular_file(receipt)) {
        return std::nullopt;
    }

    std::ifstream f(receipt);
    if (!f) {
        return std::nullopt;
    }

    try {
        const auto j = nlohmann::json::parse(f);

        Tab tab;
        tab.installed_on_request = j.value("installed_on_request", true);

        if (j.contains("runtime_dependencies") && j["runtime_dependencies"].is_array()) {
            for (const auto& dep : j["runtime_dependencies"]) {
                if (dep.contains("full_name") && dep["full_name"].is_string()) {
                    tab.runtime_deps.push_back(dep["full_name"].get<std::string>());
                }
            }
        }

        return tab;
    } catch (const nlohmann::json::exception& e) {
        SPDLOG_WARN("Failed to parse INSTALL_RECEIPT.json at {}: {}", receipt.string(), e.what());
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// Caskroom scanning (casks)
// ---------------------------------------------------------------------------

std::vector<HomebrewCask> scan_homebrew_caskroom(const fs::path& caskroom) {
    std::vector<HomebrewCask> casks;

    if (!fs::is_directory(caskroom)) {
        SPDLOG_DEBUG("Caskroom not found or not a directory: {}", caskroom.string());
        return casks;
    }

    std::error_code ec;
    for (const auto& cask_entry : fs::directory_iterator(caskroom, ec)) {
        if (!cask_entry.is_directory()) {
            continue;
        }
        const std::string name = cask_entry.path().filename().string();
        // Skip dotfiles / hidden cask-token dirs defensively.
        if (!name.empty() && name[0] == '.') {
            continue;
        }

        std::error_code vec;
        for (const auto& version_entry : fs::directory_iterator(cask_entry.path(), vec)) {
            if (!version_entry.is_directory()) {
                continue;
            }
            const std::string version = version_entry.path().filename().string();
            // `.metadata` (and any other dotfile) is bookkeeping, not a version.
            if (version.empty() || version[0] == '.') {
                continue;
            }
            casks.push_back({name, version, version_entry.path()});
        }
    }

    std::sort(casks.begin(), casks.end(), [](const HomebrewCask& a, const HomebrewCask& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.version > b.version; // newest first within same name
    });

    return casks;
}

// ---------------------------------------------------------------------------
// Tap scanning
// ---------------------------------------------------------------------------

std::vector<HomebrewTap> scan_homebrew_taps(const fs::path& taps_root) {
    std::vector<HomebrewTap> taps;

    if (!fs::is_directory(taps_root)) {
        SPDLOG_DEBUG("Taps root not found or not a directory: {}", taps_root.string());
        return taps;
    }

    constexpr std::string_view kRepoPrefix = "homebrew-";

    std::error_code ec;
    for (const auto& user_entry : fs::directory_iterator(taps_root, ec)) {
        if (!user_entry.is_directory()) {
            continue;
        }
        const std::string user = user_entry.path().filename().string();
        if (!user.empty() && user[0] == '.') {
            continue;
        }

        std::error_code rec;
        for (const auto& repo_entry : fs::directory_iterator(user_entry.path(), rec)) {
            if (!repo_entry.is_directory()) {
                continue;
            }
            const std::string repo_dir = repo_entry.path().filename().string();
            // Homebrew lays taps out as <user>/homebrew-<repo>.
            if (repo_dir.rfind(kRepoPrefix, 0) != 0) {
                continue;
            }
            const std::string repo = repo_dir.substr(kRepoPrefix.size());
            if (repo.empty()) {
                continue;
            }
            taps.push_back({user + "/" + repo, repo_entry.path()});
        }
    }

    std::sort(taps.begin(), taps.end(),
              [](const HomebrewTap& a, const HomebrewTap& b) { return a.name < b.name; });

    return taps;
}

// ---------------------------------------------------------------------------
// Service enumeration
// ---------------------------------------------------------------------------

std::vector<HomebrewService> parse_brew_services_json(const std::string& json_text) {
    std::vector<HomebrewService> services;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_text);
    } catch (const nlohmann::json::exception& e) {
        SPDLOG_WARN("Failed to parse `brew services list --json` output: {}", e.what());
        return services;
    }

    if (!j.is_array()) {
        return services;
    }

    for (const auto& entry : j) {
        if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string()) {
            continue;
        }
        HomebrewService svc;
        svc.name = entry["name"].get<std::string>();
        svc.status = entry.value("status", std::string{});
        if (entry.contains("user") && entry["user"].is_string()) {
            svc.user = entry["user"].get<std::string>();
        }
        if (entry.contains("file") && entry["file"].is_string()) {
            svc.plist_path = entry["file"].get<std::string>();
        }
        svc.running = (svc.status == "started" || svc.status == "running");
        services.push_back(std::move(svc));
    }

    std::sort(services.begin(), services.end(),
              [](const HomebrewService& a, const HomebrewService& b) { return a.name < b.name; });

    return services;
}

std::vector<HomebrewService> scan_launchagent_plists(const fs::path& launch_agents_dir) {
    std::vector<HomebrewService> services;

    if (!fs::is_directory(launch_agents_dir)) {
        SPDLOG_DEBUG("LaunchAgents dir not found: {}", launch_agents_dir.string());
        return services;
    }

    // Homebrew names its plists `homebrew.mxcl.<name>.plist`.
    constexpr std::string_view kPrefix = "homebrew.mxcl.";
    constexpr std::string_view kSuffix = ".plist";

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(launch_agents_dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string fn = entry.path().filename().string();
        if (fn.rfind(kPrefix, 0) != 0 || fn.size() <= kPrefix.size() + kSuffix.size()) {
            continue;
        }
        if (fn.compare(fn.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
            continue;
        }
        const std::string name =
            fn.substr(kPrefix.size(), fn.size() - kPrefix.size() - kSuffix.size());
        if (name.empty()) {
            continue;
        }
        HomebrewService svc;
        svc.name = name;
        svc.status = "started"; // presence of a LaunchAgent plist implies loaded
        svc.plist_path = entry.path();
        svc.running = true;
        services.push_back(std::move(svc));
    }

    std::sort(services.begin(), services.end(),
              [](const HomebrewService& a, const HomebrewService& b) { return a.name < b.name; });

    return services;
}

// ---------------------------------------------------------------------------
// Shell integration (opt-in)
// ---------------------------------------------------------------------------

namespace {

/// Determine the user's shell profile path from $SHELL. Returns empty if it
/// cannot be determined.
fs::path detect_shell_profile() {
    const char* home_c = std::getenv("HOME");
    if (!home_c) {
        return {};
    }
    const fs::path home(home_c);

    std::string shell;
    if (const char* sh = std::getenv("SHELL")) {
        const std::string s(sh);
        const auto pos = s.rfind('/');
        shell = (pos != std::string::npos) ? s.substr(pos + 1) : s;
    }

    if (shell == "zsh") {
        return home / ".zshrc";
    }
    if (shell == "bash") {
        return home / ".bashrc";
    }
    if (shell == "fish") {
        return home / ".config" / "fish" / "config.fish";
    }
    // Default to bash profile.
    return home / ".profile";
}

/// Append `eval "$(den init)"` to the user's profile if not already present.
/// Returns the profile path on success (empty on failure). Idempotent: a
/// no-op if the line is already there.
fs::path integrate_shell() {
    const fs::path profile = detect_shell_profile();
    if (profile.empty()) {
        SPDLOG_WARN("Could not determine shell profile for integration");
        return {};
    }

    const std::string marker = "den init"; // recognise prior integration
    const std::string line = "eval \"$(den init)\"";

    // Read existing contents (if any) to check idempotency and to preserve.
    if (fs::is_regular_file(profile)) {
        std::ifstream in(profile);
        const std::string contents((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        if (contents.find(marker) != std::string::npos) {
            SPDLOG_DEBUG("Shell profile already integrates den: {}", profile.string());
            return profile;
        }
    }

    std::error_code ec;
    fs::create_directories(profile.parent_path(), ec);
    std::ofstream out(profile, std::ios::app);
    if (!out) {
        SPDLOG_ERROR("Failed to open shell profile for writing: {}", profile.string());
        return {};
    }
    out << "\n# Added by `den migrate --integrate-shell`\n" << line << "\n";
    return profile;
}

/// Acquire the list of services from the best available source.
std::vector<HomebrewService> gather_services(const MigrateOptions& opts) {
    if (opts.brew_services_json) {
        return parse_brew_services_json(*opts.brew_services_json);
    }
    if (!opts.query_services) {
        return {}; // hermetic by default; do not touch the host environment
    }
    // Real path: prefer `brew services list --json`, fall back to plists.
    auto [rc, out] = run_capture("brew services list --json");
    if (rc == 0 && !out.empty()) {
        auto svcs = parse_brew_services_json(out);
        if (!svcs.empty()) {
            return svcs;
        }
    }
    if (const char* home = std::getenv("HOME")) {
        return scan_launchagent_plists(fs::path(home) / "Library" / "LaunchAgents");
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// Migration
// ---------------------------------------------------------------------------

MigrationSummary migrate_from_homebrew(const Config& config, const std::vector<std::string>& names,
                                       const MigrateOptions& opts) {
    SPDLOG_INFO("Scanning Homebrew Cellar at {}", config.homebrew_cellar.string());

    const auto filter = std::unordered_set<std::string>(names.begin(), names.end());
    const bool filter_all = filter.empty();

    // --- Formulae ---
    auto kegs = scan_homebrew_cellar(config.homebrew_cellar);
    if (!filter_all) {
        kegs.erase(std::remove_if(
                       kegs.begin(), kegs.end(),
                       [&](const HomebrewKeg& k) { return filter.find(k.name) == filter.end(); }),
                   kegs.end());
    }

    // --- Casks ---
    auto casks = scan_homebrew_caskroom(config.homebrew_caskroom);
    if (!filter_all) {
        casks.erase(std::remove_if(
                        casks.begin(), casks.end(),
                        [&](const HomebrewCask& c) { return filter.find(c.name) == filter.end(); }),
                    casks.end());
    }

    // --- Taps & services (only when migrating everything) ---
    std::vector<HomebrewTap> taps;
    std::vector<HomebrewService> services;
    if (filter_all) {
        taps = scan_homebrew_taps(config.homebrew_taps);
        services = gather_services(opts);
    }

    SPDLOG_INFO("Found {} kegs, {} casks, {} taps, {} services", kegs.size(), casks.size(),
                taps.size(), services.size());

    // Load den's root manifest.
    const fs::path manifest_dir = config.den_home / "manifests";
    const fs::path manifest_path = manifest_dir / "ROOT.json";
    nlohmann::json manifest = load_root_manifest(manifest_path);

    auto& pkgs = manifest["packages"];
    auto& auto_pkgs = manifest["auto"];
    auto& cask_obj = manifest["casks"];
    auto& tap_arr = manifest["taps"];
    auto& svc_arr = manifest["services"];

    std::set<std::string> auto_set;
    for (const auto& a : auto_pkgs) {
        if (a.is_string()) {
            auto_set.insert(a.get<std::string>());
        }
    }
    std::set<std::string> tap_set;
    for (const auto& t : tap_arr) {
        if (t.is_string()) {
            tap_set.insert(t.get<std::string>());
        }
    }
    std::set<std::string> svc_set;
    for (const auto& s : svc_arr) {
        if (s.is_object() && s.contains("name") && s["name"].is_string()) {
            svc_set.insert(s["name"].get<std::string>());
        }
    }

    MigrationSummary summary;
    bool changed = false;

    // Formulae: kegs are sorted newest-first per name; take the first.
    {
        std::set<std::string> seen;
        for (const auto& keg : kegs) {
            if (seen.count(keg.name)) {
                continue;
            }
            seen.insert(keg.name);
            ++summary.formulae;

            if (pkgs.contains(keg.name)) {
                continue; // already tracked — never disturb existing state
            }
            const auto tab = read_tab(keg.path);
            const bool on_request = tab ? tab->installed_on_request : true;

            if (!opts.dry_run) {
                pkgs[keg.name] = keg.version;
                if (!on_request && auto_set.find(keg.name) == auto_set.end()) {
                    auto_pkgs.push_back(keg.name);
                    auto_set.insert(keg.name);
                }
            }
            changed = true;
            SPDLOG_DEBUG("Formula {}: {}{}", keg.name, keg.version, on_request ? "" : " [auto]");
        }
    }

    // Casks.
    {
        std::set<std::string> seen;
        for (const auto& cask : casks) {
            if (seen.count(cask.name)) {
                continue;
            }
            seen.insert(cask.name);
            ++summary.casks;

            if (cask_obj.contains(cask.name)) {
                continue;
            }
            if (!opts.dry_run) {
                cask_obj[cask.name] = cask.version;
            }
            changed = true;
            SPDLOG_DEBUG("Cask {}: {}", cask.name, cask.version);
        }
    }

    // Taps.
    for (const auto& tap : taps) {
        ++summary.taps;
        if (tap_set.count(tap.name)) {
            continue;
        }
        if (!opts.dry_run) {
            tap_arr.push_back(tap.name);
            tap_set.insert(tap.name);
        }
        changed = true;
        SPDLOG_DEBUG("Tap {}", tap.name);
    }

    // Services.
    for (const auto& svc : services) {
        ++summary.services;
        if (svc_set.count(svc.name)) {
            continue;
        }
        if (!opts.dry_run) {
            nlohmann::json s;
            s["name"] = svc.name;
            s["status"] = svc.status;
            s["running"] = svc.running;
            if (!svc.user.empty()) {
                s["user"] = svc.user;
            }
            if (!svc.plist_path.empty()) {
                s["plist"] = svc.plist_path.string();
            }
            svc_arr.push_back(std::move(s));
            svc_set.insert(svc.name);
        }
        changed = true;
        SPDLOG_DEBUG("Service {} ({})", svc.name, svc.status);
    }

    // Write manifest back (skip entirely in dry-run mode).
    if (!opts.dry_run && changed) {
        std::error_code ec;
        fs::create_directories(manifest_dir, ec);
        // Atomic write: temp file + rename, preserving any future attrs.
        const fs::path tmp = manifest_path.string() + ".tmp";
        {
            std::ofstream out(tmp);
            if (!out) {
                SPDLOG_ERROR("Failed to write root manifest to {}", tmp.string());
            } else {
                out << manifest.dump(2) << "\n";
            }
        }
        if (fs::exists(tmp)) {
            fs::rename(tmp, manifest_path, ec);
            if (ec) {
                SPDLOG_ERROR("Failed to install manifest {}: {}", manifest_path.string(),
                             ec.message());
            }
        }
    }

    // --- Summary output ---
    const char* prefix = opts.dry_run ? "==> [dry-run] Would migrate" : "==> Migration complete";
    std::cout << prefix << ": " << summary.formulae << " formulae, " << summary.casks << " casks, "
              << summary.taps << " taps, " << summary.services << " services\n";
    SPDLOG_INFO("Migration{}: {} formulae, {} casks, {} taps, {} services",
                opts.dry_run ? " (dry-run)" : "", summary.formulae, summary.casks, summary.taps,
                summary.services);

    // --- Shell integration (opt-in, never in dry-run) ---
    if (opts.integrate_shell && !opts.dry_run) {
        const fs::path profile = integrate_shell();
        if (!profile.empty()) {
            std::cout << "==> Added `eval \"$(den init)\"` to " << profile.string() << "\n";
            std::cout << "    To undo: remove that line from " << profile.string() << "\n";
        }
    }

    if (!opts.dry_run) {
        std::cout << "==> Homebrew was not modified; `brew` continues to work.\n";
        std::cout << "    To roll back den's migration, remove "
                  << (manifest_dir / "ROOT.json").string() << "\n";
    }

    return summary;
}

// ---------------------------------------------------------------------------
// Post-migration health check
// ---------------------------------------------------------------------------

HealthReport check_migration_health(const Config& config, bool print) {
    HealthReport report;

    const fs::path manifest_path = config.den_home / "manifests" / "ROOT.json";
    nlohmann::json manifest;
    if (fs::is_regular_file(manifest_path)) {
        std::ifstream f(manifest_path);
        try {
            manifest = nlohmann::json::parse(f);
        } catch (const nlohmann::json::exception& e) {
            report.issues.push_back({std::string("ROOT.json is malformed: ") + e.what(), true});
            if (print) {
                std::cout << "[error] ROOT.json is malformed: " << e.what() << "\n";
            }
            return report;
        }
    } else {
        report.issues.push_back({"no migration manifest found at " + manifest_path.string(), true});
        if (print) {
            std::cout << "[error] no migration manifest found at " << manifest_path.string()
                      << "\n";
        }
        return report;
    }

    auto add = [&](const std::string& msg, bool err) {
        report.issues.push_back({msg, err});
        if (print) {
            std::cout << (err ? "[error] " : "[warning] ") << msg << "\n";
        }
    };

    // Formulae: each manifest package must resolve to a keg directory.
    if (manifest.contains("packages") && manifest["packages"].is_object()) {
        for (const auto& [name, ver] : manifest["packages"].items()) {
            if (!ver.is_string()) {
                add("package '" + name + "' has a non-string version", true);
                continue;
            }
            const fs::path keg = config.homebrew_cellar / name / ver.get<std::string>();
            if (!fs::is_directory(keg)) {
                add("formula '" + name + "' does not resolve to a keg: " + keg.string(), true);
            } else {
                ++report.formulae_ok;
            }
        }
    }

    // Casks: each manifest cask must resolve to a Caskroom version directory.
    if (manifest.contains("casks") && manifest["casks"].is_object()) {
        for (const auto& [name, ver] : manifest["casks"].items()) {
            if (!ver.is_string()) {
                add("cask '" + name + "' has a non-string version", true);
                continue;
            }
            const fs::path dir = config.homebrew_caskroom / name / ver.get<std::string>();
            if (!fs::is_directory(dir)) {
                // Warn, not error — casks may be self-updating apps whose
                // on-disk version drifts from the recorded one.
                add("cask '" + name + "' not found in Caskroom: " + dir.string(), false);
            } else {
                ++report.casks_ok;
            }
        }
    }

    // Taps: count only (a tap is a name reference; den re-adds it lazily).
    if (manifest.contains("taps") && manifest["taps"].is_array()) {
        report.taps_ok = static_cast<int>(manifest["taps"].size());
    }

    // Services: recorded-running services should still have a plist on disk
    // (or be reported running by the OS). We only check the plist reference.
    if (manifest.contains("services") && manifest["services"].is_array()) {
        for (const auto& s : manifest["services"]) {
            if (!s.is_object() || !s.contains("name") || !s["name"].is_string()) {
                continue;
            }
            const std::string name = s["name"].get<std::string>();
            const bool running = s.value("running", false);
            if (running && s.contains("plist") && s["plist"].is_string()) {
                const fs::path plist = s["plist"].get<std::string>();
                if (!plist.empty() && !fs::exists(plist)) {
                    add("service '" + name + "' plist missing: " + plist.string(), false);
                    continue;
                }
            }
            ++report.services_ok;
        }
    }

    if (print) {
        std::cout << "==> Health: " << report.formulae_ok << " formulae, " << report.casks_ok
                  << " casks, " << report.taps_ok << " taps, " << report.services_ok
                  << " services OK";
        if (report.healthy()) {
            std::cout << " — all good\n";
        } else {
            std::cout << " — see errors above\n";
        }
    }

    return report;
}

} // namespace den

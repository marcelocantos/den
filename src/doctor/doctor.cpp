// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "doctor.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace den {

namespace {

// ---------------------------------------------------------------------------
// Helper: record a finding.
// ---------------------------------------------------------------------------
void warn(std::vector<Finding>& findings, std::string msg) {
    findings.push_back({Severity::Warning, std::move(msg)});
}
void error(std::vector<Finding>& findings, std::string msg) {
    findings.push_back({Severity::Error, std::move(msg)});
}

// ---------------------------------------------------------------------------
// Check 1: den_home directory structure
// ---------------------------------------------------------------------------
void check_den_home_structure(const Config& config, std::vector<Finding>& findings) {
    SPDLOG_DEBUG("check: den_home structure");

    if (!fs::exists(config.den_home)) {
        warn(findings, "den home directory does not exist: " +
                       config.den_home.string() +
                       " (run `den init` to create it)");
        return;
    }
    if (!fs::is_directory(config.den_home)) {
        error(findings, "den home path exists but is not a directory: " +
                        config.den_home.string());
        return;
    }

    // Expected subdirectories.
    for (const auto& sub : {"bin", "envs", "cache"}) {
        fs::path p = config.den_home / sub;
        if (!fs::exists(p)) {
            warn(findings, "expected directory missing: " + p.string());
        } else if (!fs::is_directory(p)) {
            error(findings, "expected directory but found file: " + p.string());
        }
    }
}

// ---------------------------------------------------------------------------
// Check 2: store directory exists and is a directory
// ---------------------------------------------------------------------------
void check_store(const Config& config, std::vector<Finding>& findings) {
    SPDLOG_DEBUG("check: store");

    if (!fs::exists(config.store)) {
        warn(findings, "package store not initialised: " +
                       config.store.string() +
                       " (no packages installed yet)");
    } else if (!fs::is_directory(config.store)) {
        error(findings, "store path exists but is not a directory: " +
                        config.store.string());
    }
}

// ---------------------------------------------------------------------------
// Check 3: manifest integrity
// Verify <den_home>/manifest.json is valid JSON (if it exists).
// ---------------------------------------------------------------------------
void check_manifest(const Config& config, std::vector<Finding>& findings) {
    SPDLOG_DEBUG("check: manifest integrity");

    fs::path manifest = config.den_home / "manifest.json";
    if (!fs::exists(manifest)) {
        // No manifest yet — not an error for a fresh install.
        SPDLOG_DEBUG("manifest.json absent, skipping");
        return;
    }

    std::ifstream f(manifest);
    if (!f) {
        error(findings, "cannot read manifest.json: " + manifest.string());
        return;
    }

    try {
        nlohmann::json j;
        f >> j;
        if (!j.is_object()) {
            error(findings, "manifest.json is not a JSON object: " +
                            manifest.string());
        }
    } catch (const nlohmann::json::parse_error& e) {
        error(findings, std::string("manifest.json parse error: ") + e.what() +
                        " (" + manifest.string() + ")");
    }
}

// ---------------------------------------------------------------------------
// Check 4: environment materialisation
// Each env directory under <den_home>/envs/ should have a bin/ subdirectory.
// ---------------------------------------------------------------------------
void check_env_materialisation(const Config& config,
                                std::vector<Finding>& findings) {
    SPDLOG_DEBUG("check: environment materialisation");

    fs::path envs_dir = config.den_home / "envs";
    if (!fs::exists(envs_dir)) return;

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(envs_dir, ec)) {
        if (!entry.is_directory()) continue;
        fs::path bin = entry.path() / "bin";
        if (!fs::exists(bin)) {
            warn(findings, "environment '" + entry.path().filename().string() +
                           "' has no bin/ directory (may be uninitialised)");
        }
    }
    if (ec) {
        warn(findings, "error iterating envs directory: " + ec.message());
    }
}

// ---------------------------------------------------------------------------
// Check 5: broken symlinks in env directories
// ---------------------------------------------------------------------------
void check_broken_symlinks(const Config& config, std::vector<Finding>& findings) {
    SPDLOG_DEBUG("check: broken symlinks");

    fs::path envs_dir = config.den_home / "envs";
    if (!fs::exists(envs_dir)) return;

    int broken = 0;
    std::error_code ec;
    for (const auto& entry :
         fs::recursive_directory_iterator(envs_dir,
             fs::directory_options::skip_permission_denied, ec)) {
        if (entry.is_symlink()) {
            std::error_code link_ec;
            bool exists = fs::exists(entry.path(), link_ec);
            if (!exists) {
                ++broken;
                SPDLOG_DEBUG("broken symlink: {}", entry.path().string());
                if (broken <= 10) {
                    warn(findings, "broken symlink: " + entry.path().string());
                }
            }
        }
    }
    if (broken > 10) {
        warn(findings, "... and " + std::to_string(broken - 10) +
                       " more broken symlinks");
    }
    if (ec) {
        warn(findings, "error scanning env symlinks: " + ec.message());
    }
}

// ---------------------------------------------------------------------------
// Check 6: missing packages
// Entries in the store that have no content (empty keg directory).
// ---------------------------------------------------------------------------
void check_missing_packages(const Config& config,
                             std::vector<Finding>& findings) {
    SPDLOG_DEBUG("check: missing packages");

    if (!fs::exists(config.store)) return;

    std::error_code ec;
    for (const auto& pkg_entry :
         fs::directory_iterator(config.store, ec)) {
        if (!pkg_entry.is_directory()) continue;
        // Each package dir contains version subdirs.
        std::error_code ver_ec;
        for (const auto& ver_entry :
             fs::directory_iterator(pkg_entry.path(), ver_ec)) {
            if (!ver_entry.is_directory()) continue;
            // A keg directory is "missing" if it is empty.
            bool empty = fs::is_empty(ver_entry.path(), ver_ec);
            if (!ver_ec && empty) {
                warn(findings, "empty keg (possibly failed install): " +
                               ver_entry.path().string());
            }
        }
        if (ver_ec) {
            warn(findings, "error reading keg versions for " +
                           pkg_entry.path().string() + ": " + ver_ec.message());
        }
    }
    if (ec) {
        warn(findings, "error reading package store: " + ec.message());
    }
}

// ---------------------------------------------------------------------------
// Check 7: file permissions on sensitive files
// config.json and any *.key / *.pem files should be mode 0600.
// ---------------------------------------------------------------------------
void check_file_permissions(const Config& config,
                             std::vector<Finding>& findings) {
    SPDLOG_DEBUG("check: file permissions");

    // Sensitive files to check.
    std::vector<fs::path> sensitive = {
        config.den_home / "config.json",
    };

    // Also scan for private key / pem files anywhere under den_home.
    std::error_code ec;
    for (const auto& entry :
         fs::recursive_directory_iterator(config.den_home,
             fs::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string fn = entry.path().filename().string();
        if (fn.size() >= 4 &&
            (fn.substr(fn.size() - 4) == ".key" ||
             fn.substr(fn.size() - 4) == ".pem")) {
            sensitive.push_back(entry.path());
        }
    }

    for (const auto& p : sensitive) {
        if (!fs::exists(p)) continue;
        struct stat st{};
        if (::stat(p.c_str(), &st) != 0) {
            warn(findings, "cannot stat " + p.string() + ": " +
                           std::strerror(errno));
            continue;
        }
        // Check that group and other have no read/write/exec bits.
        constexpr mode_t bad_bits = 077; // g+rwx o+rwx
        if (st.st_mode & bad_bits) {
            error(findings, "sensitive file has overly permissive mode " +
                            std::to_string(st.st_mode & 0777) + " (expected 0600): " +
                            p.string());
        }
    }
    if (ec) {
        warn(findings, "error scanning for sensitive files: " + ec.message());
    }
}

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

const char* severity_label(Severity s) {
    switch (s) {
    case Severity::Warning: return "warning";
    case Severity::Error:   return "error";
    }
    return "unknown";
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::vector<Finding> doctor(const Config& config) {
    std::vector<Finding> findings;

    check_den_home_structure(config, findings);
    check_store(config, findings);
    check_manifest(config, findings);
    check_env_materialisation(config, findings);
    check_broken_symlinks(config, findings);
    check_missing_packages(config, findings);
    check_file_permissions(config, findings);

    // Summary output.
    int warnings = 0;
    int errors   = 0;
    for (const auto& f : findings) {
        if (f.severity == Severity::Warning) ++warnings;
        else                                 ++errors;
        std::cout << "[" << severity_label(f.severity) << "] " << f.message << "\n";
    }

    if (findings.empty()) {
        std::cout << "All checks passed.\n";
    } else {
        std::cout << "\n"
                  << findings.size() << " finding(s): "
                  << warnings << " warning(s), "
                  << errors   << " error(s).\n";
    }

    return findings;
}

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "doctor.h"

#include "../build/linkage_scan.h"
#include "../build/macho_scan.h"
#include "../core/error.h"
#include "../env/environment.h"
#include "../env/manifest.h"
#include "../env/post_install.h"
#include "../index/bottle_cellar.h"
#include "../index/index.h"
#include "../platform/platform.h"
#include "../settings/settings.h"
#include "trust_checks.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <tuple>
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
        warn(findings, "den home directory does not exist: " + config.den_home.string() +
                           " (run `den init` to create it)");
        return;
    }
    if (!fs::is_directory(config.den_home)) {
        error(findings, "den home path exists but is not a directory: " + config.den_home.string());
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
        warn(findings, "Cellar does not exist: " + config.store.string() +
                           " (install Homebrew or create it manually)");
    } else if (!fs::is_directory(config.store)) {
        error(findings, "store path exists but is not a directory: " + config.store.string());
    }
    if (config.shares_homebrew_cellar()) {
        warn(findings, shared_prefix_hint());
    }
}

// ---------------------------------------------------------------------------
// Check 2b: bottle Cellars and Mach-O header space. A prefix longer than the
// standard one is only an informational note; it's a warning when the store
// already holds kegs whose bottles were built for a different fixed Cellar.
// ---------------------------------------------------------------------------
void check_prefix_header_space(const Config& config, std::vector<Finding>& findings,
                               std::vector<std::string>& notes) {
    SPDLOG_DEBUG("check: prefix header space");
#ifdef __APPLE__
    constexpr bool macho = true;
#else
    constexpr bool macho = false;
#endif
    auto note = prefix_header_space_note(config.prefix.string(),
                                         standard_bottle_prefix(config.arch), macho);

    std::vector<std::string> mismatched;
    const auto index_path = config.cache / "index.json";
    std::error_code ec;
    if (fs::is_directory(config.store, ec) && fs::exists(index_path, ec)) {
        try {
            auto idx = load_index(index_path);
            mismatched =
                kegs_with_mismatched_cellar(config.store, idx, config.arch, config.macos_version);
        } catch (const std::exception& e) {
            SPDLOG_DEBUG("bottle cellar check skipped: {}", e.what());
        }
    }

    if (mismatched.empty()) {
        if (!note.empty())
            notes.push_back(std::move(note));
        return;
    }
    std::string msg = std::to_string(mismatched.size()) + " keg(s) in " + config.store.string() +
                      " come from bottles built for a different fixed Cellar and likely don't "
                      "work (den no longer pours these):";
    for (const auto& k : mismatched)
        msg += "\n    " + k;
    msg += "\n  Reinstall them from source (den install --build-from-source <name>) or remove "
           "them.";
    if (!note.empty())
        msg += "\n  " + note;
    warn(findings, std::move(msg));
}

// ---------------------------------------------------------------------------
// Check 3: manifest integrity
// Validate every runtime manifest (`manifests/<slug>/manifest.json`) with the
// same reader `den list` uses. A fresh install with no manifests is fine.
// Leftover files at the historical wrong paths are reported, not parsed as
// if they were the live manifest.
// ---------------------------------------------------------------------------
void check_manifest(const Config& config, std::vector<Finding>& findings) {
    SPDLOG_DEBUG("check: manifest integrity");

    for (const fs::path& legacy :
         {config.den_home / "manifest.json", config.den_home / "manifests" / "ROOT.json"}) {
        std::error_code ec;
        if (fs::is_regular_file(legacy, ec)) {
            warn(findings, "ignored non-runtime manifest " + legacy.string() +
                               " (den reads manifests/<env>/manifest.json)");
        }
    }

    std::vector<std::string> envs;
    try {
        envs = list_all(config.den_home);
    } catch (const UserError& e) {
        error(findings, e.what());
        return;
    }

    for (const auto& env : envs) {
        try {
            (void)read_manifest(config.den_home, env);
        } catch (const UserError& e) {
            error(findings, e.what());
        }
    }
}

// ---------------------------------------------------------------------------
// Check 4: environment materialisation
// Each env directory under <den_home>/envs/ should have a bin/ subdirectory.
// ---------------------------------------------------------------------------
void check_env_materialisation(const Config& config, std::vector<Finding>& findings) {
    SPDLOG_DEBUG("check: environment materialisation");

    fs::path envs_dir = config.den_home / "envs";
    if (!fs::exists(envs_dir))
        return;

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(envs_dir, ec)) {
        if (!entry.is_directory())
            continue;
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
// Check 4b: root env layout. The root env is linked into the prefix; an
// install from before that change still has envs/ROOT/{bin,lib,...} as real
// directories until `den update` migrates it. With shell integration active,
// <prefix>/bin and <prefix>/sbin must also lead PATH.
// ---------------------------------------------------------------------------
void check_root_prefix_layout(const Config& config, std::vector<Finding>& findings) {
    SPDLOG_DEBUG("check: root env prefix layout");
    if (root_env_needs_migration(config.den_home)) {
        warn(findings, "the root environment still uses the old envs/ROOT link layout; run "
                       "`den update` to link it into " +
                           config.store.parent_path().string());
    }
    if (active_env_path(config.den_home) != "/" || !std::getenv("DEN_ENV")) {
        return;
    }
    const char* path = std::getenv("PATH");
    auto problem = root_path_order_problem(path ? path : "", config.store.parent_path(),
                                           config.den_home / "bin");
    if (!problem.empty()) {
        warn(findings, problem);
    }
}

// ---------------------------------------------------------------------------
// Check 5: broken symlinks in env directories
// ---------------------------------------------------------------------------
void check_broken_symlinks(const Config& config, std::vector<Finding>& findings) {
    SPDLOG_DEBUG("check: broken symlinks");

    fs::path envs_dir = config.den_home / "envs";
    if (!fs::exists(envs_dir))
        return;

    int broken = 0;
    try {
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(
                 envs_dir, fs::directory_options::skip_permission_denied, ec)) {
            std::error_code entry_ec;
            if (entry.is_symlink(entry_ec) && !entry_ec) {
                std::error_code link_ec;
                auto target_status = fs::status(entry.path(), link_ec);
                if (link_ec || target_status.type() == fs::file_type::not_found) {
                    ++broken;
                    SPDLOG_DEBUG("broken symlink: {}", entry.path().string());
                    if (broken <= 10) {
                        warn(findings, "broken symlink: " + entry.path().string());
                    }
                }
            }
        }
        if (broken > 10) {
            warn(findings, "... and " + std::to_string(broken - 10) + " more broken symlinks");
        }
        if (broken > 0) {
            warn(findings, broken_symlinks_hint(broken, config.shares_homebrew_cellar()));
        }
        if (ec) {
            warn(findings, "error scanning env symlinks: " + ec.message());
        }
    } catch (const fs::filesystem_error& e) {
        warn(findings, std::string("error scanning env symlinks: ") + e.what());
    }
}

// ---------------------------------------------------------------------------
// Check 6: missing packages
// Entries in the store that have no content (empty keg directory).
// ---------------------------------------------------------------------------
void check_missing_packages(const Config& config, std::vector<Finding>& findings) {
    SPDLOG_DEBUG("check: missing packages");

    if (!fs::exists(config.store))
        return;

    std::error_code ec;
    for (const auto& pkg_entry : fs::directory_iterator(config.store, ec)) {
        if (!pkg_entry.is_directory())
            continue;
        // Each package dir contains version subdirs.
        std::error_code ver_ec;
        for (const auto& ver_entry : fs::directory_iterator(pkg_entry.path(), ver_ec)) {
            if (!ver_entry.is_directory())
                continue;
            // A keg directory is "missing" if it is empty.
            bool empty = fs::is_empty(ver_entry.path(), ver_ec);
            if (!ver_ec && empty) {
                warn(findings, "empty keg (possibly failed install): " + ver_entry.path().string());
            }
        }
        if (ver_ec) {
            warn(findings, "error reading keg versions for " + pkg_entry.path().string() + ": " +
                               ver_ec.message());
        }
    }
    if (ec) {
        warn(findings, "error reading package store: " + ec.message());
    }
}

// ---------------------------------------------------------------------------
// Check: remaining brew dependencies
// Kegs den's own prefix still takes from Homebrew's: an [info] count and
// list, or a warning when any of them no longer resolves.
// ---------------------------------------------------------------------------
void check_brew_dependencies(const Config& config, std::vector<Finding>& findings,
                             std::vector<std::string>& notes) {
    SPDLOG_DEBUG("check: remaining brew dependencies");
    std::vector<BrewDependency> deps;
    try {
        deps = remaining_brew_dependencies(config);
    } catch (const std::exception& e) {
        SPDLOG_DEBUG("brew dependency check skipped: {}", e.what());
        return;
    }
    if (deps.empty()) {
        return;
    }
    auto text = format_brew_dependencies(deps);
    const auto broken =
        std::count_if(deps.begin(), deps.end(), [](const BrewDependency& d) { return d.broken; });
    if (broken == 0) {
        notes.push_back(std::move(text));
        return;
    }
    text += "\n  " + std::to_string(broken) + " of them no longer resolve in " +
            config.homebrew_prefix.string() +
            " (Homebrew removed or upgraded the keg, or is not installed); `brew install` it "
            "again for den's kegs that load it.";
    warn(findings, std::move(text));
}

// ---------------------------------------------------------------------------
// Check: keg linkage
// Pins that remain (an [info] list) and libraries den's kegs load through a
// missing or dangling <prefix>/opt/<dep> (a warning).
// ---------------------------------------------------------------------------
void check_keg_linkage(const Config& config, std::vector<Finding>& findings,
                       std::vector<std::string>& notes) {
    SPDLOG_DEBUG("check: keg linkage");
    std::error_code ec;
    if (!fs::is_directory(config.store, ec))
        return;
    const auto linkage = scan_keg_linkage(config);
    if (auto note = format_pinned_kegs(linkage.pinned, config.shares_homebrew_cellar());
        !note.empty())
        notes.push_back(std::move(note));
    if (linkage.broken_opt.empty())
        return;
    std::string text = std::to_string(linkage.broken_opt.size()) + " librar" +
                       (linkage.broken_opt.size() == 1 ? "y" : "ies") +
                       " den's kegs load through " + (config.prefix / "opt").string() +
                       " cannot be found:";
    for (const auto& line : linkage.broken_opt)
        text += "\n    " + line;
    text += "\n  Reinstall the dependency (den install <dep>) so den recreates its opt link.";
    warn(findings, std::move(text));
}

// ---------------------------------------------------------------------------
// Check 7: file permissions on sensitive files
// config.json and any *.key / *.pem files should be mode 0600.
// ---------------------------------------------------------------------------
void check_file_permissions(const Config& config, std::vector<Finding>& findings) {
    SPDLOG_DEBUG("check: file permissions");

    // Sensitive files to check.
    std::vector<fs::path> sensitive = {
        config.den_home / "config.json",
    };

    // Also scan for private key / pem files anywhere under den_home.
    try {
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(
                 config.den_home, fs::directory_options::skip_permission_denied, ec)) {
            std::error_code entry_ec;
            if (!entry.is_regular_file(entry_ec) || entry_ec)
                continue;
            std::string fn = entry.path().filename().string();
            if (fn.size() >= 4 &&
                (fn.substr(fn.size() - 4) == ".key" || fn.substr(fn.size() - 4) == ".pem")) {
                sensitive.push_back(entry.path());
            }
        }
    } catch (const fs::filesystem_error&) {
        // Ignore traversal errors (symlink loops, etc.).
    }

    for (const auto& p : sensitive) {
        if (!fs::exists(p))
            continue;
        struct stat st{};
        if (::stat(p.c_str(), &st) != 0) {
            warn(findings, "cannot stat " + p.string() + ": " + std::strerror(errno));
            continue;
        }
        auto problem = sensitive_file_mode_problem(p, st.st_mode);
        if (!problem.empty()) {
            error(findings, problem);
        }
    }
}

// ---------------------------------------------------------------------------
// Check 8: host toolchain (🎯T66/T2)
// On macOS, the source-build path needs the Command Line Tools and an SDK.
// Surface their absence as warnings. Output is deterministic (no timestamps).
// ---------------------------------------------------------------------------
void check_host_toolchain(const Config& config, std::vector<Finding>& findings) {
    SPDLOG_DEBUG("check: host toolchain");
    (void)config;

    auto facts = detect_host_facts();
#ifdef __APPLE__
    if (!facts.clt_version) {
        warn(findings, "Command Line Tools not detected "
                       "(run `xcode-select --install` to enable source builds)");
    }
    if (!facts.sdk_path) {
        warn(findings, "macOS SDK path not detected (no SDKROOT, xcode-select -p SDK, or Command "
                       "Line Tools SDK)");
    }
#else
    if (!facts.glibc) {
        warn(findings, "glibc version not detected (getconf GNU_LIBC_VERSION failed)");
    }
#endif
}

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

const char* severity_label(Severity s) {
    switch (s) {
    case Severity::Warning:
        return "warning";
    case Severity::Error:
        return "error";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Check: source-built kegs (receipt) and pending rebuilds
// ---------------------------------------------------------------------------

void check_source_builds(const Config& config, std::vector<Finding>& findings,
                         std::vector<std::string>& notes) {
    SPDLOG_DEBUG("check: source-built kegs");
    std::error_code ec;
    if (!fs::is_directory(config.store, ec))
        return;
    std::optional<PackageIndex> idx;
    const auto index_path = config.cache / "index.json";
    if (fs::exists(index_path, ec)) {
        try {
            idx = load_index(index_path);
        } catch (const std::exception& e) {
            SPDLOG_DEBUG("source-build rebuild check skipped: {}", e.what());
        }
    }
    const auto kegs = source_built_kegs(config.store, idx ? &*idx : nullptr);
    auto note = format_source_built_kegs(kegs);
    if (!note.empty())
        notes.push_back(std::move(note));
    std::string fallbacks;
    int fallback_count = 0;
    for (const auto& k : kegs) {
        if (k.receipt.fallback.empty())
            continue;
        ++fallback_count;
        fallbacks += "\n    " + k.name + "/" + k.version;
        if (!k.receipt.fallback_reason.empty())
            fallbacks += ": " + k.receipt.fallback_reason;
    }
    if (fallback_count > 0)
        warn(findings, std::to_string(fallback_count) +
                           " source-built keg(s) were built by den's native recipe parser after "
                           "their Ruby build failed (fallback=native):" +
                           fallbacks +
                           "\n  Rebuild them once the Ruby build works (den install "
                           "--build-from-source <name>). `den set build.strict true` makes "
                           "that fallback an error.");
    std::string pending;
    int count = 0;
    for (const auto& k : kegs) {
        if (k.pending_rebuild.empty())
            continue;
        ++count;
        pending += "\n    " + k.name + "/" + k.version + ": " + k.pending_rebuild;
    }
    if (count == 0)
        return;
    warn(findings, std::to_string(count) +
                       " source-built keg(s) were built from a source the current formula no "
                       "longer names:" +
                       pending + "\n  Rebuild them (den install --build-from-source <name>).");
}

} // namespace

std::vector<SourceBuiltKeg> source_built_kegs(const fs::path& store, const PackageIndex* idx) {
    std::vector<SourceBuiltKeg> out;
    for (const auto& keg : list_installed(store)) {
        auto receipt = read_source_build_receipt(keg.path);
        if (!receipt)
            continue;
        SourceBuiltKeg k{keg.name, keg.version, std::move(*receipt), {}};
        const Package* pkg = idx ? idx->find(keg.name) : nullptr;
        if (pkg && pkg->version == keg.version && pkg->source_sha256 &&
            !pkg->source_sha256->empty() && !k.receipt.source.sha256.empty() &&
            *pkg->source_sha256 != k.receipt.source.sha256)
            k.pending_rebuild = "built from sha256 " + k.receipt.source.sha256 +
                                ", the formula now names " + *pkg->source_sha256;
        out.push_back(std::move(k));
    }
    std::sort(out.begin(), out.end(), [](const SourceBuiltKeg& a, const SourceBuiltKeg& b) {
        return std::tie(a.name, a.version) < std::tie(b.name, b.version);
    });
    return out;
}

std::string format_source_built_kegs(const std::vector<SourceBuiltKeg>& kegs) {
    if (kegs.empty())
        return {};
    std::string text = "source-built kegs: " + std::to_string(kegs.size());
    for (const auto& k : kegs) {
        text += "\n    " + k.name + " " + k.version;
        const auto& f = k.receipt;
        if (!f.formula_tap.empty()) {
            text += " (" + f.formula_tap;
            if (!f.formula_commit.empty())
                text += "@" + f.formula_commit.substr(0, 12);
            text += ")";
        }
        if (!f.fallback.empty())
            text += " [fallback=" + f.fallback + "]";
        if (!k.pending_rebuild.empty())
            text += " [rebuild pending]";
    }
    return text;
}

KegLinkage scan_keg_linkage(const Config& config) {
    KegLinkage out;
    const auto opt_dir = config.store.parent_path() / "opt";
    std::set<std::string> broken;
    for (const auto& keg : list_installed(config.store)) {
        if (!keg_owned_by_den(keg.path))
            continue;
        const auto label = keg.name + " " + keg.version;
        std::map<std::string, std::pair<size_t, fs::path>> pins;
        for (const auto& p : keg_pinned_loads(keg.path, config.store, keg.name)) {
            auto& [count, file] = pins[p.dep + " " + p.version];
            if (count++ == 0)
                file = p.file.lexically_relative(keg.path);
        }
        for (const auto& [dep, cf] : pins)
            out.pinned.push_back(label + " -> " + dep + " (" + std::to_string(cf.first) +
                                 " load path" + (cf.first == 1 ? "" : "s") + ", e.g. " +
                                 cf.second.string() + ")");
        std::set<std::string> reported_links;
        for (const auto& load : keg_opt_loads(keg.path, opt_dir)) {
            if (load.dep == keg.name)
                continue;
            const auto link = opt_dir / load.dep;
            std::error_code lec;
            const auto st = fs::symlink_status(link, lec);
            std::string why;
            if (st.type() == fs::file_type::not_found)
                why = link.string() + " is missing";
            else if (!fs::exists(link, lec))
                why = link.string() + " is dangling";
            else if (load.exact && !fs::exists(link / load.tail, lec))
                why = link.string() + " has no " + load.tail.string();
            if (why.empty())
                continue;
            // One line per missing link and keg; every missing file.
            if (!load.exact && !reported_links.insert(load.dep).second)
                continue;
            broken.insert(label + ": " + load.file.lexically_relative(keg.path).string() +
                          " loads " + load.load + ": " + why);
        }
    }
    std::sort(out.pinned.begin(), out.pinned.end());
    out.broken_opt.assign(broken.begin(), broken.end());
    return out;
}

std::string format_pinned_kegs(const std::vector<std::string>& pinned, bool shared) {
    if (pinned.empty())
        return {};
    std::string text = "kegs pinned to versioned dependency kegs: " + std::to_string(pinned.size());
    if (shared)
        return text + " (expected: den pins dependencies while it shares Homebrew's prefix, "
                      "which can repoint any opt link)";
    text += " (they keep loading that version after it upgrades; `den update` relinks them "
            "through <prefix>/opt where that link is den's and names the same version, and "
            "reinstalling binds the rest)";
    for (const auto& p : pinned)
        text += "\n    " + p;
    return text;
}

std::string shared_prefix_hint() {
    return "den is still installing into Homebrew's folder, so Homebrew upgrades can break "
           "your den environments. Run `den migrate-prefix` to see what moving would do. It "
           "changes nothing until you add --apply.";
}

std::string broken_symlinks_hint(int broken, bool shared) {
    std::string out = std::to_string(broken) + " broken symlink" + (broken == 1 ? "" : "s") +
                      " in den environments.";
    if (shared) {
        out += " When Homebrew upgrades or removes kegs in the folder den shares with it, "
               "links into the old kegs break; `den migrate-prefix` gives den its own folder.";
    }
    return out + " den doctor only reports these links; it does not repair them.";
}

std::string standard_bottle_prefix(Arch arch) {
#ifdef __APPLE__
    return arch == Arch::Arm64 ? "/opt/homebrew" : "/usr/local";
#else
    (void)arch;
    return "/home/linuxbrew/.linuxbrew";
#endif
}

std::string prefix_header_space_note(const std::string& prefix, const std::string& standard,
                                     bool macho) {
    if (!macho || prefix.size() <= standard.size())
        return {};
    return "prefix " + prefix + " is " + std::to_string(prefix.size()) +
           " characters, longer than " + standard + " (" + std::to_string(standard.size()) +
           ") that bottles are built for. Relocatable bottles pour here as they do with "
           "Homebrew; den refuses fixed-cellar bottles, and an install fails cleanly if a "
           "relocated Mach-O load command doesn't fit in the binary's header space.";
}

std::vector<std::string> kegs_with_mismatched_cellar(const fs::path& store, const PackageIndex& idx,
                                                     Arch arch,
                                                     const std::optional<MacOsVersion>& macos) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& formula : fs::directory_iterator(store, ec)) {
        if (!formula.is_directory(ec))
            continue;
        const auto name = formula.path().filename().string();
        const auto* pkg = idx.find(name);
        if (!pkg || pkg->archives.empty())
            continue;
        std::vector<std::string> tags;
        for (const auto& [tag, _] : pkg->archives)
            tags.push_back(tag);
        auto best = best_archive_tag(arch, macos, tags);
        if (!best)
            continue;
        const auto& archive = pkg->archives.at(*best);
        if (bottle_pours_into(*best, archive, store))
            continue;
        const auto required = required_bottle_cellar(*best, archive);
        std::error_code kec;
        for (const auto& keg : fs::directory_iterator(formula.path(), kec)) {
            const auto ver = keg.path().filename().string();
            if (ver.empty() || ver[0] == '.' || !keg.is_directory(kec))
                continue;
            out.push_back(name + "/" + ver + " (bottle Cellar " + required + ")");
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace {

bool lexically_within(const fs::path& p, const fs::path& root) {
    const auto rel = p.lexically_normal().lexically_relative(root.lexically_normal());
    return !rel.empty() && rel.native().rfind("..", 0) != 0;
}

// ---------------------------------------------------------------------------
// openssl cert.pem links whose CA bundle is missing
// ---------------------------------------------------------------------------
void check_cert_pem_links(const Config& config, std::vector<Finding>& findings) {
    SPDLOG_DEBUG("check: cert.pem links");
    for (const auto& link : dangling_cert_pem_links(config.store.parent_path())) {
        warn(findings, "dangling CA bundle link " + link +
                           ": TLS through that keg has no trust store. Install ca-certificates "
                           "(`den install ca-certificates`), which writes the bundle; if it is "
                           "installed, the next `den install` or `den upgrade` restores it");
    }
}

} // namespace

std::vector<BrewDependency> remaining_brew_dependencies(const Config& config) {
    std::vector<BrewDependency> out;
    const auto& brew = config.homebrew_prefix;
    if (brew.empty() || config.shares_homebrew_cellar() ||
        config.prefix.lexically_normal() == brew.lexically_normal()) {
        return out;
    }
    std::error_code ec;
    const auto brew_canon = fs::weakly_canonical(brew, ec);
    auto into_brew = [&](const fs::path& p) {
        if (lexically_within(p, brew)) {
            return true;
        }
        std::error_code cec;
        auto canon = fs::weakly_canonical(p, cec);
        return !cec && !brew_canon.empty() && lexically_within(canon, brew_canon);
    };

    // Manifest kegs den left with brew.
    std::set<std::string> names;
    for (const auto& env : list_all(config.den_home)) {
        auto m = read_manifest(config.den_home, env);
        for (const auto& [entry, _] : m.packages["homebrew"]) {
            names.insert(fs::path(entry).filename().string());
        }
        for (const auto& entry : m.auto_deps["homebrew"]) {
            names.insert(fs::path(entry).filename().string());
        }
    }
    for (const auto& name : names) {
        const auto opt = config.prefix / "opt" / name;
        std::error_code lec;
        if (!fs::is_symlink(fs::symlink_status(opt, lec))) {
            continue;
        }
        auto target = fs::read_symlink(opt, lec);
        if (lec) {
            continue;
        }
        if (target.is_relative()) {
            target = opt.parent_path() / target;
        }
        if (!into_brew(target)) {
            continue;
        }
        std::error_code cec;
        const auto keg = fs::canonical(opt, cec);
        BrewDependency d;
        d.keg = name + (cec ? "" : " " + keg.filename().string());
        d.detail =
            "stays with brew (" + opt.string() + " -> " + target.lexically_normal().string() + ")";
        d.broken = static_cast<bool>(cec);
        out.push_back(std::move(d));
    }

    // Kegs in den's store that load from Homebrew's prefix.
    for (const auto& pkg : fs::directory_iterator(config.store, ec)) {
        std::error_code vec;
        if (!pkg.is_directory(vec)) {
            continue;
        }
        std::vector<fs::path> kegs;
        for (const auto& ver : fs::directory_iterator(pkg.path(), vec)) {
            std::error_code dec;
            if (ver.is_directory(dec) && !fs::is_symlink(ver.symlink_status(dec))) {
                kegs.push_back(ver.path());
            }
        }
        std::sort(kegs.begin(), kegs.end());
        for (const auto& keg : kegs) {
            auto refs = keg_brew_links(keg, brew);
            if (refs.empty()) {
                continue;
            }
            std::string what;
            for (const auto& f : brew_link_formulae(refs)) {
                what += (what.empty() ? "" : ", ") + f;
            }
            BrewDependency d;
            d.keg = pkg.path().filename().string() + "/" + keg.filename().string();
            d.detail = "loads " + what + " from " + brew.string();
            d.broken = std::any_of(refs.begin(), refs.end(), [](const BrewLinkRef& r) {
                std::error_code xec;
                return !fs::exists(r.brew_path, xec);
            });
            out.push_back(std::move(d));
        }
    }
    std::sort(out.begin() + 0, out.end(),
              [](const BrewDependency& a, const BrewDependency& b) { return a.keg < b.keg; });
    return out;
}

std::string format_brew_dependencies(const std::vector<BrewDependency>& deps) {
    std::string s = "remaining brew dependencies: " + std::to_string(deps.size());
    for (const auto& d : deps) {
        s += "\n    " + d.keg + ": " + d.detail + (d.broken ? " [missing]" : "");
    }
    return s;
}

std::string root_path_order_problem(const std::string& path_env, const fs::path& prefix,
                                    const fs::path& den_bin) {
    const auto bin = (prefix / "bin").lexically_normal();
    const auto sbin = (prefix / "sbin").lexically_normal();
    std::vector<fs::path> lead;
    std::stringstream ss(path_env);
    std::string entry;
    while (std::getline(ss, entry, ':') && lead.size() < 2) {
        if (entry.empty()) {
            continue;
        }
        auto p = fs::path(entry).lexically_normal();
        if (p == den_bin.lexically_normal()) {
            continue;
        }
        lead.push_back(p);
    }
    if (lead.size() == 2 && lead[0] == bin && lead[1] == sbin) {
        return {};
    }
    return "PATH should start with " + bin.string() + ":" + sbin.string() +
           " (the root environment is linked into the prefix); run `eval \"$(den shell-env)\"` "
           "or restart your shell";
}

std::string format_file_mode(unsigned mode) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04o", mode & 07777u);
    return buf;
}

std::string sensitive_file_mode_problem(const fs::path& p, unsigned mode) {
    // Group and other must have no read/write/exec bits.
    constexpr unsigned bad_bits = 077; // g+rwx o+rwx
    if ((mode & bad_bits) == 0)
        return {};
    return "sensitive file has overly permissive mode " + format_file_mode(mode & 0777u) +
           " (expected " + format_file_mode(kConfigFileMode) + "): " + p.string();
}

std::vector<Finding> doctor(const Config& config) {
    std::vector<Finding> findings;
    std::vector<std::string> notes;

    check_den_home_structure(config, findings);
    check_store(config, findings);
    check_prefix_header_space(config, findings, notes);
    check_manifest(config, findings);
    check_env_materialisation(config, findings);
    check_root_prefix_layout(config, findings);
    check_broken_symlinks(config, findings);
    check_cert_pem_links(config, findings);
    check_missing_packages(config, findings);
    check_brew_dependencies(config, findings, notes);
    check_keg_linkage(config, findings, notes);
    check_source_builds(config, findings, notes);
    check_file_permissions(config, findings);
    check_host_toolchain(config, findings);
    check_trust_model(config, findings);

    // Informational notes aren't findings: they don't count or affect the
    // exit status.
    for (const auto& n : notes)
        std::cout << "[info] " << n << "\n";

    // Summary output.
    int warnings = 0;
    int errors = 0;
    for (const auto& f : findings) {
        if (f.severity == Severity::Warning)
            ++warnings;
        else
            ++errors;
        std::cout << "[" << severity_label(f.severity) << "] " << f.message << "\n";
    }

    if (findings.empty()) {
        std::cout << "All checks passed.\n";
    } else {
        std::cout << "\n"
                  << findings.size() << " finding(s): " << warnings << " warning(s), " << errors
                  << " error(s).\n";
    }

    return findings;
}

} // namespace den

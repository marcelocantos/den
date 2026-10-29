// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "source_build.h"
#include "build_sandbox.h"
#include "formula_parser.h"
#include "macho_fixup.h"
#include "opt_prefix_build.h"

#include "../core/error.h"
#include "../download/archive.h"
#include "../download/http.h"
#include "../download/sha256.h"
#include "../platform/platform.h"
#include "../provider/exec.h"
#include "../ruby/bundle.h"
#include "../ruby/ruby_env.h"
#include "../settings/settings.h"
#include "../store/link.h"
#include "../store/store.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <unistd.h>

namespace den {

namespace {

std::pair<int, std::string> run(const std::string& cmd) {
    std::string output;
    std::array<char, 4096> buf{};
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe)
        return {-1, "popen failed"};
    while (fgets(buf.data(), buf.size(), pipe))
        output += buf.data();
    int status = pclose(pipe);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
}

// den_build.rb aborts with "error: SHA256 mismatch for <name> source
// tarball" plus both digests. That is a verdict, not a failed build:
// falling back would fetch the same tarball, fail the same check and report
// the mismatch a second time. Returns the message (from "SHA256 mismatch")
// when the Ruby output carries one. Its refusal of a resource or patch
// without a sha256 is a verdict too.
std::optional<std::string> ruby_sha_mismatch(const std::string& output) {
    for (auto at = output.find("no sha256 for "); at != std::string::npos;
         at = output.find("no sha256 for ", at + 1)) {
        auto line = output.substr(at, output.find('\n', at) - at);
        if (line.find("refusing to build with an unverified") != std::string::npos) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            return line;
        }
    }
    auto pos = output.find("SHA256 mismatch for ");
    if (pos == std::string::npos)
        return std::nullopt;
    auto msg = output.substr(pos);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' '))
        msg.pop_back();
    return msg;
}

// Single-quote a string for safe use as one shell word. Wraps in '...' and
// escapes embedded single quotes as '\''. Values like "-isysroot /path" (with
// spaces) and paths with shell metacharacters must be quoted or the shell would
// word-split them — e.g. an env value "-isysroot /sdk" would otherwise run /sdk
// as a command.
std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

int run_in_dir(const fs::path& dir, const std::string& cmd,
               const std::map<std::string, std::string>& env = {}) {
    std::string env_str;
    for (const auto& [k, v] : env)
        env_str += k + "=" + shell_quote(v) + " ";
    std::string full_cmd = "cd " + shell_quote(dir.string()) + " && " + env_str + cmd;
    SPDLOG_INFO("build: {}", full_cmd);
    int rc = std::system(full_cmd.c_str());
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

std::string detect_build_system(const fs::path& src_dir) {
    if (fs::exists(src_dir / "CMakeLists.txt"))
        return "cmake";
    if (fs::exists(src_dir / "meson.build"))
        return "meson";
    if (fs::exists(src_dir / "configure"))
        return "autotools";
    if (fs::exists(src_dir / "Makefile") || fs::exists(src_dir / "makefile") ||
        fs::exists(src_dir / "GNUmakefile"))
        return "make";
    if (fs::exists(src_dir / "configure.ac") || fs::exists(src_dir / "configure.in"))
        return "autogen";
    return "unknown";
}

BuildRecipe parse_formula_source(const std::string& formula_output) {
    BuildRecipe recipe;
    std::smatch match;

    std::regex url_re(R"RE(url\s+"([^"]+)")RE");
    if (std::regex_search(formula_output, match, url_re))
        recipe.source_url = match[1].str();

    std::regex sha_re(R"RE(sha256\s+"([0-9a-f]{64})")RE");
    if (std::regex_search(formula_output, match, sha_re))
        recipe.source_sha256 = match[1].str();

    if (formula_output.find(R"(system "cmake")") != std::string::npos)
        recipe.build_system = "cmake";
    else if (formula_output.find(R"(system "meson")") != std::string::npos)
        recipe.build_system = "meson";
    else if (formula_output.find(R"(system "./configure")") != std::string::npos)
        recipe.build_system = "autotools";
    else if (formula_output.find(R"(system "make")") != std::string::npos)
        recipe.build_system = "make";

    return recipe;
}

// 🎯T45 — Shim-free toolchain environment for macOS source builds.
//
// Homebrew historically routes the compiler through an `xcrun` shim, which nags
// the user to update the Command Line Tools and breaks reproducibility. den
// instead resolves the SDK path without xcrun (macos_sdk_path) and points the
// linker at /usr/bin/ld, so no xcrun invocation is ever constructed.
//
// On Linux this is a no-op: the system toolchain needs no SDK steering.
void apply_shim_free_toolchain(std::map<std::string, std::string>& env) {
#ifdef __APPLE__
    const auto sdk_path = macos_sdk_path();
    if (sdk_path) {
        // SDKROOT is honoured by clang/ld directly without xcrun. -isysroot
        // covers build systems (autotools, plain make) that read CFLAGS but
        // ignore SDKROOT.
        env["SDKROOT"] = *sdk_path;
        // Homebrew's superenv reads HOMEBREW_SDKROOT; setting it keeps the Ruby
        // side from shelling out to xcrun to find the SDK.
        env["HOMEBREW_SDKROOT"] = *sdk_path;
        const std::string isysroot = "-isysroot " + *sdk_path;
        for (const char* var : {"CFLAGS", "CXXFLAGS", "OBJCFLAGS"}) {
            if (env.count(var))
                env[var] += " " + isysroot;
            else
                env[var] = isysroot;
        }
    } else {
        SPDLOG_DEBUG("shim-free build: no SDK path from host facts; relying on compiler defaults");
    }
    // Resolve the system linker directly. /usr/bin/ld is the stable toolchain
    // entry point; this avoids `xcrun -f ld` while staying on the active CLT.
    std::error_code ec;
    if (fs::is_regular_file("/usr/bin/ld", ec)) {
        env["LD"] = "/usr/bin/ld";
    }
#else
    (void)env;
#endif
}

// The receipt's toolchain: the SDK den steers builds to and the compiler.
void record_toolchain(SourceBuildReceipt& receipt) {
#ifdef __APPLE__
    if (auto sdk = macos_sdk_path())
        receipt.toolchain_sdk = *sdk;
    const std::vector<std::string> cc = {"/usr/bin/clang", "--version"};
#else
    const std::vector<std::string> cc = {"cc", "--version"};
#endif
    const auto r = run_tool(cc);
    if (r.spawned && r.exit_code == 0)
        receipt.toolchain_clang = r.output.substr(0, r.output.find('\n'));
}

} // namespace

std::string formula_source_url(const Package& pkg) {
    const std::string ref = pkg.tap_git_head ? *pkg.tap_git_head : "master";
    return "https://raw.githubusercontent.com/Homebrew/homebrew-core/" + ref + "/" +
           pkg.ruby_source_path.value_or("");
}

std::string fetch_formula_source(const PackageIndex& idx, const std::string& name,
                                 FormulaOrigin* origin) {
    const auto* pkg = idx.find(name);
    if (!pkg || !pkg->ruby_source_path) {
        SPDLOG_DEBUG("no ruby_source_path for '{}' in index", name);
        return "";
    }

    // Tap formulae (🎯T67): ruby_source_path is an absolute path to a local .rb
    // file that merge_taps_into_index materialised. Read it directly — no
    // network, no Homebrew. The check is a filesystem stat, so a relative
    // homebrew-core path ("Formula/t/tree.rb") naturally falls through to the
    // GitHub fetch below.
    {
        std::error_code ec;
        if (fs::is_regular_file(*pkg->ruby_source_path, ec)) {
            std::ifstream f(*pkg->ruby_source_path);
            if (f) {
                std::string source((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
                SPDLOG_INFO("read tap formula source for '{}' from {} ({} bytes)", name,
                            *pkg->ruby_source_path, source.size());
                if (origin) {
                    const auto slash = name.rfind('/');
                    *origin = {slash == std::string::npos ? std::string() : name.substr(0, slash),
                               "", *pkg->ruby_source_path};
                }
                return source;
            }
        }
    }

    const auto url = formula_source_url(*pkg);
    if (!pkg->tap_git_head)
        SPDLOG_WARN("index has no tap_git_head for '{}'; fetching its formula from master", name);
    std::string source;
    try {
        source = fetch_url(url);
    } catch (const std::exception& e) {
        SPDLOG_WARN("failed to fetch formula source for '{}': {}", name, e.what());
        return "";
    }
    if (pkg->ruby_source_sha256) {
        const auto actual = hash_string(source);
        if (actual != *pkg->ruby_source_sha256)
            throw UserError("SHA256 mismatch for " + name +
                            " formula source\n  expected: " + *pkg->ruby_source_sha256 +
                            "\n  actual:   " + actual + "\n  url:      " + url);
    }
    SPDLOG_INFO("fetched formula source for '{}' from {} ({} bytes{})", name, url, source.size(),
                pkg->ruby_source_sha256 ? ", sha256 verified" : "");
    if (origin)
        *origin = {"homebrew/core", pkg->tap_git_head.value_or(""), *pkg->ruby_source_path};
    return source;
}

void parse_ruby_build_inputs(const std::string& output, SourceBuildReceipt& receipt) {
    std::istringstream in(output);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const auto key = line.substr(0, eq);
        std::vector<std::string> f;
        std::istringstream fields(line.substr(eq + 1));
        for (std::string v; std::getline(fields, v, '\t');)
            f.push_back(v);
        if (key == "source" && f.size() == 2)
            receipt.source = {"", f[0], f[1]};
        else if (key == "resource" && f.size() == 3)
            receipt.resources.push_back({f[0], f[1], f[2]});
        else if (key == "patch" && f.size() == 2)
            receipt.patches.push_back({"", f[0], f[1]});
    }
}

BuildRecipe extract_build_recipe(const PackageIndex& idx, const std::string& name) {
    auto source = fetch_formula_source(idx, name);
    if (!source.empty()) {
        return parse_formula_source(source);
    }

    // Fallback: brew info JSON for source URL/checksum. argv-based (no shell)
    // so a metacharacter name cannot inject commands even if validation regresses.
    auto brew = run_tool({"brew", "info", "--json=v1", name});
    if (!brew.spawned || brew.exit_code != 0)
        throw UserError("cannot find formula for '" + name + "'");
    BuildRecipe recipe;
    std::smatch match;
    std::regex url_re(R"RE("url"\s*:\s*"([^"]+)")RE");
    std::regex sha_re(R"RE("checksum"\s*:\s*"([0-9a-f]{64})")RE");
    if (std::regex_search(brew.output, match, url_re))
        recipe.source_url = match[1].str();
    if (std::regex_search(brew.output, match, sha_re))
        recipe.source_sha256 = match[1].str();
    return recipe;
}

std::string bundled_ruby_command(const Config& config, const fs::path& ruby_dir,
                                 const fs::path& formula_file, const std::string& name,
                                 const std::string& version) {
    const char* path = std::getenv("PATH");
    auto env = bundled_ruby_env(config, ruby_dir, read_env_snapshot(ruby_dir / "homebrew_env.txt"),
                                path ? path : "/usr/bin:/bin:/usr/sbin:/sbin");
    std::string cmd;
    for (const auto& [k, v] : env)
        cmd += k + "=" + shell_quote(v) + " ";
    auto ruby_bin = ruby_dir / "ruby" / "bin" / "ruby";
    auto homebrew_lib = ruby_dir / "homebrew";
    auto sorbet_lib = ruby_dir / "gems" / "sorbet-runtime";
    auto ruby_lib = ruby_dir / "ruby" / "lib" / "ruby" / "4.0.0";
    cmd += shell_quote(ruby_bin.string()) + " -I" + shell_quote(homebrew_lib.string()) + " -I" +
           shell_quote(sorbet_lib.string()) + " -I" + shell_quote(ruby_lib.string()) + " " +
           shell_quote((ruby_dir / "den_build.rb").string()) + " " +
           shell_quote(formula_file.string()) + " " + shell_quote(config.store.string()) + " " +
           shell_quote(config.den_home.string()) + " " + shell_quote(version) + " " +
           shell_quote(name);
    return cmd;
}

fs::path find_brew_on_path(const std::string& path) {
    std::istringstream in(path);
    for (std::string dir; std::getline(in, dir, ':');) {
        if (dir.empty())
            continue;
        auto candidate = fs::path(dir) / "brew";
        std::error_code ec;
        if (fs::exists(candidate, ec) && ::access(candidate.c_str(), X_OK) == 0)
            return candidate;
    }
    return {};
}

bool brew_serves_prefix(const fs::path& brew, const fs::path& prefix) {
    if (brew.empty() || prefix.empty())
        return false;
    std::error_code ec;
    auto real = fs::weakly_canonical(brew, ec);
    if (ec)
        return false;
    auto want = fs::weakly_canonical(prefix, ec);
    if (ec)
        return false;
    // <prefix>/bin/brew -> <prefix>/Library/.../bin/brew or
    // <prefix>/Homebrew/bin/brew (Linux/Intel repository layouts).
    for (auto p = real.parent_path(); !p.empty() && p != p.root_path(); p = p.parent_path()) {
        if (p == want)
            return true;
    }
    return false;
}

namespace {

// ": <last non-empty line>" of a build's output (at most 200 characters), or
// empty: enough to say why a Ruby build failed without its whole log.
std::string last_output_line(const std::string& output) {
    auto end = output.find_last_not_of(" \t\r\n");
    if (end == std::string::npos)
        return {};
    auto begin = output.rfind('\n', end);
    begin = begin == std::string::npos ? 0 : begin + 1;
    auto line = output.substr(begin, end - begin + 1);
    if (line.size() > 200)
        line = line.substr(0, 197) + "...";
    return ": " + line;
}

} // namespace

bool bakes_fixed_prefix(const PackageIndex& idx, const std::string& requested) {
    const Package* pkg = idx.find(requested);
    if (!pkg)
        return false;
    for (const auto& [tag, archive] : pkg->archives) {
        if (archive.relocation == RelocationType::HardcodedPrefix)
            return true;
    }
    return false;
}

void allow_native_fallback(const fs::path& den_home, const std::string& name,
                           const std::string& version, const std::string& why,
                           SourceBuildReceipt& receipt) {
    if (read_settings(den_home).build.strict)
        throw UserError(why + "\nbuild.strict is set, so den will not build " + name + " " +
                        version +
                        " with its native recipe parser instead. Fix the Ruby build, or run "
                        "`den set build.strict false` to allow the fallback.");
    std::cerr << "Warning: " << why << "\n"
              << "Warning: building " << name << " " << version
              << " with den's native recipe parser instead. Its receipt records "
                 "fallback=native and `den doctor` lists it; `den set build.strict true` "
                 "makes this an error.\n";
    receipt.fallback = "native";
    receipt.fallback_reason = why;
}

fs::path build_from_source(const Config& config, const PackageIndex& idx,
                           const std::string& requested, const std::string& version) {
    // "user/repo/name" is looked up qualified and built into Cellar/<name>.
    const std::string name = keg_name(requested);
    // Validate before any store path, shell, or brew fallback uses the name.
    if (!is_valid_package_name(name)) {
        throw UserError("invalid package name: " + name);
    }

    auto dest = package_path(config.store, name, version);
    if (fs::is_directory(dest) && !fs::is_empty(dest)) {
        // Already on disk — may be a Homebrew keg in the shared Cellar.
        // Do not claim it; cleanup must not delete what den did not build.
        SPDLOG_INFO("{} {} already built", name, version);
        return dest;
    }

    // Successful builds below call this so cleanup can tell a keg den produced
    // from one Homebrew poured into the same Cellar.
    // They also fix the keg's Mach-O linkage first (macOS): ids, loads and
    // rpaths stop naming the versioned keg, so it survives opt moving on.
    SourceBuildReceipt receipt;
    receipt.opt_prefix_baked = opt_prefix_for(config.store, name).string();
    const auto claim = [&]() -> fs::path {
        try {
            const auto n =
                fixup_keg_macho({dest, opt_prefix_for(config.store, name), config.store, name});
            if (n > 0) {
                std::cout << "==> Fixed Mach-O linkage of " << n << " file(s)\n";
            }
        } catch (...) {
            std::error_code ec;
            fs::remove_all(dest, ec);
            throw;
        }
        const Package* indexed = idx.find(requested);
        record_toolchain(receipt);
        mark_keg_owned(dest, name, version, indexed ? indexed->version_scheme : 0, &receipt);
        return dest;
    };

    std::cout << "==> Building " << name << " " << version << " from source\n";

    // Build sandbox (docs/source-builds.md): on macOS, configure, build and
    // install commands may not write den's prefix or $DEN_HOME outside the
    // build directory, the staging DESTDIR and the cache temp directory. An
    // install step that ignores DESTDIR then fails instead of writing the
    // live opt path or, through an opt link, an older keg.
    require_build_sandbox();
    const auto sandbox = build_sandbox_for(config, name, version);
    const bool sandboxed = build_sandbox_supported();
    struct TempDirGuard {
        fs::path path;
        ~TempDirGuard() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } sandbox_temp{sandbox.temp_dir};
    {
        std::error_code ec;
        fs::remove_all(sandbox.temp_dir, ec);
        fs::create_directories(sandbox.temp_dir);
    }
    // den_build.rb runs each command the formula's install method runs
    // (Formula#system) under this profile.
    // Every path also gets its language tool caches under the cache temp
    // directory (build_cache_env).
    std::string ruby_sandbox_env;
    for (const auto& [k, v] : build_cache_env(sandbox))
        ruby_sandbox_env += k + "=" + shell_quote(v) + " ";
    if (sandboxed)
        ruby_sandbox_env +=
            "DEN_BUILD_SANDBOX_PROFILE=" + shell_quote(seatbelt_profile(sandbox)) + " ";
    // A formula whose bottles are fixed-cellar bakes its prefix into
    // binaries, so an install step that ignores DESTDIR stays an error; any
    // other formula retries that step into a staging keg
    // (docs/source-builds.md).
    const bool keg_fallback = !bakes_fixed_prefix(idx, requested);
    if (keg_fallback)
        ruby_sandbox_env += "DEN_BUILD_KEG_FALLBACK=1 ";

    // Fetch formula source from GitHub (no brew dependency).
    FormulaOrigin origin;
    auto formula_source = fetch_formula_source(idx, requested, &origin);
    receipt.formula_tap = origin.tap;
    receipt.formula_commit = origin.commit;
    receipt.formula_path = origin.path;

    // Why a Ruby build (Path A or B) failed; set means the native parser
    // below is a fallback rather than the only path den had.
    std::string ruby_failure;

    // Path A: Bundled Ruby subprocess. macOS only: Linux's lazily downloaded
    // Portable Ruby has no Homebrew library, so fetching it here would only
    // be thrown away.
#ifdef __APPLE__
    if (!formula_source.empty()) {
        auto ruby_dir = ensure_ruby_bundle(config.den_home);
        auto ruby_bin = ruby_dir / "ruby" / "bin" / "ruby";
        auto script_path = ruby_dir / "den_build.rb";

        // Linux's lazily downloaded Portable Ruby has no Homebrew library.
        if (fs::exists(ruby_bin) && fs::exists(script_path) &&
            fs::is_directory(ruby_dir / "homebrew")) {
            auto formula_file = config.cache / "formulas" / (name + ".rb");
            fs::create_directories(formula_file.parent_path());
            {
                std::ofstream f(formula_file);
                f << formula_source;
            }

            auto cmd = ruby_sandbox_env +
                       bundled_ruby_command(config, ruby_dir, formula_file, name, version);
            SPDLOG_INFO("bundled ruby build: {}", cmd);
            auto [ruby_rc, ruby_output] = run(cmd);

            std::error_code dec;
            const bool built = fs::is_directory(dest, dec) && !fs::is_empty(dest, dec);
            if (ruby_rc == 0 && ruby_output.find("status=ok") != std::string::npos && built) {
                std::cout << "==> Built " << name << " " << version << "\n";
                parse_ruby_build_inputs(ruby_output, receipt);
                return claim();
            }
            if (auto mismatch = ruby_sha_mismatch(ruby_output)) {
                std::error_code ec;
                fs::remove_all(dest, ec);
                throw UserError(*mismatch);
            }
            SPDLOG_WARN("bundled ruby build failed (rc={}{}), falling back", ruby_rc,
                        built ? "" : ", no keg at " + dest.string());
            ruby_failure = "bundled Ruby build failed (exit " + std::to_string(ruby_rc) +
                           (ruby_rc == 0 && !built ? ", no keg" : "") + ")" +
                           last_output_line(ruby_output);
            SPDLOG_WARN("bundled ruby output (tail): {}",
                        ruby_output.size() > 2000 ? ruby_output.substr(ruby_output.size() - 2000)
                                                  : ruby_output);
            std::error_code ec;
            fs::remove_all(dest, ec);
        }
    }
#endif

    // Path B: brew ruby subprocess (Homebrew fallback — only if formula
    // source was fetched but bundled Ruby failed).
    // Only a brew that owns den's prefix may build: any other Homebrew (e.g.
    // a live /opt/homebrew while den uses its own prefix) would build with
    // its own Cellar, caches and environment.
    const char* path_env = std::getenv("PATH");
    const auto brew = find_brew_on_path(path_env ? path_env : "");
    const bool brew_ok = brew_serves_prefix(brew, config.prefix);
    if (!formula_source.empty() && !brew_ok)
        SPDLOG_INFO("skipping `brew ruby` build: {} does not serve den's prefix {}",
                    brew.empty() ? "no brew on PATH" : brew.string(), config.prefix.string());
    if (!formula_source.empty() && brew_ok) {
        auto formula_file = config.cache / "formulas" / (name + ".rb");
        // formula_file was already written in Path A; reuse it.

        std::string build_script;
        for (const auto& candidate : {"src/ruby/den_build.rb", "../share/den/den_build.rb",
                                      "/usr/local/share/den/den_build.rb"}) {
            if (fs::exists(candidate)) {
                build_script = candidate;
                break;
            }
        }

        if (!build_script.empty()) {
            auto cmd = ruby_sandbox_env + shell_quote(brew.string()) + " ruby " +
                       shell_quote(build_script) + " " + shell_quote(formula_file.string()) + " " +
                       shell_quote(config.store.string()) + " " +
                       shell_quote(config.den_home.string()) + " " + shell_quote(version) + " " +
                       shell_quote(name);
            SPDLOG_INFO("ruby build: {}", cmd);
            auto [ruby_rc, ruby_output] = run(cmd);

            if (ruby_rc == 0 && ruby_output.find("status=ok") != std::string::npos) {
                std::cout << "==> Built " << name << " " << version << " (via formula)\n";
                parse_ruby_build_inputs(ruby_output, receipt);
                return claim();
            }
            if (auto mismatch = ruby_sha_mismatch(ruby_output)) {
                std::error_code ec;
                fs::remove_all(dest, ec);
                throw UserError(*mismatch);
            }
            SPDLOG_WARN("ruby build failed (rc={}), falling back to parser", ruby_rc);
            ruby_failure = "`brew ruby` build failed (exit " + std::to_string(ruby_rc) + ")" +
                           last_output_line(ruby_output);
            std::error_code ec;
            fs::remove_all(dest, ec);
        }
    }

    // Fallback: text-parser-based build.
    if (!ruby_failure.empty())
        allow_native_fallback(config.den_home, name, version, ruby_failure, receipt);
    auto recipe = extract_build_recipe(idx, requested);
    if (recipe.source_url.empty())
        throw UserError("no source URL found for " + name);

    // Download source. The cache name keeps the URL's archive suffix
    // (.tar.xz, .tar.bz2, ...); extraction detects the format from content.
    auto tarball_path = source_archive_path(config.cache, name, version, recipe.source_url);
    fs::create_directories(tarball_path.parent_path());

    if (!fs::exists(tarball_path)) {
        std::cout << "==> Downloading source\n";
        auto data = fetch_url(recipe.source_url);
        auto tmp = tarball_path.string() + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary);
            f.write(data.c_str(), static_cast<std::streamsize>(data.size()));
        }
        fs::rename(tmp, tarball_path);
    }

    // Verify hash.
    if (!recipe.source_sha256.empty() && !verify_file(tarball_path, recipe.source_sha256)) {
        fs::remove(tarball_path);
        throw UserError("SHA256 mismatch for " + name + " source tarball");
    }

    // Extract.
    auto build_dir = config.cache / "build" / (name + "-" + version);
    std::error_code ec;
    fs::remove_all(build_dir, ec);
    fs::create_directories(build_dir);

    std::cout << "==> Extracting source\n";
    auto extracted = extract_archive(tarball_path, build_dir);

    fs::path src_dir = build_dir;
    if (!extracted.root.empty())
        src_dir = build_dir / extracted.root;

    fs::create_directories(dest);

    // Opt-prefix mode (docs/source-builds.md): configure with the stable
    // <prefix>/opt/<name> so baked-in paths survive upgrades; install steps
    // stage through DESTDIR and the staged tree merges into the versioned keg.
    const auto opt = opt_prefix_for(config.store, name);
    const auto shared_prefix = config.store.parent_path();
    const auto stage = config.cache / "build" / (name + "-" + version + ".destdir");
    fs::remove_all(stage, ec);
    const OptPathGuard opt_guard(opt, dest);

    // Build environment: point at den's installed packages.
    std::map<std::string, std::string> env;
    env["PREFIX"] = opt.string();
    env["TMPDIR"] = sandbox.temp_dir.string();
    for (const auto& [k, v] : build_cache_env(sandbox))
        env[k] = v;
    std::string pkg_config_path, cpath, library_path;
    for (const auto& pkg : list_installed(config.store)) {
        auto pc = pkg.path / "lib" / "pkgconfig";
        if (fs::is_directory(pc))
            pkg_config_path += (pkg_config_path.empty() ? "" : ":") + pc.string();
        auto inc = pkg.path / "include";
        if (fs::is_directory(inc))
            cpath += (cpath.empty() ? "" : ":") + inc.string();
        auto lib = pkg.path / "lib";
        if (fs::is_directory(lib))
            library_path += (library_path.empty() ? "" : ":") + lib.string();
    }
    if (!pkg_config_path.empty())
        env["PKG_CONFIG_PATH"] = pkg_config_path;
    if (!cpath.empty())
        env["CPATH"] = cpath;
    if (!library_path.empty())
        env["LIBRARY_PATH"] = library_path;

    // 🎯T45: steer the compiler/linker via host facts, not an xcrun shim.
    apply_shim_free_toolchain(env);

    // Run one build command; an install step stages into DESTDIR and its
    // output merges into the keg before the next command runs.
    const auto run_step = [&](std::string cmd) {
        // The parser's one file operation, `mkdir_p "#{prefix}/..."`, makes
        // a keg directory: it must not create the live opt path.
        const auto opt_mkdir = "mkdir -p " + opt.string();
        if (cmd.starts_with(opt_mkdir) &&
            (cmd.size() == opt_mkdir.size() || cmd[opt_mkdir.size()] == '/')) {
            const auto rest = cmd.substr(opt_mkdir.size());
            // den makes a single keg directory itself: the sandbox denies
            // build commands writes into the Cellar.
            if (rest.find_first_of(" \t'\"$`;&|<>()\\*?") == std::string::npos) {
                std::error_code mk;
                fs::create_directories(dest.string() + rest, mk);
                return mk ? 1 : 0;
            }
            cmd = "mkdir -p " + dest.string() + rest;
        }
        const bool install = is_install_step(cmd);
        int step_rc = run_in_dir(
            src_dir, sandboxed_shell_command(sandbox, install ? with_destdir(cmd, stage) : cmd),
            env);
        if (!install)
            return step_rc;
        // An install step that ignored DESTDIR: tree's Makefile, say, uses
        // DESTDIR as its bin directory, so the binary lands loose in the
        // stage and the man pages head for the live opt path (which the
        // sandbox denies). Unless the formula bakes a fixed prefix, retry
        // the step brew-style: install straight into the staging keg
        // <stage><opt>, then relocate any text that names it back to <opt>.
        const bool ignored = (sandboxed && step_rc != 0) ||
                             (step_rc == 0 && !staged_strays(stage, opt, shared_prefix).empty()) ||
                             (step_rc == 0 && !opt_guard.unchanged());
        if (ignored && keg_fallback && opt_guard.settle()) {
            std::error_code rm;
            fs::remove_all(stage, rm);
            const auto staged_keg = stage / opt.relative_path();
            fs::create_directories(staged_keg);
            std::cout << "==> Install step ignored DESTDIR; installing into a staging keg\n";
            auto keg_env = env;
            keg_env["PREFIX"] = staged_keg.string();
            step_rc = run_in_dir(
                src_dir, sandboxed_shell_command(sandbox, with_staged_keg(cmd, opt, staged_keg)),
                keg_env);
            if (step_rc != 0)
                return step_rc;
            relocate_staged_keg(staged_keg, opt);
        }
        if (step_rc == 0)
            merge_destdir_stage(stage, opt, dest, shared_prefix);
        return step_rc;
    };

    // Try formula-parsed build commands first.
    int rc = 0;
    bool used_formula = false;
    try {
        if (!formula_source.empty()) {
            auto parsed = parse_formula(formula_source, opt.string(), name);

            if (parsed.complexity != FormulaComplexity::Simple) {
                // Soundness invariant: never use parser output for a formula
                // whose install body contains constructs we do not handle.
                // Doing so would silently run a truncated build. Log the
                // markers for explainability, then fall through to the
                // generic build-system detector below.
                SPDLOG_WARN("native parser refused {} ({}): {} complexity markers", name,
                            to_string(parsed.complexity), parsed.complexity_markers.size());
                for (const auto& m : parsed.complexity_markers) {
                    SPDLOG_DEBUG("  [{}:{}] {}", m.construct, m.line, m.detail);
                }
            } else {
                // Apply env settings from formula.
                for (const auto& e : parsed.env_settings) {
                    auto eq = e.find("+=");
                    if (eq != std::string::npos) {
                        auto key = e.substr(0, eq);
                        auto val = e.substr(eq + 2);
                        if (env.count(key))
                            env[key] += " " + val;
                        else
                            env[key] = val;
                    }
                }

                if (!parsed.build_commands.empty()) {
                    std::cout << "==> Building (formula: " << parsed.build_commands.size()
                              << " commands)\n";
                    for (const auto& cmd : parsed.build_commands) {
                        rc = run_step(cmd);
                        if (rc != 0)
                            break;
                    }
                    used_formula = true;
                }
            }
        }

        if (!used_formula) {
            // Generic build system detection fallback.
            auto build_sys = detect_build_system(src_dir);
            std::cout << "==> Building (" << build_sys << ")\n";

            const auto q = [](const fs::path& p) { return shell_quote(p.string()); };
            std::vector<std::string> steps;
            if (build_sys == "cmake") {
                steps = {"cmake -B build -DCMAKE_INSTALL_PREFIX=" + q(opt) +
                             " -DCMAKE_BUILD_TYPE=Release && cmake --build build",
                         "cmake --install build"};
            } else if (build_sys == "autotools") {
                steps = {"./configure --prefix=" + q(opt) + " && make", "make install"};
            } else if (build_sys == "make") {
                steps = {"make PREFIX=" + q(opt),
                         "make install PREFIX=" + q(opt) + " MANDIR=" + q(opt / "share" / "man")};
            } else {
                throw UserError("don't know how to build " + name);
            }
            for (const auto& step : steps) {
                rc = run_step(step);
                if (rc != 0)
                    break;
            }
        }
        // The build must not have written the live opt path directly.
        opt_guard.check();
    } catch (...) {
        fs::remove_all(dest, ec);
        fs::remove_all(stage, ec);
        throw;
    }

    if (rc != 0) {
        fs::remove_all(dest, ec);
        fs::remove_all(stage, ec);
        throw UserError("build failed for " + name + " " + version + " (exit " +
                        std::to_string(rc) + ")" +
                        (sandboxed ? "; build commands may not write " + config.prefix.string() +
                                         " or " + config.den_home.string() +
                                         " outside the build directory, so an install step that "
                                         "ignored DESTDIR fails here (see docs/source-builds.md)"
                                   : std::string()));
    }

    fs::remove_all(build_dir, ec);
    std::cout << "==> Built " << name << " " << version << "\n";
    receipt.source = {"", recipe.source_url, recipe.source_sha256};
    return claim();
}

} // namespace den

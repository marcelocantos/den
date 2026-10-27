// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "source_build.h"
#include "formula_parser.h"

#include "../core/error.h"
#include "../download/archive.h"
#include "../download/http.h"
#include "../download/sha256.h"
#include "../platform/platform.h"
#include "../provider/exec.h"
#include "../ruby/bundle.h"
#include "../ruby/ruby_env.h"
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
// when the Ruby output carries one.
std::optional<std::string> ruby_sha_mismatch(const std::string& output) {
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
// instead resolves the SDK path directly via host facts (🎯T66) and points the
// linker at /usr/bin/ld, so no xcrun invocation is ever constructed.
//
// On Linux this is a no-op: the system toolchain needs no SDK steering.
void apply_shim_free_toolchain(std::map<std::string, std::string>& env) {
#ifdef __APPLE__
    auto facts = detect_host_facts();
    if (facts.sdk_path && fs::is_directory(*facts.sdk_path)) {
        // SDKROOT is honoured by clang/ld directly without xcrun. -isysroot
        // covers build systems (autotools, plain make) that read CFLAGS but
        // ignore SDKROOT.
        env["SDKROOT"] = *facts.sdk_path;
        const std::string isysroot = "-isysroot " + *facts.sdk_path;
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

} // namespace

std::string fetch_formula_source(const PackageIndex& idx, const std::string& name) {
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
                return source;
            }
        }
    }

    auto url =
        "https://raw.githubusercontent.com/Homebrew/homebrew-core/master/" + *pkg->ruby_source_path;
    try {
        auto source = fetch_url(url);
        SPDLOG_INFO("fetched formula source for '{}' from GitHub ({} bytes)", name, source.size());
        return source;
    } catch (const std::exception& e) {
        SPDLOG_WARN("failed to fetch formula source for '{}': {}", name, e.what());
        return "";
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
    const auto claim = [&]() -> fs::path {
        mark_keg_owned(dest, name, version);
        return dest;
    };

    std::cout << "==> Building " << name << " " << version << " from source\n";

    // Fetch formula source from GitHub (no brew dependency).
    auto formula_source = fetch_formula_source(idx, requested);

    // Path A: Bundled Ruby subprocess.
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

            auto cmd = bundled_ruby_command(config, ruby_dir, formula_file, name, version);
            SPDLOG_INFO("bundled ruby build: {}", cmd);
            auto [ruby_rc, ruby_output] = run(cmd);

            std::error_code dec;
            const bool built = fs::is_directory(dest, dec) && !fs::is_empty(dest, dec);
            if (ruby_rc == 0 && ruby_output.find("status=ok") != std::string::npos && built) {
                std::cout << "==> Built " << name << " " << version << "\n";
                return claim();
            }
            if (auto mismatch = ruby_sha_mismatch(ruby_output)) {
                std::error_code ec;
                fs::remove_all(dest, ec);
                throw UserError(*mismatch);
            }
            SPDLOG_WARN("bundled ruby build failed (rc={}{}), falling back", ruby_rc,
                        built ? "" : ", no keg at " + dest.string());
            SPDLOG_WARN("bundled ruby output (tail): {}",
                        ruby_output.size() > 2000 ? ruby_output.substr(ruby_output.size() - 2000)
                                                  : ruby_output);
            std::error_code ec;
            fs::remove_all(dest, ec);
        }
    }

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
            auto cmd = shell_quote(brew.string()) + " ruby " + shell_quote(build_script) + " " +
                       shell_quote(formula_file.string()) + " " +
                       shell_quote(config.store.string()) + " " +
                       shell_quote(config.den_home.string()) + " " + shell_quote(version) + " " +
                       shell_quote(name);
            SPDLOG_INFO("ruby build: {}", cmd);
            auto [ruby_rc, ruby_output] = run(cmd);

            if (ruby_rc == 0 && ruby_output.find("status=ok") != std::string::npos) {
                std::cout << "==> Built " << name << " " << version << " (via formula)\n";
                return claim();
            }
            if (auto mismatch = ruby_sha_mismatch(ruby_output)) {
                std::error_code ec;
                fs::remove_all(dest, ec);
                throw UserError(*mismatch);
            }
            SPDLOG_WARN("ruby build failed (rc={}), falling back to parser", ruby_rc);
            std::error_code ec;
            fs::remove_all(dest, ec);
        }
    }

    // Fallback: text-parser-based build.
    auto recipe = extract_build_recipe(idx, requested);
    if (recipe.source_url.empty())
        throw UserError("no source URL found for " + name);

    // Download source.
    auto cache_dir = config.cache / "sources";
    fs::create_directories(cache_dir);
    auto tarball_path = cache_dir / (name + "-" + version + ".tar.gz");

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

    // Build environment: point at den's installed packages.
    std::map<std::string, std::string> env;
    env["PREFIX"] = dest.string();
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

    // Try formula-parsed build commands first.
    int rc = 0;
    bool used_formula = false;
    if (!formula_source.empty()) {
        auto parsed = parse_formula(formula_source, dest.string(), name);

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
                    rc = run_in_dir(src_dir, cmd, env);
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

        if (build_sys == "cmake") {
            rc = run_in_dir(src_dir,
                            "cmake -B build -DCMAKE_INSTALL_PREFIX=" + dest.string() +
                                " -DCMAKE_BUILD_TYPE=Release && cmake --build build"
                                " && cmake --install build",
                            env);
        } else if (build_sys == "autotools") {
            rc = run_in_dir(
                src_dir, "./configure --prefix=" + dest.string() + " && make && make install", env);
        } else if (build_sys == "make") {
            rc = run_in_dir(src_dir,
                            "make PREFIX=" + dest.string() + " && make install PREFIX=" +
                                dest.string() + " MANDIR=" + (dest / "share" / "man").string(),
                            env);
        } else {
            throw UserError("don't know how to build " + name);
        }
    }

    if (rc != 0) {
        fs::remove_all(dest, ec);
        throw UserError("build failed for " + name + " " + version + " (exit " +
                        std::to_string(rc) + ")");
    }

    fs::remove_all(build_dir, ec);
    std::cout << "==> Built " << name << " " << version << "\n";
    return claim();
}

} // namespace den

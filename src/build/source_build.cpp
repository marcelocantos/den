// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "source_build.h"
#include "formula_parser.h"

#include "../core/error.h"
#include "../download/archive.h"
#include "../download/http.h"
#include "../download/sha256.h"
#include "../ruby/bundle.h"
#include "../store/store.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>

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

int run_in_dir(const fs::path& dir, const std::string& cmd,
               const std::map<std::string, std::string>& env = {}) {
    std::string env_str;
    for (const auto& [k, v] : env)
        env_str += k + "=" + v + " ";
    std::string full_cmd = "cd " + dir.string() + " && " + env_str + cmd;
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

} // namespace

std::string fetch_formula_source(const PackageIndex& idx, const std::string& name) {
    const auto* pkg = idx.find(name);
    if (!pkg || !pkg->ruby_source_path) {
        SPDLOG_DEBUG("no ruby_source_path for '{}' in index", name);
        return "";
    }

    auto url = "https://raw.githubusercontent.com/Homebrew/homebrew-core/master/" +
               *pkg->ruby_source_path;
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

    // Fallback: try brew info JSON for source URL/checksum.
    auto [rc, json_out] = run("brew info --json=v1 " + name + " 2>/dev/null");
    if (rc != 0)
        throw UserError("cannot find formula for '" + name + "'");
    BuildRecipe recipe;
    std::smatch match;
    std::regex url_re(R"RE("url"\s*:\s*"([^"]+)")RE");
    std::regex sha_re(R"RE("checksum"\s*:\s*"([0-9a-f]{64})")RE");
    if (std::regex_search(json_out, match, url_re))
        recipe.source_url = match[1].str();
    if (std::regex_search(json_out, match, sha_re))
        recipe.source_sha256 = match[1].str();
    return recipe;
}

fs::path build_from_source(const Config& config, const PackageIndex& idx,
                           const std::string& name, const std::string& version) {
    auto dest = package_path(config.store, name, version);
    if (fs::is_directory(dest) && !fs::is_empty(dest)) {
        SPDLOG_INFO("{} {} already built", name, version);
        return dest;
    }

    std::cout << "==> Building " << name << " " << version << " from source\n";

    // Fetch formula source from GitHub (no brew dependency).
    auto formula_source = fetch_formula_source(idx, name);

    // Path A: Bundled Ruby subprocess.
    if (!formula_source.empty()) {
        auto ruby_dir = ensure_ruby_bundle(config.den_home);
        auto ruby_bin = ruby_dir / "ruby" / "bin" / "ruby";
        auto script_path = ruby_dir / "den_build.rb";

        if (fs::exists(ruby_bin) && fs::exists(script_path)) {
            auto formula_file = config.cache / "formulas" / (name + ".rb");
            fs::create_directories(formula_file.parent_path());
            {
                std::ofstream f(formula_file);
                f << formula_source;
            }

            auto homebrew_lib = ruby_dir / "homebrew";
            auto sorbet_lib = ruby_dir / "gems" / "sorbet-runtime";
            auto ruby_lib = ruby_dir / "ruby" / "lib" / "ruby" / "4.0.0";

            auto cmd = ruby_bin.string() + " -I" + homebrew_lib.string() + " -I" +
                       sorbet_lib.string() + " -I" + ruby_lib.string() + " " +
                       script_path.string() + " " + formula_file.string() + " " +
                       config.store.string() + " " + config.den_home.string();

            SPDLOG_INFO("bundled ruby build: {}", cmd);
            auto [ruby_rc, ruby_output] = run(cmd);

            if (ruby_rc == 0 && ruby_output.find("status=ok") != std::string::npos) {
                std::cout << "==> Built " << name << " " << version << "\n";
                return dest;
            }
            SPDLOG_WARN("bundled ruby build failed (rc={}), falling back", ruby_rc);
            SPDLOG_DEBUG("output: {}", ruby_output.substr(0, 500));
            std::error_code ec;
            fs::remove_all(dest, ec);
        }
    }

    // Path B: brew ruby subprocess (Homebrew fallback — only if formula
    // source was fetched but bundled Ruby failed).
    if (!formula_source.empty()) {
        auto formula_file = config.cache / "formulas" / (name + ".rb");
        // formula_file was already written in Path A; reuse it.

        std::string build_script;
        for (const auto& candidate :
             {"src/ruby/den_build.rb", "../share/den/den_build.rb",
              "/usr/local/share/den/den_build.rb"}) {
            if (fs::exists(candidate)) {
                build_script = candidate;
                break;
            }
        }

        if (!build_script.empty()) {
            auto cmd = "brew ruby " + build_script + " " + formula_file.string() + " " +
                       config.store.string() + " " + config.den_home.string();
            SPDLOG_INFO("ruby build: {}", cmd);
            auto [ruby_rc, ruby_output] = run(cmd);

            if (ruby_rc == 0 && ruby_output.find("status=ok") != std::string::npos) {
                std::cout << "==> Built " << name << " " << version << " (via formula)\n";
                return dest;
            }
            SPDLOG_WARN("ruby build failed (rc={}), falling back to parser", ruby_rc);
            std::error_code ec;
            fs::remove_all(dest, ec);
        }
    }

    // Fallback: text-parser-based build.
    auto recipe = extract_build_recipe(idx, name);
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

    // Try formula-parsed build commands first.
    int rc = 0;
    bool used_formula = false;
    if (!formula_source.empty()) {
        auto parsed = parse_formula(formula_source, dest.string(), name);

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
    return dest;
}

} // namespace den

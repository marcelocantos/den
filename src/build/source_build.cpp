// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "source_build.h"

#include "../core/error.h"
#include "../download/archive.h"
#include "../download/http.h"
#include "../download/sha256.h"
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

BuildRecipe extract_build_recipe(const std::string& name) {
    auto [rc, output] = run("brew cat " + name + " 2>/dev/null");
    if (rc != 0) {
        auto [rc2, json_out] = run("brew info --json=v1 " + name + " 2>/dev/null");
        if (rc2 != 0)
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
    return parse_formula_source(output);
}

fs::path build_from_source(const Config& config, const std::string& name,
                           const std::string& version) {
    auto dest = package_path(config.store, name, version);
    if (fs::is_directory(dest) && !fs::is_empty(dest)) {
        SPDLOG_INFO("{} {} already built", name, version);
        return dest;
    }

    std::cout << "==> Building " << name << " " << version << " from source\n";

    auto recipe = extract_build_recipe(name);
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

    // Detect build system.
    if (recipe.build_system.empty())
        recipe.build_system = detect_build_system(src_dir);

    fs::create_directories(dest);

    // Build environment: point at den's installed packages.
    std::map<std::string, std::string> env;
    env["PREFIX"] = dest.string();
    std::string pkg_config_path, cpath, library_path, cmake_prefix_path;
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
        cmake_prefix_path += (cmake_prefix_path.empty() ? "" : ";") + pkg.path.string();
    }
    if (!pkg_config_path.empty())
        env["PKG_CONFIG_PATH"] = pkg_config_path;
    if (!cpath.empty())
        env["CPATH"] = cpath;
    if (!library_path.empty())
        env["LIBRARY_PATH"] = library_path;

    // Build.
    std::cout << "==> Building (" << recipe.build_system << ")\n";
    int rc = 0;

    if (recipe.build_system == "cmake") {
        std::string cmake_args = "-DCMAKE_INSTALL_PREFIX=" + dest.string() +
                                 " -DCMAKE_BUILD_TYPE=Release";
        if (!cmake_prefix_path.empty())
            cmake_args += " \"-DCMAKE_PREFIX_PATH=" + cmake_prefix_path + "\"";
        rc = run_in_dir(src_dir, "cmake -B build " + cmake_args +
                                     " && cmake --build build && cmake --install build",
                        env);
    } else if (recipe.build_system == "meson") {
        rc = run_in_dir(src_dir,
                        "meson setup build --prefix=" + dest.string() +
                            " --buildtype=release && ninja -C build && ninja -C build install",
                        env);
    } else if (recipe.build_system == "autotools") {
        rc = run_in_dir(src_dir, "./configure --prefix=" + dest.string() + " && make && make install",
                        env);
    } else if (recipe.build_system == "autogen") {
        rc = run_in_dir(src_dir,
                        "autoreconf -fi && ./configure --prefix=" + dest.string() +
                            " && make && make install",
                        env);
    } else if (recipe.build_system == "make") {
        rc = run_in_dir(src_dir,
                        "make PREFIX=" + dest.string() + " && make install PREFIX=" + dest.string() +
                            " MANDIR=" + (dest / "share" / "man").string(),
                        env);
    } else {
        throw UserError("don't know how to build " + name + " (build system: " +
                         recipe.build_system + ")");
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

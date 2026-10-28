// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Mach-O load-command scan (build/macho_scan.h) and doctor's "remaining brew
// dependencies" check, on synthetic Mach-O files so they run on any host.
// Fake Homebrew prefixes live in temp dirs; the real /opt/homebrew is never
// read or written.

#include <doctest.h>

#include "build/macho_scan.h"
#include "core/config.h"
#include "doctor/doctor.h"

#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct MsTmp {
    fs::path path;
    MsTmp() {
        auto tmpl = (fs::temp_directory_path() / "den-macho-XXXXXX").string();
        char* p = ::mkdtemp(tmpl.data());
        REQUIRE(p != nullptr);
        path = fs::canonical(p);
    }
    ~MsTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    MsTmp(const MsTmp&) = delete;
    MsTmp& operator=(const MsTmp&) = delete;
};

struct MsEnv {
    std::string name;
    std::optional<std::string> old;
    MsEnv(std::string n, std::optional<std::string> value) : name(std::move(n)) {
        if (const char* v = std::getenv(name.c_str())) {
            old = v;
        }
        if (value) {
            ::setenv(name.c_str(), value->c_str(), 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }
    ~MsEnv() {
        if (old) {
            ::setenv(name.c_str(), old->c_str(), 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }
    MsEnv(const MsEnv&) = delete;
    MsEnv& operator=(const MsEnv&) = delete;
};

using Bytes = std::vector<unsigned char>;

void le32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<unsigned char>(v >> (8 * i)));
    }
}
void be32(Bytes& b, uint32_t v) {
    for (int i = 3; i >= 0; --i) {
        b.push_back(static_cast<unsigned char>(v >> (8 * i)));
    }
}

// A load command naming `path` at lc_str offset `str_off` (24 for dylib
// commands, 12 for LC_RPATH), padded to 8 bytes.
Bytes path_cmd(uint32_t cmd, uint32_t str_off, const std::string& path) {
    uint32_t size = str_off + static_cast<uint32_t>(path.size()) + 1;
    size = (size + 7) & ~7u;
    Bytes b;
    le32(b, cmd);
    le32(b, size);
    le32(b, str_off);
    while (b.size() < str_off) {
        b.push_back(0);
    }
    b.insert(b.end(), path.begin(), path.end());
    b.resize(size, 0);
    return b;
}

constexpr uint32_t kLcIdDylib = 0xd;
constexpr uint32_t kLcLoadDylib = 0xc;
constexpr uint32_t kLcLoadWeakDylib = 0x80000018;
constexpr uint32_t kLcRpath = 0x8000001c;

struct Lc {
    uint32_t cmd;
    std::string path;
};

// A thin little-endian 64-bit Mach-O image with `cmds` (plus an LC_UUID the
// scan must skip) and some padding.
Bytes thin_macho(const std::vector<Lc>& cmds) {
    Bytes body;
    uint32_t n = 0;
    {
        Bytes uuid;
        le32(uuid, 0x1b); // LC_UUID
        le32(uuid, 24);
        uuid.resize(24, 0xab);
        body.insert(body.end(), uuid.begin(), uuid.end());
        ++n;
    }
    for (const auto& c : cmds) {
        auto b = path_cmd(c.cmd, c.cmd == kLcRpath ? 12 : 24, c.path);
        body.insert(body.end(), b.begin(), b.end());
        ++n;
    }
    Bytes h;
    le32(h, 0xfeedfacf);
    le32(h, 0x0100000c); // CPU_TYPE_ARM64
    le32(h, 0);
    le32(h, 6); // MH_DYLIB
    le32(h, n);
    le32(h, static_cast<uint32_t>(body.size()));
    le32(h, 0);
    le32(h, 0);
    h.insert(h.end(), body.begin(), body.end());
    h.resize(h.size() + 256, 0);
    return h;
}

// A fat (universal) file holding `slice` at offset 4096.
Bytes fat_macho(const Bytes& slice) {
    Bytes f;
    be32(f, 0xcafebabe);
    be32(f, 1);
    be32(f, 0x0100000c);
    be32(f, 0);
    be32(f, 4096);
    be32(f, static_cast<uint32_t>(slice.size()));
    be32(f, 12);
    f.resize(4096, 0);
    f.insert(f.end(), slice.begin(), slice.end());
    return f;
}

void write_bytes(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

void put(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << content;
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

TEST_SUITE("macho_scan") {

    TEST_CASE("macho_load_paths reads dylib and rpath load commands of a thin file") {
        MsTmp tmp;
        const auto f = tmp.path / "libx.dylib";
        write_bytes(f, thin_macho({{kLcIdDylib, "/opt/homebrew/opt/x/lib/libx.dylib"},
                                   {kLcLoadDylib, "/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib"},
                                   {kLcLoadWeakDylib, "@rpath/libweak.dylib"},
                                   {kLcRpath, "/opt/homebrew/lib"},
                                   {kLcLoadDylib, "/usr/lib/libSystem.B.dylib"}}));
        CHECK(macho_load_paths(f) ==
              std::vector<std::string>{"/opt/homebrew/opt/x/lib/libx.dylib",
                                       "/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib",
                                       "@rpath/libweak.dylib", "/opt/homebrew/lib",
                                       "/usr/lib/libSystem.B.dylib"});
    }

    TEST_CASE("macho_load_paths reads every slice of a fat file") {
        MsTmp tmp;
        const auto f = tmp.path / "fat";
        write_bytes(f, fat_macho(thin_macho({{kLcLoadDylib, "/a/lib/liba.dylib"}})));
        CHECK(macho_load_paths(f) == std::vector<std::string>{"/a/lib/liba.dylib"});
    }

    TEST_CASE("macho_load_paths ignores text, Java class files and truncated headers") {
        MsTmp tmp;
        put(tmp.path / "script", "#!/bin/sh\necho /opt/homebrew/bin\n");
        CHECK(macho_load_paths(tmp.path / "script").empty());
        Bytes java;
        be32(java, 0xcafebabe);
        be32(java, 0x00000034); // class file version, not a slice count
        java.resize(64, 0);
        write_bytes(tmp.path / "A.class", java);
        CHECK(macho_load_paths(tmp.path / "A.class").empty());
        auto cut = thin_macho({{kLcLoadDylib, "/a/lib/liba.dylib"}});
        cut.resize(40);
        write_bytes(tmp.path / "cut", cut);
        CHECK(macho_load_paths(tmp.path / "cut").empty());
        CHECK(macho_load_paths(tmp.path / "absent").empty());
    }

    TEST_CASE("keg_brew_links finds literal and symlinked paths into Homebrew's prefix") {
        MsTmp tmp;
        const auto brew = tmp.path / "brew";
        const auto den = tmp.path / "den";
        // den/opt/present -> brew/opt/present -> brew/Cellar/present/1.0
        fs::create_directories(brew / "Cellar" / "present" / "1.0" / "lib");
        put(brew / "Cellar" / "present" / "1.0" / "lib" / "libp.dylib", "x");
        fs::create_directories(brew / "opt");
        fs::create_symlink("../Cellar/present/1.0", brew / "opt" / "present");
        fs::create_directories(den / "opt");
        fs::create_symlink(brew / "opt" / "present", den / "opt" / "present");
        // den/opt/gone -> brew/opt/gone, which brew removed (dangling)
        fs::create_symlink(brew / "opt" / "gone", den / "opt" / "gone");
        const auto keg = den / "Cellar" / "user" / "1.0";
        write_bytes(
            keg / "lib" / "libu.dylib",
            thin_macho({{kLcIdDylib, (keg / "lib" / "libu.dylib").string()},
                        {kLcLoadDylib, (brew / "opt" / "lit" / "lib/libl.dylib").string()},
                        {kLcLoadDylib, (den / "opt" / "present" / "lib/libp.dylib").string()},
                        {kLcLoadDylib, (den / "opt" / "gone" / "lib/libg.dylib").string()},
                        {kLcLoadDylib, "@rpath/libr.dylib"},
                        {kLcLoadDylib, "/usr/lib/libSystem.B.dylib"}}));
        fs::create_symlink("libu.dylib", keg / "lib" / "libu.1.dylib"); // not scanned twice
        put(keg / "bin" / "script", "#!/bin/sh\n" + (brew / "opt" / "lit").string() + "\n");

        auto refs = keg_brew_links(keg, brew);
        REQUIRE(refs.size() == 3);
        CHECK(brew_link_formulae(refs) == std::vector<std::string>{"gone", "lit", "present"});
        for (const auto& r : refs) {
            CAPTURE(r.load);
            CHECK(r.file == keg / "lib" / "libu.dylib");
            CHECK(has(r.brew_path, brew.string() + "/"));
        }
        CHECK(keg_brew_links(keg, tmp.path / "elsewhere").empty());
        CHECK(keg_brew_links(keg, "").empty());
    }

    TEST_CASE("doctor lists remaining brew dependencies of den's own prefix") {
        MsTmp tmp;
        const auto brew = tmp.path / "brew";
        const auto den_home = tmp.path / "den";
        const auto prefix = den_home / "prefix";
        fs::create_directories(brew / "Cellar" / "keep" / "3.6.3" / "lib");
        put(brew / "Cellar" / "keep" / "3.6.3" / "INSTALL_RECEIPT.json", "{}");
        fs::create_directories(brew / "opt");
        fs::create_symlink("../Cellar/keep/3.6.3", brew / "opt" / "keep");
        put(brew / "bin" / "brew", "#!/bin/sh\n");
        put(den_home / "manifests" / "ROOT" / "manifest.json",
            R"({"packages":{"homebrew":{"keep":"3.6.3","user":"1.0","plain":"1.0"}},)"
            R"("auto_deps":{}})");
        fs::create_directories(prefix / "opt");
        fs::create_symlink(brew / "opt" / "keep", prefix / "opt" / "keep");
        const auto user = prefix / "Cellar" / "user" / "1.0";
        write_bytes(
            user / "lib" / "libu.dylib",
            thin_macho({{kLcLoadDylib, (prefix / "opt" / "keep" / "lib/libk.dylib").string()},
                        {kLcLoadDylib, "/usr/lib/libSystem.B.dylib"}}));
        const auto plain = prefix / "Cellar" / "plain" / "1.0";
        write_bytes(plain / "lib" / "libp.dylib",
                    thin_macho({{kLcLoadDylib, (plain / "lib" / "libp.dylib").string()}}));
        fs::create_symlink("../Cellar/plain/1.0", prefix / "opt" / "plain");
        put(brew / "Cellar" / "keep" / "3.6.3" / "lib" / "libk.dylib", "x");

        MsEnv h("DEN_HOME", den_home.string());
        MsEnv dp("DEN_PREFIX", std::nullopt);
        MsEnv bp("HOMEBREW_PREFIX", brew.string());
        MsEnv bc("HOMEBREW_CELLAR", (brew / "Cellar").string());
        const auto cfg = Config::detect();
        REQUIRE(cfg.prefix == prefix);
        REQUIRE_FALSE(cfg.shares_homebrew_cellar());

        auto deps = remaining_brew_dependencies(cfg);
        REQUIRE(deps.size() == 2);
        CHECK(deps[0].keg == "keep 3.6.3");
        CHECK(has(deps[0].detail, "stays with brew (" + (prefix / "opt" / "keep").string() +
                                      " -> " + (brew / "opt" / "keep").string() + ")"));
        CHECK_FALSE(deps[0].broken);
        CHECK(deps[1].keg == "user/1.0");
        CHECK(deps[1].detail == "loads keep from " + brew.string());
        CHECK_FALSE(deps[1].broken);
        const auto text = format_brew_dependencies(deps);
        CHECK(has(text, "remaining brew dependencies: 2\n    keep 3.6.3: stays with brew"));
        CHECK(has(text, "\n    user/1.0: loads keep from " + brew.string()));

        // Brew removed the keg: both entries are flagged, never silently fine.
        fs::remove_all(brew / "Cellar" / "keep");
        deps = remaining_brew_dependencies(cfg);
        REQUIRE(deps.size() == 2);
        CHECK(deps[0].keg == "keep");
        CHECK(deps[0].broken);
        CHECK(deps[1].broken);
        CHECK(has(format_brew_dependencies(deps), "[missing]"));
    }

    TEST_CASE("no remaining brew dependencies while den shares Homebrew's prefix") {
        MsTmp tmp;
        const auto brew = tmp.path / "brew";
        fs::create_directories(brew / "Cellar");
        MsEnv h("DEN_HOME", (tmp.path / "den").string());
        MsEnv dp("DEN_PREFIX", brew.string());
        MsEnv bp("HOMEBREW_PREFIX", brew.string());
        MsEnv bc("HOMEBREW_CELLAR", (brew / "Cellar").string());
        CHECK(remaining_brew_dependencies(Config::detect()).empty());
    }
}

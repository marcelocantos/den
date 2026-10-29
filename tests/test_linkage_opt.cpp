// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Bottle linkage through <prefix>/opt/<dep> (as Homebrew links it), with the
// guard that keeps a pin wherever den cannot rely on the opt link, and the
// doctor report of pins that remain and of opt links that are missing.

#include <doctest.h>

#include "build/elf_scan.h"
#include "build/linkage_scan.h"
#include "build/macho_scan.h"
#include "build/relocate.h"
#include "core/config.h"
#include "doctor/doctor.h"
#include "store/store.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct LinkTmp {
    fs::path path;
    LinkTmp() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("den-linkage-opt-" + std::to_string(rd()));
        fs::create_directories(path);
        path = fs::canonical(path);
    }
    ~LinkTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    LinkTmp(const LinkTmp&) = delete;
    LinkTmp& operator=(const LinkTmp&) = delete;
};

void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}

fs::path owned_keg(const fs::path& cellar, const std::string& name, const std::string& version) {
    const auto keg = cellar / name / version;
    fs::create_directories(keg);
    mark_keg_owned(keg, name, version);
    return keg;
}

using Bytes = std::vector<unsigned char>;

void le32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<unsigned char>(v >> (8 * i)));
}

constexpr uint32_t kLcIdDylib = 0xd;
constexpr uint32_t kLcLoadDylib = 0xc;
constexpr uint32_t kLcRpath = 0x8000001c;

struct Lc {
    uint32_t cmd;
    std::string path;
};

// A thin little-endian 64-bit Mach-O image of `filetype` with `cmds`.
void put_macho(const fs::path& p, uint32_t filetype, const std::vector<Lc>& cmds) {
    Bytes body;
    for (const auto& c : cmds) {
        const uint32_t off = c.cmd == kLcRpath ? 12 : 24;
        const uint32_t size = (off + static_cast<uint32_t>(c.path.size()) + 1 + 7) & ~7u;
        Bytes b;
        le32(b, c.cmd);
        le32(b, size);
        le32(b, off);
        b.resize(off, 0);
        b.insert(b.end(), c.path.begin(), c.path.end());
        b.resize(size, 0);
        body.insert(body.end(), b.begin(), b.end());
    }
    Bytes h;
    le32(h, 0xfeedfacf);
    le32(h, 0x0100000c); // CPU_TYPE_ARM64
    le32(h, 0);
    le32(h, filetype);
    le32(h, static_cast<uint32_t>(cmds.size()));
    le32(h, static_cast<uint32_t>(body.size()));
    le32(h, 0);
    le32(h, 0);
    h.insert(h.end(), body.begin(), body.end());
    h.resize(h.size() + 256, 0);
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));
}

// den's own prefix, separate from Homebrew's.
Config private_config(const fs::path& root) {
    Config c;
    c.den_home = root / "den";
    c.prefix = root / "prefix";
    c.store = c.prefix / "Cellar";
    c.homebrew_prefix = root / "homebrew";
    c.homebrew_cellar = c.homebrew_prefix / "Cellar";
    fs::create_directories(c.store);
    return c;
}

bool contains(const std::vector<std::string>& v, const std::string& needle) {
    for (const auto& s : v)
        if (s.find(needle) != std::string::npos)
            return true;
    return false;
}

const std::string kOptRef = "@@HOMEBREW_PREFIX@@/opt/gettext/lib/libintl.8.dylib";

} // namespace

TEST_SUITE("linkage_opt") {

    TEST_CASE("bind_opt_reference keeps the opt reference when den owns the link") {
        LinkTmp t;
        const auto cellar = t.path / "prefix" / "Cellar";
        const auto self = cellar / "pv" / "1.12.0";
        fs::create_directories(self);
        const auto gettext = owned_keg(cellar, "gettext", "1.0");
        touch(gettext / "lib" / "libintl.8.dylib");

        SUBCASE("link missing: den creates it when it links gettext") {
            CHECK(bind_opt_reference(kOptRef, cellar, "pv", self, false) == kOptRef);
        }
        SUBCASE("link to den's keg") {
            fs::create_directories(cellar.parent_path() / "opt");
            fs::create_symlink("../Cellar/gettext/1.0", cellar.parent_path() / "opt" / "gettext");
            CHECK(bind_opt_reference(kOptRef, cellar, "pv", self, false) == kOptRef);
        }
        SUBCASE("dangling link: den repoints it") {
            fs::create_directories(cellar.parent_path() / "opt");
            fs::create_symlink("../Cellar/gettext/0.9", cellar.parent_path() / "opt" / "gettext");
            CHECK(bind_opt_reference(kOptRef, cellar, "pv", self, false) == kOptRef);
        }
        // The package's own opt references still go to the keg being poured.
        CHECK(bind_opt_reference("@@HOMEBREW_PREFIX@@/opt/pv/lib/libpv.dylib", cellar, "pv", self,
                                 false) == self.string() + "/lib/libpv.dylib");
        CHECK(bind_opt_reference("/usr/lib/libSystem.B.dylib", cellar, "pv", self, false) ==
              "/usr/lib/libSystem.B.dylib");
    }

    TEST_CASE("bind_opt_reference pins where den cannot rely on the opt link") {
        LinkTmp t;
        const auto cellar = t.path / "prefix" / "Cellar";
        const auto opt = cellar.parent_path() / "opt";
        const auto self = cellar / "pv" / "1.12.0";
        fs::create_directories(self);
        const auto pinned = (cellar / "gettext" / "1.0").string() + "/lib/libintl.8.dylib";

        SUBCASE("shared prefix: Homebrew may repoint any opt link") {
            owned_keg(cellar, "gettext", "1.0");
            CHECK(bind_opt_reference(kOptRef, cellar, "pv", self, true) == pinned);
        }
        SUBCASE("the opt link names a keg den did not pour (PR #64's case)") {
            touch(cellar / "gettext" / "1.0" / "lib" / "libintl.8.dylib");
            fs::create_directories(opt);
            fs::create_symlink("../Cellar/gettext/1.0", opt / "gettext");
            CHECK(bind_opt_reference(kOptRef, cellar, "pv", self, false) == pinned);
        }
        SUBCASE("the opt link resolves outside the Cellar") {
            owned_keg(cellar, "gettext", "1.0");
            touch(t.path / "elsewhere" / "gettext" / "lib" / "libintl.8.dylib");
            fs::create_directories(opt);
            fs::create_symlink(t.path / "elsewhere" / "gettext", opt / "gettext");
            CHECK(bind_opt_reference(kOptRef, cellar, "pv", self, false) == pinned);
        }
        SUBCASE("opt/<dep> is a directory, not a link") {
            owned_keg(cellar, "gettext", "1.0");
            fs::create_directories(opt / "gettext" / "lib");
            CHECK(bind_opt_reference(kOptRef, cellar, "pv", self, false) == pinned);
        }
        SUBCASE("no keg at all: left as an opt reference") {
            CHECK(bind_opt_reference(kOptRef, cellar, "pv", self, false) == kOptRef);
        }
    }

    TEST_CASE("doctor reports kegs still pinned to versioned dependency kegs") {
        LinkTmp t;
        auto cfg = private_config(t.path);
        const auto libx = owned_keg(cfg.store, "libx", "1.0");
        put_macho(libx / "lib" / "libx.dylib", kMachoDylib,
                  {{kLcIdDylib, (cfg.prefix / "opt" / "libx" / "lib" / "libx.dylib").string()}});
        fs::create_directories(cfg.prefix / "opt");
        fs::create_symlink("../Cellar/libx/1.0", cfg.prefix / "opt" / "libx");
        const auto pinned_app = owned_keg(cfg.store, "old", "1.0");
        put_macho(pinned_app / "bin" / "old", kMachoExecute,
                  {{kLcLoadDylib, (libx / "lib" / "libx.dylib").string()},
                   {kLcRpath, (libx / "lib").string()}});
        const auto bound_app = owned_keg(cfg.store, "new", "1.0");
        put_macho(bound_app / "bin" / "new", kMachoExecute,
                  {{kLcLoadDylib, (cfg.prefix / "opt" / "libx" / "lib" / "libx.dylib").string()}});
        // A keg den did not pour is not den's to report.
        touch(cfg.store / "brewed" / "1.0" / "INSTALL_RECEIPT.json");
        put_macho(cfg.store / "brewed" / "1.0" / "bin" / "brewed", kMachoExecute,
                  {{kLcLoadDylib, (libx / "lib" / "libx.dylib").string()}});

        const auto r = scan_keg_linkage(cfg);
        REQUIRE(r.pinned.size() == 1);
        CHECK(r.pinned[0] == "old 1.0 -> libx 1.0 (2 load paths, e.g. bin/old)");
        CHECK(r.broken_opt.empty());
        const auto note = format_pinned_kegs(r.pinned, false);
        CHECK(note.find("kegs pinned to versioned dependency kegs: 1") != std::string::npos);
        CHECK(note.find("\n    old 1.0 -> libx 1.0") != std::string::npos);
        CHECK(format_pinned_kegs(r.pinned, true).find("old 1.0") == std::string::npos);
        CHECK(format_pinned_kegs({}, false).empty());
    }

    TEST_CASE("doctor reports libraries loaded through a missing opt link") {
        LinkTmp t;
        auto cfg = private_config(t.path);
        const auto opt = cfg.prefix / "opt";
        fs::create_directories(opt);
        const auto app = owned_keg(cfg.store, "app", "1.0");
        put_macho(app / "bin" / "app", kMachoExecute,
                  {{kLcLoadDylib, (opt / "gone" / "lib" / "libgone.dylib").string()},
                   {kLcLoadDylib, (opt / "dang" / "lib" / "libdang.dylib").string()},
                   {kLcLoadDylib, (opt / "short" / "lib" / "libshort.1.dylib").string()},
                   {kLcLoadDylib, (opt / "fine" / "lib" / "libfine.dylib").string()},
                   {kLcLoadDylib, "/usr/lib/libSystem.B.dylib"}});
        fs::create_symlink("../Cellar/dang/1.0", opt / "dang");
        touch(owned_keg(cfg.store, "short", "2.0") / "lib" / "libshort.2.dylib");
        fs::create_symlink("../Cellar/short/2.0", opt / "short");
        touch(owned_keg(cfg.store, "fine", "1.0") / "lib" / "libfine.dylib");
        fs::create_symlink("../Cellar/fine/1.0", opt / "fine");

        const auto r = scan_keg_linkage(cfg);
        CHECK(r.pinned.empty());
        CHECK(r.broken_opt.size() == 3);
        CHECK(contains(r.broken_opt, "app 1.0: bin/app loads " +
                                         (opt / "gone" / "lib" / "libgone.dylib").string() + ": " +
                                         (opt / "gone").string() + " is missing"));
        CHECK(contains(r.broken_opt, (opt / "dang").string() + " is dangling"));
        CHECK(contains(r.broken_opt, (opt / "short").string() + " has no lib/libshort.1.dylib"));
        CHECK_FALSE(contains(r.broken_opt, "libfine"));
    }

    TEST_CASE("keg_opt_loads follows @rpath loads through an opt rpath") {
        LinkTmp t;
        const auto opt = t.path / "prefix" / "opt";
        const auto keg = t.path / "prefix" / "Cellar" / "app" / "1.0";
        put_macho(
            keg / "bin" / "app", kMachoExecute,
            {{kLcLoadDylib, "@rpath/libz.1.dylib"}, {kLcRpath, (opt / "zlib" / "lib").string()}});
        const auto loads = keg_opt_loads(keg, opt);
        REQUIRE(loads.size() == 1);
        CHECK(loads[0].dep == "zlib");
        CHECK(loads[0].tail == fs::path("lib/libz.1.dylib"));
        CHECK_FALSE(loads[0].exact);
    }

#ifndef __APPLE__
    TEST_CASE("elf_dynamic reads DT_NEEDED and RPATH") {
        LinkTmp t;
        {
            std::ofstream(t.path / "a.c") << "int a(void) { return 1; }\n";
            std::ofstream(t.path / "m.c") << "int a(void);\nint main(void) { return a(); }\n";
        }
        const auto lib = t.path / "lib" / "liba.so.1";
        fs::create_directories(lib.parent_path());
        const auto cmd1 = "cc -shared -fPIC -Wl,-soname,liba.so.1 -o '" + lib.string() + "' '" +
                          (t.path / "a.c").string() + "'";
        REQUIRE(std::system(cmd1.c_str()) == 0);
        const auto cmd2 = "cc -o '" + (t.path / "m").string() + "' '" + (t.path / "m.c").string() +
                          "' '" + lib.string() + "' -Wl,-rpath,/x/opt/a/lib:/y";
        REQUIRE(std::system(cmd2.c_str()) == 0);
        const auto e = elf_dynamic(t.path / "m");
        CHECK(e.is_elf);
        CHECK(std::find(e.needed.begin(), e.needed.end(), "liba.so.1") != e.needed.end());
        CHECK(e.search_paths == std::vector<std::string>{"/x/opt/a/lib", "/y"});
        CHECK_FALSE(elf_dynamic(t.path / "m.c").is_elf);
    }
#endif
}

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Mach-O linkage fix-up for source-built kegs (build/macho_fixup.h).
//
// The planning and verification tests use synthetic Mach-O files, so they
// run on any host. The macOS tests compile a real fixture keg (nested
// libexec helper, plugin dylibs under lib/engines-3 and lib/ossl-modules, a
// dependency linked through its versioned keg), fix it up, and check that it
// still loads after its own opt link, and its dependency's, move to new
// versions. Everything lives in temp dirs; no real prefix is touched.

#include <doctest.h>

#include "build/macho_fixup.h"
#include "build/macho_scan.h"
#include "build/source_build.h"
#include "core/config.h"
#include "core/error.h"
#include "download/sha256.h"
#include "index/package.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct FxTmp {
    fs::path path;
    FxTmp() {
        auto tmpl = (fs::temp_directory_path() / "den-fixup-XXXXXX").string();
        char* p = ::mkdtemp(tmpl.data());
        REQUIRE(p != nullptr);
        path = fs::canonical(p);
    }
    ~FxTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    FxTmp(const FxTmp&) = delete;
    FxTmp& operator=(const FxTmp&) = delete;
};

using Bytes = std::vector<unsigned char>;

void le32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<unsigned char>(v >> (8 * i)));
    }
}

constexpr uint32_t kLcIdDylib = 0xd;
constexpr uint32_t kLcLoadDylib = 0xc;
constexpr uint32_t kLcLoadWeakDylib = 0x80000018;
constexpr uint32_t kLcRpath = 0x8000001c;

struct Lc {
    uint32_t cmd;
    std::string path;
};

// A thin little-endian 64-bit Mach-O image of `filetype` with `cmds`.
Bytes macho(uint32_t filetype, const std::vector<Lc>& cmds) {
    Bytes body;
    for (const auto& c : cmds) {
        const uint32_t off = c.cmd == kLcRpath ? 12 : 24;
        uint32_t size = (off + static_cast<uint32_t>(c.path.size()) + 1 + 7) & ~7u;
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
    return h;
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

// A fake prefix with den's store and the keg fx 1.0.
struct Prefix {
    FxTmp tmp;
    fs::path store = tmp.path / "Cellar";
    MachoFixContext ctx = macho_fix_context(store, "fx", "1.0");
    std::string keg = ctx.keg.string();
    std::string opt = ctx.opt.string();
};

bool has_problem(const std::vector<MachoProblem>& ps, const fs::path& file,
                 const std::string& needle) {
    for (const auto& p : ps) {
        if (p.file == file && p.detail.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

#ifdef __APPLE__

namespace {

std::string sh_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return out + "'";
}

struct Run {
    int rc;
    std::string out;
};

Run run_cmd(const std::string& cmd) {
    std::string out;
    FILE* p = ::popen((cmd + " 2>&1").c_str(), "r");
    REQUIRE(p != nullptr);
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), buf.size(), p)) {
        out += buf.data();
    }
    const int status = ::pclose(p);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : 128, out};
}

void cc(const std::string& args) {
    const auto r = run_cmd("cc " + args);
    INFO("cc " << args << "\n" << r.out);
    REQUIRE(r.rc == 0);
}

std::string q(const fs::path& p) {
    return sh_quote(p.string());
}

// depa <version>: a dependency keg whose id is its versioned path (as a
// pinned bottle's is).
void build_dep(const fs::path& store, const fs::path& src, const std::string& version) {
    const auto lib = store / "depa" / version / "lib" / "libdepa.dylib";
    fs::create_directories(lib.parent_path());
    put(src / ("depa-" + version + ".c"),
        "const char* depa(void) { return \"depa " + version + "\"; }\n");
    cc("-dynamiclib -install_name " + q(lib) + " -o " + q(lib) + " " +
       q(src / ("depa-" + version + ".c")));
}

void point(const fs::path& opt, const std::string& target) {
    std::error_code ec;
    fs::remove(opt, ec);
    fs::create_directories(opt.parent_path());
    fs::create_directory_symlink(target, opt);
}

// fx <version>, linked the way an opt-prefix build links it: its own
// libraries by their opt id (and one by @rpath with a versioned rpath),
// depa by depa's versioned keg path.
void build_fx(const fs::path& store, const fs::path& src, const std::string& version) {
    const auto ctx = macho_fix_context(store, "fx", version);
    const auto& k = ctx.keg;
    const auto deplib = store / "depa" / "1.0" / "lib" / "libdepa.dylib";
    for (const auto* d : {"bin", "lib/engines-3", "lib/ossl-modules", "libexec/fx-core"}) {
        fs::create_directories(k / d);
    }
    put(src / "fx.c", "#include <stdio.h>\nconst char* depa(void);\n"
                      "const char* fx(void) { static char b[64];\n"
                      "  snprintf(b, sizeof b, \"fx " +
                          version + " + %s\", depa()); return b; }\n");
    put(src / "fxutil.c", "const char* fxutil(void) { return \"fxutil " + version + "\"; }\n");
    put(src / "main.c", "#include <dlfcn.h>\n#include <stdio.h>\n"
                        "const char* fx(void);\n"
                        "int main(int argc, char** argv) {\n"
                        "  printf(\"%s\\n\", fx());\n"
                        "  if (argc > 1) {\n"
                        "    void* h = dlopen(argv[1], RTLD_NOW);\n"
                        "    if (!h) { printf(\"dlopen: %s\\n\", dlerror()); return 3; }\n"
                        "    const char* (*p)(void) = (const char* (*)(void))dlsym(h, "
                        "\"plugin\");\n"
                        "    if (!p) return 4;\n"
                        "    printf(\"%s\\n\", p());\n"
                        "  }\n"
                        "  return 0;\n}\n");
    put(src / "helper.c", "#include <stdio.h>\nconst char* fx(void);\n"
                          "const char* fxutil(void);\n"
                          "int main(void) { printf(\"helper %s %s\\n\", fx(), fxutil()); "
                          "return 0; }\n");
    put(src / "plugin.c", "const char* fx(void);\nconst char* plugin(void) { return fx(); }\n");
    put(src / "module.c", "const char* fxutil(void);\n"
                          "const char* plugin(void) { return fxutil(); }\n");

    const auto libfx = k / "lib" / "libfx.1.dylib";
    const auto libfxutil = k / "lib" / "libfxutil.dylib";
    cc("-dynamiclib -install_name " + q(ctx.opt / "lib" / "libfx.1.dylib") + " -o " + q(libfx) +
       " " + q(src / "fx.c") + " " + q(deplib));
    fs::create_symlink("libfx.1.dylib", k / "lib" / "libfx.dylib");
    cc("-dynamiclib -install_name @rpath/libfxutil.dylib -o " + q(libfxutil) + " " +
       q(src / "fxutil.c"));
    cc("-o " + q(k / "bin" / "fx") + " " + q(src / "main.c") + " " + q(libfx));
    cc("-o " + q(k / "libexec" / "fx-core" / "fx-helper") + " " + q(src / "helper.c") + " " +
       q(libfx) + " " + q(libfxutil));
    cc("-bundle -o " + q(k / "lib" / "engines-3" / "padlock.dylib") + " " + q(src / "plugin.c") +
       " " + q(libfx));
    cc("-dynamiclib -install_name " + q(k / "lib" / "ossl-modules" / "legacy.dylib") + " -o " +
       q(k / "lib" / "ossl-modules" / "legacy.dylib") + " " + q(src / "module.c") + " " +
       q(libfxutil) + " -Wl,-rpath," + (k / "lib").string());
}

// Every path any Mach-O file in `keg` names.
std::vector<std::string> all_load_paths(const fs::path& keg) {
    std::vector<std::string> out;
    for (const auto& f : keg_macho_files(keg)) {
        for (const auto& p : macho_load_paths(f)) {
            out.push_back(f.filename().string() + ": " + p);
        }
    }
    return out;
}

} // namespace

#endif // __APPLE__

TEST_SUITE("macho_fixup") {

    TEST_CASE("macho_info types load commands and reads the file type") {
        FxTmp tmp;
        const auto f = tmp.path / "libx.dylib";
        write_bytes(f, macho(kMachoDylib, {{kLcIdDylib, "/p/opt/x/lib/libx.dylib"},
                                           {kLcLoadDylib, "/usr/lib/libSystem.B.dylib"},
                                           {kLcLoadWeakDylib, "@rpath/libw.dylib"},
                                           {kLcRpath, "@loader_path"}}));
        const auto info = macho_info(f);
        CHECK(info.is_macho);
        CHECK(info.filetype == kMachoDylib);
        CHECK(info.commands == std::vector<MachoLoadCommand>{
                                   {MachoLoadKind::Id, "/p/opt/x/lib/libx.dylib"},
                                   {MachoLoadKind::Load, "/usr/lib/libSystem.B.dylib"},
                                   {MachoLoadKind::WeakLoad, "@rpath/libw.dylib"},
                                   {MachoLoadKind::Rpath, "@loader_path"},
                               });
        put(tmp.path / "text", "#!/bin/sh\n");
        CHECK_FALSE(macho_info(tmp.path / "text").is_macho);
    }

    TEST_CASE("fixup_load_path: keg-relative for the keg, opt for dependencies") {
        Prefix px;
        const auto bin = px.ctx.keg / "bin" / "fx";
        const auto helper = px.ctx.keg / "libexec" / "git-core" / "git-remote-http";
        const auto engine = px.ctx.keg / "lib" / "engines-3" / "padlock.dylib";
        const auto lib = px.ctx.keg / "lib" / "libfx.1.dylib";

        // The keg itself, by versioned path or through its own opt link.
        CHECK(fixup_load_path(px.keg + "/lib/libfx.1.dylib", bin, px.ctx) ==
              "@loader_path/../lib/libfx.1.dylib");
        CHECK(fixup_load_path(px.opt + "/lib/libfx.1.dylib", bin, px.ctx) ==
              "@loader_path/../lib/libfx.1.dylib");
        CHECK(fixup_load_path(px.opt + "/lib/libfx.1.dylib", helper, px.ctx) ==
              "@loader_path/../../lib/libfx.1.dylib");
        CHECK(fixup_load_path(px.opt + "/lib/libcrypto.3.dylib", engine, px.ctx) ==
              "@loader_path/../libcrypto.3.dylib");
        CHECK(fixup_load_path(px.keg + "/lib", lib, px.ctx) == "@loader_path");
        // Another version of the same formula is still this keg.
        CHECK(fixup_load_path((px.store / "fx" / "0.9" / "lib" / "libfx.1.dylib").string(), bin,
                              px.ctx) == "@loader_path/../lib/libfx.1.dylib");

        // A dependency's versioned keg goes through the dependency's opt link.
        CHECK(
            fixup_load_path((px.store / "openssl@3" / "3.4.1" / "lib" / "libssl.3.dylib").string(),
                            bin, px.ctx) ==
            (px.tmp.path / "opt" / "openssl@3" / "lib" / "libssl.3.dylib").string());
        CHECK(fixup_load_path((px.store / "gettext" / "0.22" / "lib").string(), bin, px.ctx) ==
              (px.tmp.path / "opt" / "gettext" / "lib").string());

        // Everything else is left alone.
        for (const auto& p : std::vector<std::string>{
                 "/usr/lib/libSystem.B.dylib", "@rpath/libz.1.dylib", "@loader_path/../lib/x.dylib",
                 "@executable_path/../lib/x.dylib",
                 (px.tmp.path / "opt" / "zlib" / "lib" / "libz.1.dylib").string()}) {
            CHECK(fixup_load_path(p, bin, px.ctx) == p);
        }
    }

    TEST_CASE("plan_macho_fixup rewrites ids, loads and rpaths of a nested plugin dylib") {
        Prefix px;
        const auto engine = px.ctx.keg / "lib" / "engines-3" / "padlock.dylib";
        put(px.ctx.keg / "lib" / "libcrypto.3.dylib", "x");
        const auto dep = (px.store / "zlib" / "1.3" / "lib" / "libz.1.dylib").string();
        write_bytes(engine,
                    macho(kMachoDylib, {{kLcIdDylib, px.keg + "/lib/engines-3/padlock.dylib"},
                                        {kLcLoadDylib, px.opt + "/lib/libcrypto.3.dylib"},
                                        {kLcLoadDylib, dep},
                                        {kLcLoadDylib, "@rpath/libcrypto.3.dylib"},
                                        {kLcLoadDylib, "/usr/lib/libSystem.B.dylib"},
                                        {kLcRpath, px.keg + "/lib"},
                                        {kLcRpath, px.opt + "/lib"}}));
        const auto plan = plan_macho_fixup(engine, macho_info(engine), px.ctx);
        CHECK(plan.new_id == px.opt + "/lib/engines-3/padlock.dylib");
        CHECK(plan.changes ==
              std::vector<std::pair<std::string, std::string>>{
                  {px.opt + "/lib/libcrypto.3.dylib", "@loader_path/../libcrypto.3.dylib"},
                  {dep, (px.tmp.path / "opt" / "zlib" / "lib" / "libz.1.dylib").string()},
              });
        // Both rpaths name the keg's lib; the second would duplicate the first.
        CHECK(plan.rpath_changes == std::vector<std::pair<std::string, std::string>>{
                                        {px.keg + "/lib", "@loader_path/.."}});
        CHECK(plan.delete_rpaths == std::vector<std::string>{px.opt + "/lib"});
        CHECK(plan.add_rpaths.empty()); // @rpath/libcrypto resolves via @loader_path/..
    }

    TEST_CASE(
        "plan_macho_fixup leaves @rpath ids and adds an rpath for an unresolved @rpath load") {
        Prefix px;
        put(px.ctx.keg / "lib" / "libfxutil.dylib", "x");
        const auto lib = px.ctx.keg / "lib" / "libfxutil.dylib";
        write_bytes(lib, macho(kMachoDylib, {{kLcIdDylib, "@rpath/libfxutil.dylib"}}));
        CHECK(plan_macho_fixup(lib, macho_info(lib), px.ctx).empty());

        const auto helper = px.ctx.keg / "libexec" / "git-core" / "git-remote-http";
        write_bytes(helper, macho(kMachoExecute, {{kLcLoadDylib, "@rpath/libfxutil.dylib"}}));
        const auto plan = plan_macho_fixup(helper, macho_info(helper), px.ctx);
        CHECK(plan.new_id.empty());
        CHECK(plan.changes.empty());
        CHECK(plan.add_rpaths == std::vector<std::string>{"@loader_path/../../lib"});

        // An id outside the keg (a build-tree path) becomes the opt name.
        const auto stray = px.ctx.keg / "libexec" / "gettext" / "libhelper.dylib";
        write_bytes(stray,
                    macho(kMachoDylib, {{kLcIdDylib, "/private/tmp/build/libhelper.dylib"}}));
        CHECK(plan_macho_fixup(stray, macho_info(stray), px.ctx).new_id ==
              px.opt + "/libexec/gettext/libhelper.dylib");
    }

    TEST_CASE("keg_macho_files walks every directory and skips symlinks and non-images") {
        Prefix px;
        const auto& k = px.ctx.keg;
        const std::vector<fs::path> want{
            k / "bin" / "fx",
            k / "lib" / "engines-3" / "padlock.dylib",
            k / "lib" / "libfx.1.dylib",
            k / "lib" / "ossl-modules" / "legacy.dylib",
            k / "libexec" / "gettext" / "hostname",
            k / "libexec" / "git-core" / "git-remote-http",
        };
        write_bytes(want[0], macho(kMachoExecute, {}));
        write_bytes(want[1], macho(kMachoBundle, {}));
        write_bytes(want[2], macho(kMachoDylib, {}));
        write_bytes(want[3], macho(kMachoBundle, {}));
        write_bytes(want[4], macho(kMachoExecute, {}));
        write_bytes(want[5], macho(kMachoExecute, {}));
        fs::create_symlink("libfx.1.dylib", k / "lib" / "libfx.dylib");
        write_bytes(k / "lib" / "fx.o", macho(1, {})); // MH_OBJECT
        put(k / "lib" / "libfx.a", "!<arch>\n");
        put(k / "bin" / "fx-config", "#!/bin/sh\n");
        CHECK(keg_macho_files(k) == want);
    }

    TEST_CASE("verify_keg_macho reports versioned keg paths and unresolved loads") {
        Prefix px;
        const auto& k = px.ctx.keg;
        write_bytes(k / "lib" / "libfx.1.dylib",
                    macho(kMachoDylib, {{kLcIdDylib, px.opt + "/lib/libfx.1.dylib"},
                                        {kLcLoadDylib, "/usr/lib/libSystem.B.dylib"}}));
        const auto bin = k / "bin" / "fx";
        write_bytes(bin, macho(kMachoExecute, {{kLcLoadDylib, "@loader_path/../lib/libfx.1.dylib"},
                                               {kLcLoadDylib, "@rpath/libfx.1.dylib"},
                                               {kLcLoadWeakDylib, "@rpath/missing-weak.dylib"},
                                               {kLcRpath, "@loader_path/../lib"}}));
        CHECK(verify_keg_macho(px.ctx, false).empty());

        const auto bad = k / "libexec" / "fx-core" / "fx-helper";
        const auto dep = (px.store / "depa" / "1.0" / "lib" / "libdepa.dylib").string();
        put(px.store / "depa" / "1.0" / "lib" / "libdepa.dylib", "x");
        write_bytes(bad, macho(kMachoExecute, {{kLcLoadDylib, dep},
                                               {kLcLoadDylib, "@rpath/libfx.1.dylib"},
                                               {kLcLoadDylib, px.opt + "/lib/libfx.1.dylib"}}));
        const auto problems = verify_keg_macho(px.ctx, false);
        CHECK(problems.size() == 3);
        CHECK(has_problem(problems, bad, "versioned keg path: " + dep));
        CHECK(has_problem(problems, bad, "cannot resolve dylib load: @rpath/libfx.1.dylib"));
        // The keg's own opt link does not exist yet.
        CHECK(has_problem(problems, bad, "cannot resolve dylib load: " + px.opt));
    }

#ifdef __APPLE__

    TEST_CASE("fixup_keg_macho: a fixed keg loads after its opt link and its dependency move") {
        if (run_cmd("command -v cc").rc != 0) {
            MESSAGE("no cc; skipping");
            return;
        }
        FxTmp tmp;
        const auto store = tmp.path / "Cellar";
        const auto src = tmp.path / "src";
        const auto ctx = macho_fix_context(store, "fx", "1.0");
        const auto k = ctx.keg;
        const auto opt_depa = tmp.path / "opt" / "depa";

        build_dep(store, src / "depa", "1.0");
        point(opt_depa, "../Cellar/depa/1.0");
        build_fx(store, src / "fx-1.0", "1.0");

        // As built, bin/fx needs fx's opt link, which does not exist yet.
        CHECK(run_cmd(q(k / "bin" / "fx")).rc != 0);
        REQUIRE_FALSE(verify_keg_macho(ctx, true).empty());

        CHECK(fixup_keg_macho(ctx) == 5); // everything but libfxutil (@rpath id, no loads)

        CHECK(verify_keg_macho(ctx, true).empty());
        const auto paths = all_load_paths(k);
        INFO("load paths:\n"
             << [&] {
                    std::string s;
                    for (const auto& p : paths) {
                        s += "  " + p + "\n";
                    }
                    return s;
                }());
        for (const auto& p : paths) {
            CHECK(p.find(store.string()) == std::string::npos);
        }
        const auto fx_paths = macho_load_paths(k / "bin" / "fx");
        CHECK(std::find(fx_paths.begin(), fx_paths.end(), "@loader_path/../lib/libfx.1.dylib") !=
              fx_paths.end());
        const auto lib_paths = macho_load_paths(k / "lib" / "libfx.1.dylib");
        CHECK(std::find(lib_paths.begin(), lib_paths.end(),
                        (opt_depa / "lib" / "libdepa.dylib").string()) != lib_paths.end());
        CHECK(macho_load_paths(k / "lib" / "ossl-modules" / "legacy.dylib").front() ==
              (ctx.opt / "lib" / "ossl-modules" / "legacy.dylib").string());

        const auto check_runs = [&](const std::string& fx, const std::string& depa) {
            const std::string want = "fx " + fx + " + depa " + depa + "\n";
            auto r = run_cmd(q(k / "bin" / "fx"));
            CHECK_MESSAGE(r.rc == 0, r.out);
            CHECK(r.out == want);
            r = run_cmd(q(k / "libexec" / "fx-core" / "fx-helper"));
            CHECK_MESSAGE(r.rc == 0, r.out);
            CHECK(r.out == "helper fx " + fx + " + depa " + depa + " fxutil " + fx + "\n");
            r = run_cmd(q(k / "bin" / "fx") + " " + q(k / "lib" / "engines-3" / "padlock.dylib"));
            CHECK_MESSAGE(r.rc == 0, r.out);
            CHECK(r.out == want + want);
            r = run_cmd(q(k / "bin" / "fx") + " " + q(k / "lib" / "ossl-modules" / "legacy.dylib"));
            CHECK_MESSAGE(r.rc == 0, r.out);
            CHECK(r.out == want + "fxutil " + fx + "\n");
        };

        SUBCASE("no opt link") {
            check_runs("1.0", "1.0");
        }
        SUBCASE("opt link on this keg, then moved to a newer one") {
            point(ctx.opt, "../Cellar/fx/1.0");
            check_runs("1.0", "1.0");
            build_fx(store, src / "fx-2.0", "2.0");
            point(ctx.opt, "../Cellar/fx/2.0");
            check_runs("1.0", "1.0"); // still its own libraries
        }
        SUBCASE("dependency opt link moved on") {
            build_dep(store, src / "depa", "1.1");
            point(opt_depa, "../Cellar/depa/1.1");
            fs::remove_all(store / "depa" / "1.0");
            check_runs("1.0", "1.1"); // dependencies follow their opt link
        }
    }

    TEST_CASE("build_from_source fixes the Mach-O linkage of an opt-prefix build") {
        if (run_cmd("command -v cc").rc != 0) {
            MESSAGE("no cc; skipping");
            return;
        }
        FxTmp tmp;
        Config cfg;
        cfg.den_home = tmp.path / "home";
        cfg.cache = cfg.den_home / "cache";
        cfg.set_prefix(tmp.path / "prefix", PrefixSource::EnvDenPrefix);
        fs::create_directories(cfg.store);
        const std::string name = "den-fixture-macho";

        const auto src = tmp.path / "src" / "machofx-1.0";
        put(src / "fx.c", "const char* fx(void) { return \"fx 1.0\"; }\n");
        put(src / "main.c", "#include <stdio.h>\nconst char* fx(void);\n"
                            "int main(void) { printf(\"%s\\n\", fx()); return 0; }\n");
        put(src / "Makefile",
            "all: libfx.1.dylib fx\n"
            "libfx.1.dylib: fx.c\n"
            "\tcc -dynamiclib -install_name $(PREFIX)/lib/libfx.1.dylib -o libfx.1.dylib fx.c\n"
            "fx: main.c libfx.1.dylib\n"
            "\tcc -o fx main.c libfx.1.dylib\n"
            "install: all\n"
            "\tmkdir -p $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/bin "
            "$(DESTDIR)$(PREFIX)/libexec/fx-core\n"
            "\tcp libfx.1.dylib $(DESTDIR)$(PREFIX)/lib/libfx.1.dylib\n"
            "\tcp fx $(DESTDIR)$(PREFIX)/bin/fx\n"
            "\tcp fx $(DESTDIR)$(PREFIX)/libexec/fx-core/fx-helper\n");
        const auto archive = tmp.path / "machofx-1.0.tar.xz";
        REQUIRE(
            run_cmd("tar -cJf " + q(archive) + " -C " + q(tmp.path / "src") + " machofx-1.0").rc ==
            0);
        const auto formula = tmp.path / (name + ".rb");
        put(formula, "class DenFixtureMacho < Formula\n"
                     "  desc \"Mach-O fix-up fixture\"\n"
                     "  homepage \"https://example.invalid/machofx\"\n"
                     "  url \"file://" +
                         archive.string() + "\"\n  sha256 \"" + hash_file(archive) +
                         "\"\n\n  def install\n"
                         "    system \"make\", \"install\", \"PREFIX=#{prefix}\"\n"
                         "  end\nend\n");

        PackageIndex idx;
        Package pkg;
        pkg.name = name;
        pkg.version = "1.0";
        pkg.ruby_source_path = formula.string();
        idx.packages[name] = pkg;

        const auto keg = build_from_source(cfg, idx, name, "1.0");
        REQUIRE(keg == cfg.store / name / "1.0");
        const auto ctx = macho_fix_context(cfg.store, name, "1.0");
        CHECK(verify_keg_macho(ctx, true).empty());
        CHECK(macho_load_paths(keg / "lib" / "libfx.1.dylib").front() ==
              (ctx.opt / "lib" / "libfx.1.dylib").string());
        // No opt link exists, yet both executables load the keg's library.
        REQUIRE_FALSE(fs::exists(fs::symlink_status(ctx.opt)));
        for (const auto& exe : {keg / "bin" / "fx", keg / "libexec" / "fx-core" / "fx-helper"}) {
            const auto r = run_cmd(q(exe));
            CHECK_MESSAGE(r.rc == 0, r.out);
            CHECK(r.out == "fx 1.0\n");
        }
    }

    TEST_CASE("fixup_keg_macho fails when a dylib load does not resolve") {
        if (run_cmd("command -v cc").rc != 0) {
            MESSAGE("no cc; skipping");
            return;
        }
        FxTmp tmp;
        const auto ctx = macho_fix_context(tmp.path / "Cellar", "fx", "1.0");
        const auto gone = tmp.path / "elsewhere" / "libgone.dylib";
        fs::create_directories(gone.parent_path());
        fs::create_directories(ctx.keg / "bin");
        put(tmp.path / "gone.c", "int gone(void) { return 1; }\n");
        put(tmp.path / "main.c", "int gone(void);\nint main(void) { return gone() - 1; }\n");
        cc("-dynamiclib -install_name " + q(gone) + " -o " + q(gone) + " " +
           q(tmp.path / "gone.c"));
        cc("-o " + q(ctx.keg / "bin" / "fx") + " " + q(tmp.path / "main.c") + " " + q(gone));
        fs::remove(gone);
        CHECK_THROWS_WITH_AS(
            fixup_keg_macho(ctx),
            doctest::Contains("bin/fx: cannot resolve dylib load: " + gone.string()), UserError);
    }

#endif // __APPLE__
}

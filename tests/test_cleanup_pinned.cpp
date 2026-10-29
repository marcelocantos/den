// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Regression: `den cleanup` decided reachability from environment manifests
// alone. A bottle poured with its load commands pinned to a dependency's
// versioned keg (<Cellar>/libx/1.0/lib/libx.dylib) keeps loading that exact
// version after libx is upgraded, so deleting the now-unreferenced libx 1.0
// keg broke the dependent at dyld time. Cleanup now scans the Mach-O load
// commands, ids and rpaths of every keg that stays and keeps any den keg they
// still name by versioned path.

#include <doctest.h>

#include "build/macho_scan.h"
#include "download/sha256.h"
#include "env/manifest.h"
#include "store/store.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct PinTmp {
    fs::path path;
    PinTmp() {
        std::string tmpl = fs::temp_directory_path() / "den_cleanup_pin_XXXXXX";
        char* r = ::mkdtemp(tmpl.data());
        if (!r) {
            throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
        }
        path = fs::path(r).lexically_normal();
    }
    ~PinTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    PinTmp(const PinTmp&) = delete;
    PinTmp& operator=(const PinTmp&) = delete;
};

void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << s;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

using Bytes = std::vector<unsigned char>;

void le32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<unsigned char>(v >> (8 * i)));
    }
}

constexpr uint32_t kLcIdDylib = 0xd;
constexpr uint32_t kLcLoadDylib = 0xc;
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
    return h;
}

void put_macho(const fs::path& p, uint32_t filetype, const std::vector<Lc>& cmds) {
    fs::create_directories(p.parent_path());
    const auto b = macho(filetype, cmds);
    std::ofstream(p, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

fs::path owned_keg(const fs::path& cellar, const std::string& name, const std::string& version) {
    const auto keg = cellar / name / version;
    fs::create_directories(keg);
    mark_keg_owned(keg, name, version);
    return keg;
}

struct Scratch {
    PinTmp tmp;
    fs::path cellar = tmp.path / "Cellar";
    fs::path home = tmp.path / "home";
    fs::path cache = home / "cache";
};

bool kept(const CleanupReport& r, const std::string& name, const std::string& version) {
    for (const auto& k : r.kept_referenced) {
        if (k.keg.name == name && k.keg.version == version) {
            return true;
        }
    }
    return false;
}

bool removed(const CleanupReport& r, const std::string& name, const std::string& version) {
    for (const auto& p : r.removed) {
        if (p.name == name && p.version == version) {
            return true;
        }
    }
    return false;
}

} // namespace

// Synthetic Mach-O headers: macho_scan parses them on any host.
TEST_SUITE("cleanup::pinned") {

    TEST_CASE("a keg a surviving dependent loads by versioned path is kept and reported") {
        Scratch s;
        const auto libx10 = owned_keg(s.cellar, "libx", "1.0");
        const auto libx11 = owned_keg(s.cellar, "libx", "1.1");
        const auto app = owned_keg(s.cellar, "app", "1.0");
        const auto pinned = (libx10 / "lib" / "libx.dylib").string();
        put_macho(libx10 / "lib" / "libx.dylib", kMachoDylib, {{kLcIdDylib, pinned}});
        put_macho(libx11 / "lib" / "libx.dylib", kMachoDylib,
                  {{kLcIdDylib, (libx11 / "lib" / "libx.dylib").string()}});
        put_macho(app / "bin" / "app", kMachoExecute, {{kLcLoadDylib, pinned}});

        Manifest m;
        m.packages["homebrew"]["libx"] = "1.1";
        m.packages["homebrew"]["app"] = "1.0";
        write_manifest(s.home, "/", m);

        SUBCASE("dry-run") {
            auto r = cleanup_kegs(s.cellar, s.home, s.cache, /*dry_run=*/true);
            CHECK(r.removed.empty());
            REQUIRE(r.kept_referenced.size() == 1);
            CHECK(fs::exists(libx10 / "lib" / "libx.dylib"));
        }
        SUBCASE("for real") {
            auto r = cleanup_kegs(s.cellar, s.home, s.cache, /*dry_run=*/false);
            CHECK(r.removed.empty());
            REQUIRE(r.kept_referenced.size() == 1);
            const auto& k = r.kept_referenced[0];
            CHECK(k.keg.name == "libx");
            CHECK(k.keg.version == "1.0");
            CHECK(k.by.name == "app");
            CHECK(k.by.version == "1.0");
            CHECK(k.file == app / "bin" / "app");
            CHECK(k.load_path == pinned);
            CHECK(fs::exists(libx10 / "lib" / "libx.dylib"));
            CHECK(fs::exists(libx11));
            CHECK(fs::exists(app));
        }
    }

    TEST_CASE("an LC_RPATH or LC_ID_DYLIB naming a versioned keg keeps it too") {
        Scratch s;
        owned_keg(s.cellar, "libr", "1.0");
        owned_keg(s.cellar, "libi", "2.0");
        owned_keg(s.cellar, "libn", "3.0");
        const auto app = owned_keg(s.cellar, "app", "1.0");
        put_macho(app / "bin" / "app", kMachoExecute,
                  {{kLcLoadDylib, "@rpath/libr.dylib"},
                   {kLcRpath, (s.cellar / "libr" / "1.0" / "lib").string()}});
        // A dylib that kept a dependency's versioned id (as a copied-in
        // library does) still points dyld's consumers at that keg.
        put_macho(app / "lib" / "libcopy.dylib", kMachoDylib,
                  {{kLcIdDylib, (s.cellar / "libi" / "2.0" / "lib" / "libi.dylib").string()}});

        Manifest m;
        m.packages["homebrew"]["app"] = "1.0";
        write_manifest(s.home, "/", m);

        auto r = cleanup_kegs(s.cellar, s.home, s.cache, /*dry_run=*/false);
        CHECK(kept(r, "libr", "1.0"));
        CHECK(kept(r, "libi", "2.0"));
        CHECK(fs::exists(s.cellar / "libr" / "1.0"));
        CHECK(fs::exists(s.cellar / "libi" / "2.0"));
        // Nothing names libn: it goes as before.
        CHECK(removed(r, "libn", "3.0"));
        CHECK_FALSE(fs::exists(s.cellar / "libn" / "3.0"));
    }

    TEST_CASE("what a kept keg loads by versioned path is kept transitively") {
        Scratch s;
        const auto libx = owned_keg(s.cellar, "libx", "1.0");
        owned_keg(s.cellar, "libz", "1.0");
        const auto app = owned_keg(s.cellar, "app", "1.0");
        const auto libz = (s.cellar / "libz" / "1.0" / "lib" / "libz.dylib").string();
        put_macho(libx / "lib" / "libx.dylib", kMachoDylib,
                  {{kLcIdDylib, (libx / "lib" / "libx.dylib").string()}, {kLcLoadDylib, libz}});
        put_macho(app / "bin" / "app", kMachoExecute,
                  {{kLcLoadDylib, (libx / "lib" / "libx.dylib").string()}});
        Manifest m;
        m.packages["homebrew"]["app"] = "1.0";
        write_manifest(s.home, "/", m);

        auto r = cleanup_kegs(s.cellar, s.home, s.cache, /*dry_run=*/false);
        CHECK(r.removed.empty());
        CHECK(kept(r, "libx", "1.0"));
        CHECK(kept(r, "libz", "1.0"));
        CHECK(fs::exists(s.cellar / "libz" / "1.0"));
    }

    TEST_CASE("unreferenced kegs are still removed: self ids, opt loads, stale dependents, text") {
        Scratch s;
        // libx 1.0's own id names its versioned path; that does not keep it.
        const auto libx10 = owned_keg(s.cellar, "libx", "1.0");
        put_macho(libx10 / "lib" / "libx.dylib", kMachoDylib,
                  {{kLcIdDylib, (libx10 / "lib" / "libx.dylib").string()}});
        owned_keg(s.cellar, "libx", "1.1");
        // A surviving dependent that loads libx through opt (the stable path).
        const auto app = owned_keg(s.cellar, "app", "2.0");
        put_macho(app / "bin" / "app", kMachoExecute,
                  {{kLcLoadDylib, (s.tmp.path / "opt" / "libx" / "lib" / "libx.dylib").string()}});
        // A versioned path in a text file is not a load command.
        put(app / "share" / "notes.txt", (libx10 / "lib" / "libx.dylib").string() + "\n");
        // A stale dependent pinning a stale dependency: both go together.
        const auto old_app = owned_keg(s.cellar, "app", "1.0");
        put_macho(old_app / "bin" / "app", kMachoExecute,
                  {{kLcLoadDylib, (libx10 / "lib" / "libx.dylib").string()}});

        Manifest m;
        m.packages["homebrew"]["libx"] = "1.1";
        m.packages["homebrew"]["app"] = "2.0";
        write_manifest(s.home, "/", m);

        auto r = cleanup_kegs(s.cellar, s.home, s.cache, /*dry_run=*/false);
        CHECK(r.kept_referenced.empty());
        CHECK(removed(r, "libx", "1.0"));
        CHECK(removed(r, "app", "1.0"));
        CHECK_FALSE(fs::exists(libx10));
        CHECK_FALSE(fs::exists(old_app));
        CHECK(fs::exists(s.cellar / "libx" / "1.1"));
        CHECK(fs::exists(app));
    }

    TEST_CASE("a Homebrew keg that pins a den keg keeps it as well") {
        Scratch s;
        const auto libx = owned_keg(s.cellar, "libx", "1.0");
        const auto brew = s.cellar / "brewtool" / "4.0";
        put_macho(brew / "bin" / "brewtool", kMachoExecute,
                  {{kLcLoadDylib, (libx / "lib" / "libx.dylib").string()}});

        auto r = cleanup_kegs(s.cellar, s.home, s.cache, /*dry_run=*/false);
        CHECK(r.kept_unowned == 1);
        CHECK(kept(r, "libx", "1.0"));
        CHECK(fs::exists(libx));
    }

    TEST_CASE("macho_pinned_kegs matches the store through a symlinked path") {
        Scratch s;
        // Load commands name the resolved store path (as a build in
        // /private/tmp would), while the caller reaches it through a link.
        const auto real = fs::canonical(s.tmp.path) / "Cellar";
        owned_keg(real, "libx", "1.0");
        const auto app = owned_keg(real, "app", "1.0");
        put_macho(app / "bin" / "app", kMachoExecute,
                  {{kLcLoadDylib, (real / "libx" / "1.0" / "lib" / "libx.dylib").string()}});
        const auto link = s.tmp.path / "link";
        fs::create_directory_symlink(fs::canonical(s.tmp.path), link);

        // The store as the caller sees it, via a symlinked parent.
        const auto via = link / "Cellar";
        std::vector<InstalledPackage> survivors{{"app", "1.0", via / "app" / "1.0"}};
        std::vector<InstalledPackage> candidates{{"libx", "1.0", via / "libx" / "1.0"}};
        auto k = macho_pinned_kegs(via, survivors, candidates);
        REQUIRE(k.size() == 1);
        CHECK(k[0].keg.name == "libx");
        CHECK(macho_pinned_kegs(via, survivors, {}).empty());
    }
}

#ifdef __APPLE__

// Real dyld: install libx 1.0 and a dependent pinned to Cellar/libx/1.0 with
// the built den, upgrade libx to 1.1, run `den cleanup`, and check that 1.0
// stays and the dependent still runs (and that an opt-linked one does not
// hold 1.0 back).
namespace {

struct Run {
    std::string out;
    int rc;
};

Run sh(const std::string& cmd) {
    std::string out;
    FILE* p = ::popen((cmd + " 2>&1").c_str(), "r");
    REQUIRE(p != nullptr);
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), p)) {
        out += buf.data();
    }
    const int st = ::pclose(p);
    return {out, WIFEXITED(st) ? WEXITSTATUS(st) : 128};
}

std::string q(const fs::path& p) {
    std::string out = "'";
    for (char c : p.string()) {
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return out + "'";
}

void cc(const std::string& args) {
    const auto r = sh("cc " + args);
    INFO("cc " << args << "\n" << r.out);
    REQUIRE(r.rc == 0);
}

Run run_den(const std::string& args, const fs::path& home, const fs::path& prefix) {
    return sh("HOME=" + q(home) + " DEN_HOME=" + q(home / "den") + " DEN_PREFIX=" + q(prefix) +
              " HOMEBREW_PREFIX=" + q(prefix) + " HOMEBREW_CELLAR=" + q(prefix / "Cellar") +
              " ./den " + args);
}

// Tar `root/<name>/<version>` as a bottle into den's archive cache; return
// its sha256.
std::string stage(const fs::path& home, const fs::path& root, const std::string& name) {
    const auto tarball = home / (name + ".tar.gz");
    const auto r = sh("tar -czf " + q(tarball) + " -C " + q(root) + " " + name);
    INFO(r.out);
    REQUIRE(r.rc == 0);
    const auto data = slurp(tarball);
    const auto sha = hash_string(data);
    put(home / "den" / "cache" / "archives" / (sha + ".tar.gz"), data);
    return sha;
}

struct IndexPkg {
    std::string name;
    std::string version;
    std::string sha;
    std::vector<std::string> deps;
};

void write_index(const fs::path& home, const std::vector<IndexPkg>& pkgs) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : pkgs) {
        arr.push_back({
            {"name", p.name},
            {"version", p.version},
            {"description", "test"},
            {"homepage", ""},
            {"license", ""},
            {"artifact_type", "binary"},
            {"deprecated", false},
            {"disabled", false},
            {"dependencies", p.deps},
            {"build_dependencies", nlohmann::json::array()},
            {"archives",
             {{"all",
               {{"url", "file:///nonexistent/" + p.name + ".tar.gz"},
                {"sha256", p.sha},
                {"relocation", "portable"}}}}},
        });
    }
    const auto idx = home / "den" / "cache" / "index.json";
    put(idx, nlohmann::json{{"packages", arr}}.dump(2));
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(idx.parent_path(), ec)) {
        const auto fn = e.path().filename().string();
        if (fn != "index.json" && fn.rfind("index", 0) == 0) {
            fs::remove(e.path(), ec);
        }
    }
}

// libx <version> as a bottle tree under `root`, with the given install name.
void build_libx(const fs::path& root, const fs::path& src, const std::string& version,
                const fs::path& install_name) {
    const auto lib = root / "libx" / version / "lib" / "libx.dylib";
    fs::create_directories(lib.parent_path());
    put(src / ("libx-" + version + ".c"),
        "const char* libx(void) { return \"libx " + version + "\"; }\n");
    cc("-dynamiclib -install_name " + q(install_name) + " -o " + q(lib) + " " +
       q(src / ("libx-" + version + ".c")));
}

// <name> 1.0 as a bottle tree under `root`, linked against `dylib` (whose
// install name decides what its load command names).
void build_app(const fs::path& root, const fs::path& src, const std::string& name,
               const fs::path& dylib) {
    const auto bin = root / name / "1.0" / "bin" / name;
    fs::create_directories(bin.parent_path());
    put(src / "app.c", "#include <stdio.h>\nconst char* libx(void);\n"
                       "int main(void) { printf(\"%s\\n\", libx()); return 0; }\n");
    cc("-o " + q(bin) + " " + q(src / "app.c") + " " + q(dylib));
}

} // namespace

TEST_SUITE("cleanup::pinned") {

    TEST_CASE("cleanup keeps libx 1.0 for a dependent pinned to it, and it still runs") {
        PinTmp t;
        const auto prefix = t.path / "prefix";
        const auto cellar = prefix / "Cellar";
        fs::create_directories(cellar);
        const auto src = t.path / "src";
        const auto bottles = t.path / "bottles";

        // libx 1.0 with a versioned id, as a pinned bottle has.
        const auto libx10 = cellar / "libx" / "1.0" / "lib" / "libx.dylib";
        build_libx(bottles, src, "1.0", libx10);
        const auto libx_sha10 = stage(t.path, bottles, "libx");
        write_index(t.path, {{"libx", "1.0", libx_sha10, {}}});
        auto i1 = run_den("install libx", t.path, prefix);
        INFO(i1.out);
        REQUIRE(i1.rc == 0);
        REQUIRE(fs::is_regular_file(libx10));

        // pinapp: linked against the installed keg, so it loads
        // Cellar/libx/1.0/lib/libx.dylib by versioned path.
        build_app(bottles, src, "pinapp", libx10);
        const auto pin_sha = stage(t.path, bottles, "pinapp");
        write_index(t.path,
                    {{"libx", "1.0", libx_sha10, {}}, {"pinapp", "1.0", pin_sha, {"libx"}}});
        auto i2 = run_den("install pinapp", t.path, prefix);
        INFO(i2.out);
        REQUIRE(i2.rc == 0);
        const auto pinapp = cellar / "pinapp" / "1.0" / "bin" / "pinapp";
        {
            const auto loads = macho_load_paths(pinapp);
            CHECK(std::find(loads.begin(), loads.end(), libx10.string()) != loads.end());
            const auto r = sh(q(pinapp));
            CHECK(r.out == "libx 1.0\n");
        }

        // Upgrade libx to 1.1.
        fs::remove_all(bottles / "libx");
        build_libx(bottles, src, "1.1", cellar / "libx" / "1.1" / "lib" / "libx.dylib");
        const auto libx_sha11 = stage(t.path, bottles, "libx");
        write_index(t.path,
                    {{"libx", "1.1", libx_sha11, {}}, {"pinapp", "1.0", pin_sha, {"libx"}}});
        auto u = run_den("upgrade libx", t.path, prefix);
        INFO(u.out);
        REQUIRE(u.rc == 0);
        REQUIRE(fs::is_directory(cellar / "libx" / "1.1"));
        REQUIRE(fs::equivalent(prefix / "opt" / "libx", cellar / "libx" / "1.1"));

        auto dry = run_den("cleanup --dry-run", t.path, prefix);
        INFO(dry.out);
        CHECK(dry.rc == 0);
        CHECK(dry.out.find("Would keep libx 1.0: pinapp 1.0 still loads it (bin/pinapp -> " +
                           libx10.string() + ")") != std::string::npos);
        CHECK(dry.out.find("Would remove libx 1.0") == std::string::npos);

        auto c = run_den("cleanup", t.path, prefix);
        INFO(c.out);
        CHECK(c.rc == 0);
        CHECK(c.out.find("Keeping libx 1.0: pinapp 1.0 still loads it") != std::string::npos);
        CHECK(c.out.find("Removing libx 1.0") == std::string::npos);
        CHECK(fs::is_regular_file(libx10));
        CHECK(fs::is_directory(cellar / "libx" / "1.1"));
        CHECK(fs::equivalent(prefix / "opt" / "libx", cellar / "libx" / "1.1"));

        // The dependent still runs, against the version it is pinned to.
        const auto r = sh(q(pinapp));
        CHECK(r.rc == 0);
        CHECK(r.out == "libx 1.0\n");
    }

    TEST_CASE("cleanup removes libx 1.0 when its dependent loads it through opt") {
        PinTmp t;
        const auto prefix = t.path / "prefix";
        const auto cellar = prefix / "Cellar";
        fs::create_directories(cellar);
        const auto src = t.path / "src";
        const auto bottles = t.path / "bottles";
        const auto opt_lib = prefix / "opt" / "libx" / "lib" / "libx.dylib";

        // libx bottles with the stable opt id, as Homebrew relocates them.
        build_libx(bottles, src, "1.0", opt_lib);
        const auto libx_sha10 = stage(t.path, bottles, "libx");
        write_index(t.path, {{"libx", "1.0", libx_sha10, {}}});
        auto i1 = run_den("install libx", t.path, prefix);
        INFO(i1.out);
        REQUIRE(i1.rc == 0);
        REQUIRE(fs::is_regular_file(opt_lib));

        build_app(bottles, src, "optapp", opt_lib);
        const auto app_sha = stage(t.path, bottles, "optapp");
        write_index(t.path,
                    {{"libx", "1.0", libx_sha10, {}}, {"optapp", "1.0", app_sha, {"libx"}}});
        auto i2 = run_den("install optapp", t.path, prefix);
        INFO(i2.out);
        REQUIRE(i2.rc == 0);
        const auto optapp = cellar / "optapp" / "1.0" / "bin" / "optapp";
        CHECK(sh(q(optapp)).out == "libx 1.0\n");

        fs::remove_all(bottles / "libx");
        build_libx(bottles, src, "1.1", opt_lib);
        const auto libx_sha11 = stage(t.path, bottles, "libx");
        write_index(t.path,
                    {{"libx", "1.1", libx_sha11, {}}, {"optapp", "1.0", app_sha, {"libx"}}});
        auto u = run_den("upgrade libx", t.path, prefix);
        INFO(u.out);
        REQUIRE(u.rc == 0);

        auto c = run_den("cleanup", t.path, prefix);
        INFO(c.out);
        CHECK(c.rc == 0);
        CHECK(c.out.find("Removing libx 1.0") != std::string::npos);
        CHECK(c.out.find("Keeping") == std::string::npos);
        CHECK_FALSE(fs::exists(cellar / "libx" / "1.0"));

        // It follows opt to 1.1.
        const auto r = sh(q(optapp));
        CHECK(r.rc == 0);
        CHECK(r.out == "libx 1.1\n");
    }
}

#endif // __APPLE__

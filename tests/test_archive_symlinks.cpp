// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// extract_archive symlink policy: legitimate Homebrew bottle links are
// accepted, escapes are not. The bottle cases mirror real links found in
// python@3.15, certifi, llvm, llvm@22, powershell and fastlane bottles (see
// the PR that introduced SymlinkScope::BottlePrefix).

#include <doctest.h>

#include "core/error.h"
#include "download/archive.h"

#include <archive.h>
#include <archive_entry.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace den {
namespace test {

namespace fs = std::filesystem;

namespace {

struct Tmp {
    fs::path path;
    Tmp() {
        path = fs::temp_directory_path() /
               ("den_arsym_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    Tmp(const Tmp&) = delete;
    Tmp& operator=(const Tmp&) = delete;
};

struct Entry {
    enum Kind { File, Dir, Symlink, Hardlink } kind;
    std::string path;
    std::string arg; // file content, symlink target or hardlink target
};

Entry file(std::string p, std::string content = "x\n") {
    return {Entry::File, std::move(p), std::move(content)};
}
Entry dir(std::string p) {
    return {Entry::Dir, std::move(p), ""};
}
Entry link(std::string p, std::string target) {
    return {Entry::Symlink, std::move(p), std::move(target)};
}
Entry hardlink(std::string p, std::string target) {
    return {Entry::Hardlink, std::move(p), std::move(target)};
}

/// Write a ustar archive with exactly these entries, in order. Unlike `tar`
/// on a staging tree this can express hostile layouts (entries under a
/// symlink, duplicates, links created out of order).
fs::path make_tar(const fs::path& out, const std::vector<Entry>& entries) {
    struct archive* a = archive_write_new();
    archive_write_set_format_pax_restricted(a);
    REQUIRE(archive_write_open_filename(a, out.c_str()) == ARCHIVE_OK);
    for (const auto& e : entries) {
        struct archive_entry* ae = archive_entry_new();
        archive_entry_set_pathname(ae, e.path.c_str());
        archive_entry_set_mtime(ae, 1700000000, 0);
        switch (e.kind) {
        case Entry::File:
            archive_entry_set_filetype(ae, AE_IFREG);
            archive_entry_set_perm(ae, 0644);
            archive_entry_set_size(ae, static_cast<la_int64_t>(e.arg.size()));
            break;
        case Entry::Dir:
            archive_entry_set_filetype(ae, AE_IFDIR);
            archive_entry_set_perm(ae, 0755);
            break;
        case Entry::Symlink:
            archive_entry_set_filetype(ae, AE_IFLNK);
            archive_entry_set_perm(ae, 0755);
            archive_entry_set_symlink(ae, e.arg.c_str());
            break;
        case Entry::Hardlink:
            archive_entry_set_filetype(ae, AE_IFREG);
            archive_entry_set_perm(ae, 0644);
            archive_entry_set_hardlink(ae, e.arg.c_str());
            break;
        }
        REQUIRE(archive_write_header(a, ae) == ARCHIVE_OK);
        if (e.kind == Entry::File && !e.arg.empty()) {
            archive_write_data(a, e.arg.data(), e.arg.size());
        }
        archive_entry_free(ae);
    }
    archive_write_close(a);
    archive_write_free(a);
    return out;
}

ExtractOptions bottle() {
    ExtractOptions o;
    o.symlink_scope = SymlinkScope::BottlePrefix;
    return o;
}

bool tree_is_empty(const fs::path& p) {
    return !fs::exists(p) || fs::is_empty(p);
}

} // namespace

TEST_SUITE("archive::symlink_policy") {

    // ---- legitimate links ------------------------------------------------

    TEST_CASE("in-keg relative links (bin/share, framework Versions/Current chain)") {
        Tmp t;
        auto ar = make_tar(t.path / "a.tar",
                           {
                               dir("python@3.15/3.15.2/"),
                               file("python@3.15/3.15.2/bin/python3.15"),
                               link("python@3.15/3.15.2/bin/python3", "python3.15"),
                               link("python@3.15/3.15.2/share/man/py", "../../bin/python3"),
                               dir("python@3.15/3.15.2/Frameworks/Python.framework/Versions/3.15/"
                                   "Headers/"),
                               link("python@3.15/3.15.2/Frameworks/Python.framework/Versions/"
                                    "Current",
                                    "3.15"),
                               link("python@3.15/3.15.2/Frameworks/Python.framework/Headers",
                                    "Versions/Current/Headers"),
                           });
        auto dest = t.path / "Cellar";
        for (auto scope : {SymlinkScope::Dest, SymlinkScope::BottlePrefix}) {
            fs::remove_all(dest);
            ExtractOptions o;
            o.symlink_scope = scope;
            CHECK_NOTHROW(extract_archive(ar, dest, o));
            CHECK(fs::is_symlink(dest / "python@3.15/3.15.2/share/man/py"));
            CHECK(
                fs::is_directory(dest / "python@3.15/3.15.2/Frameworks/Python.framework/Headers"));
        }
    }

    TEST_CASE("llvm: xctoolchain usr/bin -> ../../../bin stays in keg") {
        Tmp t;
        auto ar = make_tar(
            t.path / "a.tar",
            {
                dir("llvm/23.1.4/bin/"),
                link("llvm/23.1.4/Toolchains/LLVM23.1.4.xctoolchain/usr/bin", "../../../bin"),
            });
        CHECK_NOTHROW(extract_archive(ar, t.path / "Cellar"));
    }

    TEST_CASE("bottle links into $HOMEBREW_PREFIX/etc and /opt (real python, llvm, "
              "powershell, fastlane links)") {
        Tmp t;
        auto ar = make_tar(
            t.path / "a.tar",
            {
                link("python@3.15/3.15.2/lib/python3.15/site-packages/pip/_vendor/certifi/"
                     "cacert.pem",
                     "../../../../../../../../../etc/ca-certificates/cert.pem"),
                link("certifi/2026.10.23/lib/python3.15/site-packages/certifi/cacert.pem",
                     "../../../../../../../etc/ca-certificates/cert.pem"),
                link("llvm/23.1.4/etc/clang", "../../../../etc/clang"),
                link("powershell/7.6.9_1/libexec/ref/Microsoft.CSharp.dll",
                     "../../../../../opt/dotnet/libexec/packs/Microsoft.NETCore.App.Ref/10.0.14/"
                     "ref/net10.0/Microsoft.CSharp.dll"),
                link("fastlane/2.241.2/libexec/gems/terminal-notifier-3.1.0/vendor/"
                     "terminal-notifier/terminal-notifier.app",
                     "../../../../../../../../opt/terminal-notifier/terminal-notifier.app"),
            });
        auto dest = t.path / "prefix" / "Cellar";
        CHECK_NOTHROW(extract_archive(ar, dest, bottle()));
        CHECK(fs::read_symlink(dest / "llvm/23.1.4/etc/clang") == "../../../../etc/clang");
        // Nothing was created at prefix level: the links are just dangling
        // until the prefix provides etc/ and opt/.
        CHECK_FALSE(fs::exists(t.path / "prefix" / "etc"));
        CHECK_FALSE(fs::exists(t.path / "prefix" / "opt"));
    }

    TEST_CASE("prefix-level links are rejected outside bottle scope (casks, tarballs)") {
        Tmp t;
        auto ar =
            make_tar(t.path / "a.tar", {link("llvm/23.1.4/etc/clang", "../../../../etc/clang")});
        CHECK_THROWS_AS(extract_archive(ar, t.path / "stage"), ArchiveError);
    }

    // ---- escapes ---------------------------------------------------------

    TEST_CASE("absolute symlink targets are rejected in every scope") {
        Tmp t;
        for (std::string target : {"/etc/passwd", "/opt/homebrew/etc/clang", "/"}) {
            auto ar = make_tar(t.path / "a.tar", {link("pkg/1.0/evil", target)});
            CHECK_THROWS_AS(extract_archive(ar, t.path / "d1"), ArchiveError);
            CHECK_THROWS_AS(extract_archive(ar, t.path / "d2", bottle()), ArchiveError);
        }
    }

    TEST_CASE(".. escapes beyond the prefix, or to non-standard prefix dirs, are rejected") {
        Tmp t;
        for (std::string target : {
                 "../../../../../etc/passwd",    // two levels above dest
                 "../../../../../../../../../x", // way above
                 "../../..",                     // the prefix itself
                 "../../../Cellar/other/1.0/x",  // not a standard prefix dir
                 "../../../Library/x",           //
                 "../../../etc/../../x",         // climbs back out via etc
                 "../../../opt/dep/../etc/x",    // .. inside the prefix: opt/dep is a
                 "../../../opt/dep/../../x",     // real symlink, so lexical .. lies
             }) {
            CAPTURE(target);
            auto ar = make_tar(t.path / "a.tar", {link("pkg/1.0/evil", target)});
            CHECK_THROWS_AS(extract_archive(ar, t.path / "prefix" / "Cellar", bottle()),
                            ArchiveError);
        }
    }

    TEST_CASE("entry written through an archive symlink is rejected (absolute and ..)") {
        Tmp t;
        auto victim = t.path / "victim";
        fs::create_directories(victim);
        // Even a symlink that is itself in scope must not be written through.
        for (auto [target, scope] : std::vector<std::pair<std::string, SymlinkScope>>{
                 {victim.string(), SymlinkScope::BottlePrefix},
                 {"../../../etc", SymlinkScope::BottlePrefix},
                 {"../lib", SymlinkScope::Dest},
             }) {
            CAPTURE(target);
            auto dest = t.path / "prefix" / "Cellar";
            fs::remove_all(t.path / "prefix");
            auto ar = make_tar(t.path / "a.tar", {
                                                     dir("pkg/1.0/lib/"),
                                                     link("pkg/1.0/evil", target),
                                                     file("pkg/1.0/evil/payload", "pwned\n"),
                                                 });
            ExtractOptions o;
            o.symlink_scope = scope;
            CHECK_THROWS_AS(extract_archive(ar, dest, o), ArchiveError);
            CHECK(tree_is_empty(victim));
            CHECK_FALSE(fs::exists(t.path / "prefix" / "etc"));
            CHECK_FALSE(fs::exists(dest / "pkg/1.0/lib/payload"));
        }
    }

    TEST_CASE("directory entry replacing a symlink is rejected (no chmod through link)") {
        Tmp t;
        auto ar = make_tar(t.path / "a.tar", {
                                                 link("pkg/1.0/etc", "../../../etc"),
                                                 dir("pkg/1.0/etc/"),
                                             });
        CHECK_THROWS_AS(extract_archive(ar, t.path / "prefix" / "Cellar", bottle()), ArchiveError);
        CHECK_FALSE(fs::exists(t.path / "prefix" / "etc"));
    }

    TEST_CASE("file entry replacing an existing symlink is rejected") {
        Tmp t;
        auto ar = make_tar(t.path / "a.tar", {
                                                 file("pkg/1.0/real"),
                                                 link("pkg/1.0/l", "real"),
                                                 file("pkg/1.0/l", "overwrite\n"),
                                             });
        CHECK_THROWS_AS(extract_archive(ar, t.path / "d"), ArchiveError);
    }

    TEST_CASE("symlink chain escape: target traverses a previously extracted symlink") {
        Tmp t;
        auto dest = t.path / "prefix" / "Cellar";
        // pkg/1.0/e -> ../../../etc is fine on its own (prefix/etc), but
        // e/../../x follows e first and lands above the prefix.
        auto ar = make_tar(t.path / "a.tar", {
                                                 link("pkg/1.0/e", "../../../etc"),
                                                 link("pkg/1.0/b", "e/../../x"),
                                             });
        CHECK_THROWS_AS(extract_archive(ar, dest, bottle()), ArchiveError);

        // Same in Dest scope with an in-tree hop: d -> sub/up where up -> ../../..
        auto ar2 = make_tar(t.path / "b.tar", {
                                                  dir("pkg/1.0/sub/"),
                                                  link("pkg/1.0/sub/up", "../../.."),
                                                  link("pkg/1.0/d", "sub/up/../x"),
                                              });
        CHECK_THROWS_AS(extract_archive(ar2, t.path / "stage"), ArchiveError);
    }

    TEST_CASE("symlink created later cannot redirect an earlier link out of scope") {
        Tmp t;
        // When a is checked, b does not exist so b/../.. stays at dest.
        // Once b -> ../../../opt exists, a really resolves above the prefix.
        auto ar = make_tar(t.path / "a.tar", {
                                                 link("pkg/1.0/a", "b/../../../.."),
                                                 link("pkg/1.0/b", "../../../opt"),
                                             });
        CHECK_THROWS_AS(extract_archive(ar, t.path / "prefix" / "Cellar", bottle()), ArchiveError);
    }

    TEST_CASE("symlink loops are rejected") {
        Tmp t;
        auto ar = make_tar(t.path / "a.tar", {
                                                 link("pkg/1.0/a", "b"),
                                                 link("pkg/1.0/b", "a"),
                                                 link("pkg/1.0/c", "a/x"),
                                             });
        CHECK_THROWS_AS(extract_archive(ar, t.path / "d"), ArchiveError);
    }

    TEST_CASE("hardlink through a symlink is rejected") {
        Tmp t;
        auto victim_dir = t.path / "victim";
        fs::create_directories(victim_dir);
        {
            std::ofstream(victim_dir / "secret") << "s\n";
        }
        auto ar = make_tar(t.path / "a.tar", {
                                                 link("pkg/1.0/v", victim_dir.string()),
                                                 hardlink("pkg/1.0/alias", "pkg/1.0/v/secret"),
                                             });
        // The absolute link is rejected first; also check the hardlink path
        // alone with a pre-existing on-disk symlink in dest.
        CHECK_THROWS_AS(extract_archive(ar, t.path / "d1"), ArchiveError);

        auto dest = t.path / "d2";
        fs::create_directories(dest / "pkg/1.0");
        fs::create_directory_symlink(victim_dir, dest / "pkg/1.0/v");
        auto ar2 = make_tar(t.path / "b.tar", {hardlink("pkg/1.0/alias", "pkg/1.0/v/secret")});
        CHECK_THROWS_AS(extract_archive(ar2, dest), ArchiveError);
        CHECK_FALSE(fs::exists(dest / "pkg/1.0/alias"));
    }

    TEST_CASE("pre-existing on-disk symlink in dest is not written through") {
        Tmp t;
        auto victim = t.path / "victim";
        fs::create_directories(victim);
        auto dest = t.path / "d";
        fs::create_directories(dest);
        fs::create_directory_symlink(victim, dest / "pkg");
        auto ar = make_tar(t.path / "a.tar", {file("pkg/payload", "pwned\n")});
        CHECK_THROWS_AS(extract_archive(ar, dest), ArchiveError);
        CHECK(tree_is_empty(victim));
    }

    TEST_CASE("dest reached through a symlinked parent still extracts (macOS /tmp)") {
        Tmp t;
        fs::create_directories(t.path / "real");
        fs::create_directory_symlink(t.path / "real", t.path / "alias");
        auto ar = make_tar(t.path / "a.tar", {
                                                 file("pkg/1.0/bin/tool"),
                                                 link("pkg/1.0/share/tool", "../bin/tool"),
                                             });
        auto r = extract_archive(ar, t.path / "alias" / "dest");
        CHECK(r.root == "pkg");
        CHECK(fs::is_regular_file(t.path / "real" / "dest" / "pkg/1.0/bin/tool"));
        CHECK(fs::is_symlink(t.path / "real" / "dest" / "pkg/1.0/share/tool"));
    }
}

} // namespace test
} // namespace den

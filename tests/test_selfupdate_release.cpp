// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// `den self-update --version <tag>`: tag parsing, release-JSON → asset
// resolution (RCs ship GA-named assets), and the in-process install of a
// verified tarball (private mkdtemp work dir, libarchive extraction with no
// shell, trust replica created when missing). Nothing here touches the
// network or the running binary: the install tests target a scratch exe.

#include "../src/core/error.h"
#include "../src/download/sha256.h"
#include "../src/selfupdate/selfupdate.h"

#include <doctest.h>

#include <archive.h>
#include <archive_entry.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace den {
namespace test_selfupdate_release {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& stem = "den_surel") {
        std::string tmpl = (fs::temp_directory_path() / (stem + "_XXXXXX")).string();
        if (!::mkdtemp(tmpl.data()))
            throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
        path = fs::canonical(tmpl);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << body;
}

// Write a .tar.gz of regular files (name → contents) with libarchive.
void make_tarball(const fs::path& out,
                  const std::vector<std::pair<std::string, std::string>>& files) {
    archive* a = archive_write_new();
    REQUIRE(a);
    archive_write_add_filter_gzip(a);
    archive_write_set_format_pax_restricted(a);
    REQUIRE(archive_write_open_filename(a, out.c_str()) == ARCHIVE_OK);
    for (const auto& [name, body] : files) {
        archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, name.c_str());
        archive_entry_set_size(e, static_cast<la_int64_t>(body.size()));
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        REQUIRE(archive_write_header(a, e) == ARCHIVE_OK);
        REQUIRE(archive_write_data(a, body.data(), body.size()) ==
                static_cast<la_ssize_t>(body.size()));
        archive_entry_free(e);
    }
    archive_write_close(a);
    archive_write_free(a);
}

size_t count_entries(const fs::path& dir, const std::string& prefix) {
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().filename().string().starts_with(prefix))
            ++n;
    }
    return n;
}

TEST_SUITE("self-update --version") {
    TEST_CASE("normalize_release_tag accepts GA and RC, with or without v") {
        CHECK(normalize_release_tag("0.14.0") == "v0.14.0");
        CHECK(normalize_release_tag("v0.14.0") == "v0.14.0");
        CHECK(normalize_release_tag("0.15.0-rc.1") == "v0.15.0-rc.1");
        CHECK(normalize_release_tag("v0.15.0-rc.12") == "v0.15.0-rc.12");
    }

    TEST_CASE("normalize_release_tag rejects malformed versions") {
        for (const char* bad : {"", "v", "0.14", "latest", "0.14.0-beta", "vv0.14.0", "0.14.0-rc.",
                                "0.14.0/../x", "0.14.0 ", "0.14.0-rc.1;rm"}) {
            CAPTURE(bad);
            CHECK_THROWS_AS(normalize_release_tag(bad), UserError);
        }
    }

    TEST_CASE("asset_version_for_tag drops the v and any -rc.N") {
        CHECK(asset_version_for_tag("v0.15.0-rc.1") == "0.15.0");
        CHECK(asset_version_for_tag("v0.14.0") == "0.14.0");
        CHECK(asset_version_for_tag("0.14.0") == "0.14.0");
    }

    TEST_CASE("an RC release resolves to its GA-named tarball and .sha256") {
        const std::string json = R"({
            "tag_name": "v0.15.0-rc.1",
            "assets": [
                {"name": "den-0.15.0-checksums.txt", "browser_download_url": "https://x/sums"},
                {"name": "den-0.15.0-linux-x86_64.tar.gz", "browser_download_url": "https://x/lx"},
                {"name": "den-0.15.0-darwin-aarch64.tar.gz", "browser_download_url": "https://x/da"},
                {"name": "den-0.15.0-darwin-aarch64.tar.gz.sha256",
                 "browser_download_url": "https://x/da.sha256"},
                {"name": 7},
                "junk"
            ]
        })";
        auto info = update_info_from_release_json(json, "darwin-aarch64");
        REQUIRE(info.has_value());
        CHECK(info->tag == "v0.15.0-rc.1");
        CHECK(info->latest_version == "0.15.0");
        CHECK(info->download_url == "https://x/da");
        CHECK(info->checksum_url == "https://x/da.sha256");

        // linux-x86_64 has a tarball but no .sha256: resolved, and
        // apply_update later refuses the empty checksum_url (fail closed).
        auto lx = update_info_from_release_json(json, "linux-x86_64");
        REQUIRE(lx.has_value());
        CHECK(lx->checksum_url.empty());

        CHECK_FALSE(update_info_from_release_json(json, "linux-aarch64").has_value());
    }

    TEST_CASE("malformed release JSON yields no update") {
        CHECK_FALSE(update_info_from_release_json("not json", "darwin-aarch64").has_value());
        CHECK_FALSE(update_info_from_release_json("[]", "darwin-aarch64").has_value());
        CHECK_FALSE(update_info_from_release_json(R"({"message":"Not Found"})", "darwin-aarch64")
                        .has_value());
    }

    TEST_CASE("make_private_temp_dir creates distinct private directories") {
        auto a = make_private_temp_dir("den_surel_mk");
        auto b = make_private_temp_dir("den_surel_mk");
        CHECK(a != b);
        CHECK(fs::is_directory(a));
        CHECK(fs::is_directory(b));
        struct stat st{};
        REQUIRE(::stat(a.c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == 0700);
        fs::remove_all(a);
        fs::remove_all(b);
    }

    TEST_CASE("install_update_tarball replaces exe and creates a missing trust replica") {
        // Spaces and shell metacharacters in every path: extraction is
        // in-process, so nothing is ever handed to a shell.
        TempDir t("den surel 'q' $x");
        auto tarball = t.path / "up date's.tar.gz";
        make_tarball(tarball, {{"den", "#!/bin/sh\necho new\n"},
                               {"share/den/known_hashes.json", "{\"v\":\"new\"}\n"}});
        auto exe = t.path / "home dir" / ".den" / "bin" / "den";
        write_file(exe, "old");
        auto replica = t.path / "home dir" / ".den" / "share" / "den" / "known_hashes.json";
        REQUIRE_FALSE(fs::exists(replica));

        const auto tmp = fs::temp_directory_path();
        const auto before = count_entries(tmp, "den-self-update-x.");
        install_update_tarball(tarball, exe);

        CHECK(slurp(exe) == "#!/bin/sh\necho new\n");
        CHECK((fs::status(exe).permissions() & fs::perms::owner_exec) != fs::perms::none);
        CHECK(slurp(replica) == "{\"v\":\"new\"}\n");
        CHECK_FALSE(fs::exists(fs::path(replica.string() + ".new")));
        // The private work dir is cleaned up.
        CHECK(count_entries(tmp, "den-self-update-x.") == before);
    }

    TEST_CASE("install_update_tarball swaps in a fresh inode and leaves the old one intact") {
        // A running daemon is mapped from the old inode; rewriting it in
        // place would invalidate its code signature (macOS kills it with
        // OS_REASON_CODESIGNING). The update must land on a new inode.
        TempDir t;
        auto tarball = t.path / "up.tar.gz";
        make_tarball(tarball, {{"den", "new bytes"}});
        auto exe = t.path / "bin" / "den";
        write_file(exe, "old bytes");
        struct stat before{};
        REQUIRE(::stat(exe.c_str(), &before) == 0);
        std::ifstream held(exe, std::ios::binary); // stands in for the running process

        install_update_tarball(tarball, exe);

        struct stat after{};
        REQUIRE(::stat(exe.c_str(), &after) == 0);
        CHECK(after.st_ino != before.st_ino);
        CHECK(slurp(exe) == "new bytes");
        std::string old((std::istreambuf_iterator<char>(held)), std::istreambuf_iterator<char>());
        CHECK(old == "old bytes");
        // No staging leftovers beside the binary.
        CHECK(count_entries(exe.parent_path(), "den.new") == 0);
    }

    TEST_CASE("self-update keeps install.sh's installed-tag record in step with the binary") {
        // `den --version` omits -rc.N, so install.sh records the tag; a
        // self-update that left it alone made install.sh report the release
        // install.sh last installed (rc.1) instead of the one self-update
        // installed (rc.2).
        TempDir t;
        auto exe = t.path / ".den" / "bin" / "den";
        write_file(exe, "rc.2 bytes");
        auto record = t.path / ".den" / "share" / "den" / "installed-tag";
        write_file(record, "v0.15.0-rc.1\nsha256 " + hash_string("rc.1 bytes") + "\n");

        update_installed_tag_record(exe, "v0.15.0-rc.2");

        CHECK(slurp(record) == "v0.15.0-rc.2\nsha256 " + hash_string("rc.2 bytes") + "\n");
        CHECK_FALSE(fs::exists(fs::path(record.string() + ".new")));
    }

    TEST_CASE("self-update does not create an installed-tag record install.sh never wrote") {
        TempDir t;
        auto exe = t.path / "bin" / "den";
        write_file(exe, "bytes");
        update_installed_tag_record(exe, "v0.15.0-rc.2");
        CHECK_FALSE(fs::exists(t.path / "share" / "den" / "installed-tag"));
    }

    TEST_CASE("install_update_tarball refuses a tarball without den, leaving exe alone") {
        TempDir t;
        auto tarball = t.path / "bad.tar.gz";
        make_tarball(tarball, {{"README", "nope"}});
        auto exe = t.path / "bin" / "den";
        write_file(exe, "old");
        CHECK_THROWS_AS(install_update_tarball(tarball, exe), UserError);
        CHECK(slurp(exe) == "old");
    }

    TEST_CASE("install_update_tarball refuses path-traversal entries") {
        TempDir t;
        auto tarball = t.path / "evil.tar.gz";
        make_tarball(tarball, {{"../escaped", "x"}, {"den", "new"}});
        auto exe = t.path / "bin" / "den";
        write_file(exe, "old");
        CHECK_THROWS_AS(install_update_tarball(tarball, exe), UserError);
        CHECK(slurp(exe) == "old");
    }
}

} // namespace test_selfupdate_release
} // namespace den

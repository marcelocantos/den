// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "portable_ruby.h"
#include "portable_ruby_manifest.h"

#include "../core/error.h"
#include "../download/archive.h"
#include "../download/fetch.h"
#include "../platform/platform.h"

#include <spdlog/spdlog.h>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <fstream>
#include <string>

namespace den {

namespace {

struct PortableRubyAsset {
    const char* url;
    const char* sha256;
};

PortableRubyAsset asset_for_host() {
    // Only Linux is served by this path — macOS uses the embedded bundle.
    switch (detect_arch()) {
    case Arch::Arm64:
        return {kPortableRubyArm64LinuxUrl, kPortableRubyArm64LinuxSha256};
    case Arch::X86_64:
        return {kPortableRubyX8664LinuxUrl, kPortableRubyX8664LinuxSha256};
    }
    throw InternalError("no Portable Ruby asset for the current architecture");
}

} // namespace

fs::path ensure_portable_ruby_linux(const fs::path& den_home) {
    auto ruby_dir = den_home / "ruby";
    auto marker = ruby_dir / ".unpacked";

    // Fast path: already unpacked.
    if (fs::exists(marker)) {
        return ruby_dir;
    }

    // Lock to prevent concurrent unpacking. Uses the same lock file as
    // the macOS embedded path so the two are mutually exclusive.
    auto lock_path = den_home / "ruby.lock";
    fs::create_directories(den_home);
    int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        throw InternalError("cannot create lock file: " + lock_path.string());
    }
    ::flock(fd, LOCK_EX);

    // Double-check after acquiring lock.
    if (fs::exists(marker)) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return ruby_dir;
    }

    auto asset = asset_for_host();
    SPDLOG_INFO("fetching Portable Ruby {} from {}", kPortableRubyVersion, asset.url);

    // Step 1: fetch the bottle into the content-addressed cache.
    auto cache_dir = den_home / "cache" / "portable-ruby";
    fs::path archive = fetch_archive(asset.url, asset.sha256, cache_dir);

    // Step 2: extract into a scratch directory, then atomically move the
    // inner `portable-ruby/<version>` tree to `ruby_dir/ruby` so the on-disk
    // layout matches the macOS embedded bundle
    // (`den_home/ruby/ruby/bin/ruby`).
    auto scratch = den_home / "ruby.scratch";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);

    extract_archive(archive, scratch);

    auto inner = scratch / "portable-ruby" / kPortableRubyVersion;
    if (!fs::exists(inner)) {
        fs::remove_all(scratch, ec);
        ::flock(fd, LOCK_UN);
        ::close(fd);
        throw ArchiveError("Portable Ruby bottle missing expected path: " + inner.string());
    }

    fs::create_directories(ruby_dir);
    auto target = ruby_dir / "ruby";
    fs::remove_all(target, ec);
    fs::rename(inner, target);
    fs::remove_all(scratch, ec);

    // Step 3: make the ruby binary executable (libarchive preserves mode,
    // but belt-and-braces — matches the embedded path's behaviour).
    auto ruby_bin = target / "bin" / "ruby";
    if (fs::exists(ruby_bin)) {
        fs::permissions(ruby_bin,
                        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add);
    }

    // Step 4: write the marker.
    {
        std::ofstream f(marker);
        f << "unpacked (portable-ruby " << kPortableRubyVersion << ")\n";
    }

    ::flock(fd, LOCK_UN);
    ::close(fd);

    SPDLOG_INFO("Portable Ruby {} installed at {}", kPortableRubyVersion, target.string());
    return ruby_dir;
}

} // namespace den

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T72 verification harness — in-use file detection.
//
// Tests whether the daemon's in-use detector (not yet implemented) can
// identify files under a fake Cellar path that are held open by a live child
// process.  On macOS the expected implementation uses lsof; on Linux it
// reads /proc/<pid>/maps.  All tests SKIP when the detector is absent.
//
// Upstream gap: src/daemon/ has no is_file_in_use() or equivalent surface.
// These tests will SKIP until 🎯T51 lands that surface.

#include <doctest.h>

// --------------------------------------------------------------------------
// Compile-time feature probe.
// --------------------------------------------------------------------------

// When the in-use detector is implemented it should expose a header at one
// of these paths.  We probe at compile time and set HAS_INUSE_DETECTOR.
#if __has_include("daemon/inuse.h")
#  include "daemon/inuse.h"
#  define HAS_INUSE_DETECTOR 1
#elif __has_include("daemon/file_inuse.h")
#  include "daemon/file_inuse.h"
#  define HAS_INUSE_DETECTOR 1
#else
#  define HAS_INUSE_DETECTOR 0
#endif

#include <filesystem>
#include <fstream>
#include <string>

#if !defined(_WIN32)
#  include <fcntl.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace den {
namespace test {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// RAII temp directory.
struct InuseTmpDir {
    fs::path path;

    InuseTmpDir() {
        path = fs::temp_directory_path() /
               ("den_test_inuse_" + std::to_string(static_cast<unsigned long>(::getpid())));
        fs::create_directories(path);
    }

    ~InuseTmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    InuseTmpDir(const InuseTmpDir&) = delete;
    InuseTmpDir& operator=(const InuseTmpDir&) = delete;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_SUITE("inuse_detection") {

    TEST_CASE("detector reports file held open by child process") {
#if HAS_INUSE_DETECTOR == 0
        MESSAGE("in-use detector not yet implemented (upstream gap: 🎯T51) — skipping");
        return;
#elif defined(_WIN32)
        MESSAGE("POSIX-only test — skipping on Windows");
        return;
#else
        InuseTmpDir tmp;

        // Build a fake Cellar path: <tmp>/Cellar/openssl/3.2.0/lib/libssl.dylib
        fs::path cellar_root = tmp.path / "Cellar";
        fs::path keg = cellar_root / "openssl" / "3.2.0" / "lib";
        fs::create_directories(keg);
        fs::path target_file = keg / "libssl.dylib";
        { std::ofstream f(target_file); f << "fake library\n"; }

        // Fork a child that opens the file and blocks until we signal it.
        // Parent↔child synchronisation via a pipe: parent writes one byte
        // when ready to check, child exits after reading it.
        int sync_pipe[2];
        REQUIRE(::pipe(sync_pipe) == 0);

        pid_t child = ::fork();
        REQUIRE(child >= 0);

        if (child == 0) {
            // Child: open the target file, signal readiness, wait for parent.
            ::close(sync_pipe[1]); // close write end
            int fd = ::open(target_file.c_str(), O_RDONLY);
            if (fd < 0) { ::_exit(1); }
            // Signal parent: file is now open.
            // (child is ready when it successfully opened the file.)
            // We block until parent closes the write end (EOF on read).
            char buf;
            (void)::read(sync_pipe[0], &buf, 1); // wait for parent signal
            ::close(fd);
            ::close(sync_pipe[0]);
            ::_exit(0);
        }

        // Parent.
        ::close(sync_pipe[0]); // close read end

        // Give child a moment to open the file.
        // (A short usleep is acceptable here — no daemon sleep pattern.)
        ::usleep(50'000); // 50 ms

        // ---- Probe the detector ----
        bool in_use = den::is_file_in_use(target_file);
        CHECK(in_use);

        // Signal child to exit and reap.
        ::close(sync_pipe[1]);
        int status = 0;
        ::waitpid(child, &status, 0);

        // After child exits the file should no longer be in use.
        // (Allow a moment for the OS to update lsof/proc maps.)
        ::usleep(100'000); // 100 ms
        bool still_in_use = den::is_file_in_use(target_file);
        CHECK_FALSE(still_in_use);
#endif
    }

    TEST_CASE("detector reports false for file with no open handles") {
#if HAS_INUSE_DETECTOR == 0
        MESSAGE("in-use detector not yet implemented (upstream gap: 🎯T51) — skipping");
        return;
#elif defined(_WIN32)
        MESSAGE("POSIX-only test — skipping on Windows");
        return;
#else
        InuseTmpDir tmp;
        fs::path idle_file = tmp.path / "idle.dylib";
        { std::ofstream f(idle_file); f << "idle\n"; }

        CHECK_FALSE(den::is_file_in_use(idle_file));
#endif
    }

    TEST_CASE("detector returns false for nonexistent path") {
#if HAS_INUSE_DETECTOR == 0
        MESSAGE("in-use detector not yet implemented (upstream gap: 🎯T51) — skipping");
        return;
#elif defined(_WIN32)
        MESSAGE("POSIX-only test — skipping on Windows");
        return;
#else
        InuseTmpDir tmp;
        CHECK_FALSE(den::is_file_in_use(tmp.path / "no_such_file.dylib"));
#endif
    }

    TEST_CASE("files_in_use_under scans a Cellar keg for held-open files") {
#if HAS_INUSE_DETECTOR == 0
        MESSAGE("in-use detector not yet implemented (upstream gap: 🎯T51) — skipping");
        return;
#elif defined(_WIN32)
        MESSAGE("POSIX-only test — skipping on Windows");
        return;
#else
        InuseTmpDir tmp;
        fs::path cellar_root = tmp.path / "Cellar";
        fs::path keg = cellar_root / "curl" / "8.5.0" / "lib";
        fs::create_directories(keg);
        fs::path lib_a = keg / "libcurl.dylib";
        fs::path lib_b = keg / "libcurl.4.dylib";
        { std::ofstream f(lib_a); f << "a\n"; }
        { std::ofstream f(lib_b); f << "b\n"; }

        int sync_pipe[2];
        REQUIRE(::pipe(sync_pipe) == 0);

        pid_t child = ::fork();
        REQUIRE(child >= 0);

        if (child == 0) {
            ::close(sync_pipe[1]);
            int fd = ::open(lib_a.c_str(), O_RDONLY);
            if (fd < 0) { ::_exit(1); }
            char buf;
            (void)::read(sync_pipe[0], &buf, 1);
            ::close(fd);
            ::close(sync_pipe[0]);
            ::_exit(0);
        }

        ::close(sync_pipe[0]);
        ::usleep(50'000);

        // Bulk scanner: returns paths under the keg that are in use.
        auto held = den::files_in_use_under(cellar_root / "curl" / "8.5.0");
        CHECK(!held.empty());
        bool found_a = false;
        for (const auto& p : held) {
            if (p == lib_a) { found_a = true; }
        }
        CHECK(found_a);

        ::close(sync_pipe[1]);
        int status = 0;
        ::waitpid(child, &status, 0);
#endif
    }

} // TEST_SUITE

} // namespace test
} // namespace den

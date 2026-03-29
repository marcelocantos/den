// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "manifest.h"

#include "../core/error.h"

#include <spdlog/spdlog.h>

#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace den {

namespace detail {

/// RAII wrapper for flock-based file locking.
class FileLock {
  public:
    explicit FileLock(const fs::path& path) : fd_(::open(path.c_str(), O_RDWR | O_CREAT, 0644)) {
        if (fd_ < 0) {
            throw InternalError("failed to open lock file " + path.string() + ": " +
                                std::strerror(errno));
        }
        if (::flock(fd_, LOCK_EX) < 0) {
            ::close(fd_);
            throw InternalError("failed to lock " + path.string() + ": " + std::strerror(errno));
        }
    }

    ~FileLock() {
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
    }

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

  private:
    int fd_;
};

/// Return the manifest directory for a given env path.
inline fs::path manifest_dir(const fs::path& den_home, const std::string& env_path) {
    return den_home / "manifests" / env_slug(env_path);
}

} // namespace detail

template <typename F>
auto with_manifest(const fs::path& den_home, const std::string& env_path, F&& fn)
    -> decltype(fn(std::declval<Manifest&>())) {
    auto dir = detail::manifest_dir(den_home, env_path);
    fs::create_directories(dir);

    auto lock_path = dir / ".lock";
    detail::FileLock lock(lock_path);

    auto manifest = read_manifest(den_home, env_path);
    auto result = fn(manifest);
    write_manifest(den_home, env_path, manifest);
    return result;
}

} // namespace den

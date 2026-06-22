// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "inuse.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <system_error>
#include <utility>

#if defined(__APPLE__)
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#elif defined(__linux__)
#include <cctype>
#include <fstream>
#endif

namespace den {

namespace {

// Resolve a path to its canonical form for comparison. Falls back to a
// lexically-normal absolute form if the file vanished mid-scan (transient
// kegs/symlinks) so we still produce a stable key.
fs::path canonical_or_normal(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    if (!ec && !c.empty())
        return c;
    return p.lexically_normal();
}

// Collect every regular file (following the directory tree, not symlinks out
// of it) under a keg root. Defensive against permission errors and races: a
// failed entry is skipped, not fatal.
std::vector<fs::path> regular_files_under(const fs::path& keg_root) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (!fs::exists(keg_root, ec) || ec)
        return files;

    fs::recursive_directory_iterator it(keg_root, fs::directory_options::skip_permission_denied,
                                        ec);
    if (ec) {
        SPDLOG_DEBUG("inuse: cannot iterate {}: {}", keg_root.string(), ec.message());
        return files;
    }
    fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            SPDLOG_DEBUG("inuse: iteration error under {}: {}", keg_root.string(), ec.message());
            ec.clear();
            continue;
        }
        std::error_code sec;
        if (it->is_regular_file(sec) && !sec)
            files.push_back(it->path());
    }
    return files;
}

#if defined(__APPLE__)

// Run lsof over a batch of file paths and return the set of canonical paths it
// reports as open by at least one live process. lsof field output (`-F n`)
// emits one record per open file; we only care about the `n` (name) field.
//
// Defensive notes:
//   - posix_spawnp failure (lsof missing) → empty set (treat as "unknown").
//   - lsof exits non-zero when *none* of the listed files are open; that is
//     the common case, not an error, so we still parse whatever it printed.
//   - We cap argument count per invocation to stay well under ARG_MAX.
std::set<fs::path> lsof_open_names(const std::vector<fs::path>& paths) {
    std::set<fs::path> open;
    if (paths.empty())
        return open;

    constexpr size_t kBatch = 512; // well under typical ARG_MAX

    for (size_t base = 0; base < paths.size(); base += kBatch) {
        size_t end = std::min(base + kBatch, paths.size());

        // Build argv: lsof -n -P -w -F n -- <paths...>
        //   -n: no host resolution, -P: no port names (faster, irrelevant here)
        //   -w: suppress warning messages
        //   -F n: machine-readable, name field only
        std::vector<std::string> argstore;
        argstore.reserve((end - base) + 6);
        argstore.emplace_back("lsof");
        argstore.emplace_back("-n");
        argstore.emplace_back("-P");
        argstore.emplace_back("-w");
        argstore.emplace_back("-Fn");
        argstore.emplace_back("--");
        for (size_t i = base; i < end; ++i)
            argstore.push_back(paths[i].string());

        std::vector<char*> argv;
        argv.reserve(argstore.size() + 1);
        for (auto& s : argstore)
            argv.push_back(s.data());
        argv.push_back(nullptr);

        int pipefd[2];
        if (::pipe(pipefd) != 0) {
            SPDLOG_DEBUG("inuse: pipe() failed: {}", std::strerror(errno));
            return open; // give up defensively
        }

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        // Child stdout → pipe write end; stderr → /dev/null.
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);

        pid_t pid = 0;
        int rc = ::posix_spawnp(&pid, "lsof", &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(pipefd[1]); // parent never writes

        if (rc != 0) {
            SPDLOG_WARN("inuse: cannot spawn lsof: {} — assuming files not in use",
                        std::strerror(rc));
            ::close(pipefd[0]);
            return open;
        }

        // Drain child stdout.
        std::string out;
        std::array<char, 4096> buf{};
        for (;;) {
            ssize_t n = ::read(pipefd[0], buf.data(), buf.size());
            if (n > 0) {
                out.append(buf.data(), static_cast<size_t>(n));
            } else if (n == 0) {
                break; // EOF
            } else if (errno == EINTR) {
                continue;
            } else {
                SPDLOG_DEBUG("inuse: read from lsof failed: {}", std::strerror(errno));
                break;
            }
        }
        ::close(pipefd[0]);

        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            // retry on interruption
        }

        // Parse `n<path>` lines. Any name lsof emits corresponds to an open
        // handle held by some process, so its presence means "in use".
        size_t pos = 0;
        while (pos < out.size()) {
            size_t nl = out.find('\n', pos);
            std::string line =
                out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? out.size() : nl + 1;
            if (!line.empty() && line[0] == 'n') {
                std::string name = line.substr(1);
                if (!name.empty())
                    open.insert(canonical_or_normal(fs::path(name)));
            }
        }
    }
    return open;
}

#elif defined(__linux__)

// On Linux, scan /proc/<pid>/maps and /proc/<pid>/fd for references to the
// requested files. Returns the set of canonical paths held open by any pid.
//
// Defensive notes:
//   - /proc may be unreadable for processes owned by other users; those pids
//     are skipped, not fatal.
//   - PIDs are transient: a directory may vanish between listing and reading.
std::set<fs::path> proc_open_names(const std::set<fs::path>& targets) {
    std::set<fs::path> open;
    if (targets.empty())
        return open;

    std::error_code ec;
    fs::directory_iterator proc("/proc", ec);
    if (ec) {
        SPDLOG_DEBUG("inuse: cannot read /proc: {}", ec.message());
        return open;
    }
    fs::directory_iterator end;
    for (; proc != end; proc.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const std::string fname = proc->path().filename().string();
        if (fname.empty() || !std::all_of(fname.begin(), fname.end(),
                                          [](unsigned char c) { return std::isdigit(c); }))
            continue; // not a pid directory

        // 1) /proc/<pid>/maps — mmap'd files (shared libraries, binaries).
        {
            std::ifstream maps(proc->path() / "maps");
            std::string line;
            while (std::getline(maps, line)) {
                // Format: addr perms offset dev inode pathname
                // pathname starts after the inode column; find the first '/'.
                size_t slash = line.find(" /");
                if (slash == std::string::npos)
                    continue;
                std::string path = line.substr(slash + 1);
                if (path.empty())
                    continue;
                fs::path canon = canonical_or_normal(fs::path(path));
                if (targets.count(canon))
                    open.insert(canon);
            }
        }

        // 2) /proc/<pid>/fd/* — open file descriptors (symlinks to files).
        std::error_code fec;
        fs::directory_iterator fds(proc->path() / "fd", fec);
        if (fec)
            continue;
        fs::directory_iterator fend;
        for (; fds != fend; fds.increment(fec)) {
            if (fec) {
                fec.clear();
                continue;
            }
            std::error_code lec;
            fs::path tgt = fs::read_symlink(fds->path(), lec);
            if (lec)
                continue;
            fs::path canon = canonical_or_normal(tgt);
            if (targets.count(canon))
                open.insert(canon);
        }
    }
    return open;
}

#endif

} // namespace

bool is_file_in_use(const fs::path& file) {
    std::error_code ec;
    if (!fs::exists(file, ec) || ec)
        return false;

    fs::path canon = canonical_or_normal(file);

#if defined(__APPLE__)
    auto open = lsof_open_names({file});
    return open.count(canon) > 0;
#elif defined(__linux__)
    auto open = proc_open_names({canon});
    return open.count(canon) > 0;
#else
    SPDLOG_DEBUG("inuse: no in-use detector for this platform — assuming not in use");
    return false;
#endif
}

std::vector<fs::path> files_in_use_under(const fs::path& keg_root) {
    std::vector<fs::path> result;
    std::vector<fs::path> files = regular_files_under(keg_root);
    if (files.empty())
        return result;

    // Map canonical → original path so we can return the caller's view.
    std::vector<std::pair<fs::path, fs::path>> canon_to_orig;
    canon_to_orig.reserve(files.size());
    for (const auto& f : files)
        canon_to_orig.emplace_back(canonical_or_normal(f), f);

#if defined(__APPLE__)
    std::set<fs::path> open = lsof_open_names(files);
#elif defined(__linux__)
    std::set<fs::path> targets;
    for (const auto& [canon, orig] : canon_to_orig)
        targets.insert(canon);
    std::set<fs::path> open = proc_open_names(targets);
#else
    std::set<fs::path> open;
#endif

    if (open.empty())
        return result;

    std::set<fs::path> seen;
    for (const auto& [canon, orig] : canon_to_orig) {
        if (open.count(canon) && seen.insert(orig).second)
            result.push_back(orig);
    }
    return result;
}

} // namespace den

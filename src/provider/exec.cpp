// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "exec.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>

extern "C" char** environ;

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace den {

namespace {

// Build the child's environment as a NUL-terminated argv-style array. We start
// from the inherited environment, overlay `extra_env`, and (if requested)
// prepend `extra_path` to PATH. The returned strings own their storage via the
// `storage` vector, which the caller must keep alive for the lifetime of the
// returned pointer array.
std::vector<char*> build_environ(const std::map<std::string, std::string>& extra_env,
                                 const std::optional<std::string>& extra_path,
                                 std::vector<std::string>& storage) {
    std::map<std::string, std::string> merged;
    for (char** e = environ; e && *e; ++e) {
        std::string entry(*e);
        auto eq = entry.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        merged[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
    for (const auto& [k, v] : extra_env) {
        merged[k] = v;
    }
    if (extra_path) {
        auto it = merged.find("PATH");
        std::string base = (it == merged.end()) ? std::string() : it->second;
        merged["PATH"] = base.empty() ? *extra_path : (*extra_path + ":" + base);
    }

    storage.clear();
    storage.reserve(merged.size());
    for (const auto& [k, v] : merged) {
        storage.push_back(k + "=" + v);
    }

    std::vector<char*> ptrs;
    ptrs.reserve(storage.size() + 1);
    for (auto& s : storage) {
        ptrs.push_back(s.data());
    }
    ptrs.push_back(nullptr);
    return ptrs;
}

} // namespace

ToolResult run_tool(const std::vector<std::string>& argv,
                    const std::map<std::string, std::string>& extra_env,
                    const std::optional<std::string>& extra_path) {
    ToolResult result;
    if (argv.empty()) {
        return result;
    }

    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        SPDLOG_ERROR("run_tool: pipe() failed: {}", std::strerror(errno));
        return result;
    }

    // Route both stdout and stderr to the write end of the pipe.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    // argv → char* array (posix_spawn does not modify the strings, but the API
    // wants non-const pointers).
    std::vector<std::string> argv_storage = argv;
    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(argv_storage.size() + 1);
    for (auto& a : argv_storage) {
        argv_ptrs.push_back(a.data());
    }
    argv_ptrs.push_back(nullptr);

    std::vector<std::string> env_storage;
    auto env_ptrs = build_environ(extra_env, extra_path, env_storage);

    pid_t pid = 0;
    int spawn_rc = ::posix_spawnp(&pid, argv_storage[0].c_str(), &actions, nullptr,
                                  argv_ptrs.data(), env_ptrs.data());
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipefd[1]);

    if (spawn_rc != 0) {
        // ENOENT here means the toolchain is not installed — a graceful-skip
        // signal, not a hard error. Leave result.spawned == false.
        ::close(pipefd[0]);
        SPDLOG_DEBUG("run_tool: failed to spawn '{}': {}", argv_storage[0],
                     std::strerror(spawn_rc));
        return result;
    }
    result.spawned = true;

    std::array<char, 4096> buf{};
    ssize_t n = 0;
    while ((n = ::read(pipefd[0], buf.data(), buf.size())) > 0) {
        result.output.append(buf.data(), static_cast<size_t>(n));
    }
    ::close(pipefd[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

bool tool_available(const std::string& tool) {
    // `command -v` is portable across sh-compatible shells and resolves both
    // PATH executables and builtins; we only care about executables here.
    auto r = run_tool({"/bin/sh", "-c", "command -v \"$1\" >/dev/null 2>&1", "sh", tool});
    return r.spawned && r.exit_code == 0;
}

int run_tool_interactive(const std::vector<std::string>& argv,
                         const std::map<std::string, std::string>& extra_env,
                         const std::optional<std::string>& extra_path) {
    if (argv.empty()) {
        return -1;
    }

    std::vector<std::string> argv_storage = argv;
    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(argv_storage.size() + 1);
    for (auto& a : argv_storage) {
        argv_ptrs.push_back(a.data());
    }
    argv_ptrs.push_back(nullptr);

    std::vector<std::string> env_storage;
    auto env_ptrs = build_environ(extra_env, extra_path, env_storage);

    // No file actions: the child inherits this process's stdin/stdout/stderr.
    pid_t pid = 0;
    int spawn_rc = ::posix_spawnp(&pid, argv_storage[0].c_str(), nullptr, nullptr, argv_ptrs.data(),
                                  env_ptrs.data());
    if (spawn_rc != 0) {
        SPDLOG_DEBUG("run_tool_interactive: failed to spawn '{}': {}", argv_storage[0],
                     std::strerror(spawn_rc));
        return -1;
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

} // namespace den

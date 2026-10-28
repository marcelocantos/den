// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "launchd_restart.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace den {

namespace fs = std::filesystem;

namespace {

std::string sh_q(const std::string& s) {
    std::string q = "'";
    for (char c : s) {
        if (c == '\'')
            q += "'\\''";
        else
            q += c;
    }
    return q + "'";
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    return s.substr(i);
}

std::vector<char*> c_argv(std::vector<std::string>& v) {
    std::vector<char*> out;
    out.reserve(v.size() + 1);
    for (auto& s : v)
        out.push_back(s.data());
    out.push_back(nullptr);
    return out;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

} // namespace

ScrubbedEnv scrubbed_env_for_current_user() {
    ScrubbedEnv e;
    if (const passwd* pw = ::getpwuid(::getuid())) {
        if (pw->pw_dir)
            e.home = pw->pw_dir;
        if (pw->pw_name)
            e.user = pw->pw_name;
    }
    if (e.home.empty())
        if (const char* h = std::getenv("HOME"))
            e.home = h;
    if (e.user.empty())
        if (const char* u = std::getenv("USER"))
            e.user = u;
    return e;
}

std::vector<std::string> scrubbed_argv(const ScrubbedEnv& env, const std::string& tool,
                                       const std::vector<std::string>& args) {
    std::vector<std::string> argv = {"/usr/bin/env",
                                     "-i",
                                     "HOME=" + env.home,
                                     "USER=" + env.user,
                                     std::string("PATH=") + kScrubbedPath,
                                     tool};
    argv.insert(argv.end(), args.begin(), args.end());
    return argv;
}

SpawnResult run_scrubbed(const std::vector<std::string>& argv_in) {
    SpawnResult r;
    if (argv_in.empty())
        return r;
    std::vector<std::string> argv = argv_in;
    auto cargv = c_argv(argv);
    char* empty_env[] = {nullptr};

    int fds[2];
    if (::pipe(fds) != 0)
        return r;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    pid_t pid = 0;
    int rc = ::posix_spawn(&pid, cargv[0], &fa, nullptr, cargv.data(), empty_env);
    posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);
    if (rc != 0) {
        ::close(fds[0]);
        r.output = std::string("spawn failed: ") + std::strerror(rc);
        return r;
    }
    r.spawned = true;
    std::array<char, 4096> buf{};
    for (;;) {
        ssize_t n = ::read(fds[0], buf.data(), buf.size());
        if (n > 0) {
            r.output.append(buf.data(), static_cast<size_t>(n));
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    ::close(fds[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return r;
}

bool LaunchdJobState::codesign_failure() const {
    return upper(last_exit_reason).find("CODESIGN") != std::string::npos;
}

LaunchdJobState parse_launchctl_print(bool loaded, const std::string& text) {
    LaunchdJobState st;
    st.loaded = loaded;
    if (!loaded)
        return st;
    std::istringstream in(text);
    std::string line;
    int depth = 0;
    while (std::getline(in, line)) {
        // Only the job's own top-level keys (one tab in): nested dicts such
        // as endpoints carry their own "state" lines.
        auto t = trim(line);
        if (t.empty())
            continue;
        if (t.back() == '{') {
            ++depth;
            if (depth > 1)
                continue;
        }
        if (t == "}") {
            --depth;
            continue;
        }
        if (depth != 1)
            continue;
        auto eq = t.find(" = ");
        if (eq == std::string::npos)
            continue;
        auto key = t.substr(0, eq);
        auto val = t.substr(eq + 3);
        try {
            if (key == "pid") {
                st.pid = static_cast<pid_t>(std::stol(val));
            } else if (key == "runs") {
                st.runs = std::stoi(val);
            } else if (key == "last exit reason") {
                st.last_exit_reason = val;
            } else if (key == "last exit code" &&
                       upper(val).find("CODESIGN") != std::string::npos &&
                       st.last_exit_reason.empty()) {
                st.last_exit_reason = val;
            }
        } catch (const std::exception&) {
        }
    }
    return st;
}

std::string resolve_launchctl() {
    if (const char* path = std::getenv("PATH")) {
        std::string p = path;
        size_t start = 0;
        while (start <= p.size()) {
            size_t end = p.find(':', start);
            if (end == std::string::npos)
                end = p.size();
            auto dir = p.substr(start, end - start);
            if (!dir.empty() && dir.front() == '/') {
                auto cand = fs::path(dir) / "launchctl";
                if (::access(cand.c_str(), X_OK) == 0)
                    return cand.string();
            }
            start = end + 1;
        }
    }
    return "/bin/launchctl";
}

LaunchdOps real_launchd_ops(const std::string& label, const fs::path& plist,
                            std::string launchctl_path) {
    if (launchctl_path.empty())
        launchctl_path = resolve_launchctl();
    LaunchdOps ops;
    ops.domain = "gui/" + std::to_string(static_cast<unsigned>(::getuid()));
    ops.label = label;
    ops.plist = plist;
    const auto env = scrubbed_env_for_current_user();
    ops.launchctl = [env, launchctl_path](const std::vector<std::string>& args) {
        auto argv = scrubbed_argv(env, launchctl_path, args);
        SPDLOG_DEBUG("launchd restart: {}", [&] {
            std::string s;
            for (const auto& a : argv)
                s += (s.empty() ? "" : " ") + a;
            return s;
        }());
        return run_scrubbed(argv);
    };
    ops.sleep = [](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); };
    ops.now = [] { return std::chrono::steady_clock::now(); };
    return ops;
}

namespace {

LaunchdJobState query(const LaunchdOps& ops) {
    auto r = ops.launchctl({"print", ops.domain + "/" + ops.label});
    return parse_launchctl_print(r.spawned && r.exit_code == 0, r.output);
}

} // namespace

LaunchdRestartResult restart_launchd_job(const LaunchdOps& ops, std::optional<pid_t> old_pid,
                                         std::ostream& out) {
    const std::string service = ops.domain + "/" + ops.label;
    // bootout stops the old process and unregisters the job. "Not loaded" is
    // fine: bootstrap below loads it either way.
    (void)ops.launchctl({"bootout", service});

    // bootstrap can briefly fail (EIO, "Bootstrap failed: 5") while launchd is
    // still tearing the old instance down; retry a few times.
    SpawnResult boot;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        boot = ops.launchctl({"bootstrap", ops.domain, ops.plist.string()});
        if (boot.spawned && boot.exit_code == 0)
            break;
        if (attempt < 3)
            ops.sleep(std::chrono::milliseconds(1000));
    }
    if (!boot.spawned || boot.exit_code != 0) {
        out << "launchctl bootstrap " << ops.domain << " " << ops.plist.string() << " failed (exit "
            << boot.exit_code << "): " << trim(boot.output) << "\n";
        return LaunchdRestartResult::Failed;
    }

    // Confirm a new PID. Tolerate exactly one codesigning spawn failure:
    // kickstart once and keep waiting; a second one is a real failure.
    int codesign_failures = 0;
    std::optional<int> counted_runs;
    const auto deadline = ops.now() + ops.confirm_timeout;
    LaunchdJobState st;
    for (;;) {
        st = query(ops);
        if (st.pid && (!old_pid || *st.pid != *old_pid)) {
            if (codesign_failures == 0)
                return LaunchdRestartResult::Restarted;
            out << "The den daemon came back (pid " << *st.pid
                << ") after one codesigning spawn failure.\n";
            return LaunchdRestartResult::RestartedAfterCodesignRetry;
        }
        if (st.loaded && !st.pid && st.codesign_failure() &&
            (!counted_runs || st.runs != counted_runs)) {
            counted_runs = st.runs;
            ++codesign_failures;
            if (codesign_failures > 1) {
                out << "launchd refused to spawn " << ops.label
                    << " twice (last exit reason = " << st.last_exit_reason << ").\n";
                return LaunchdRestartResult::Failed;
            }
            (void)ops.launchctl({"kickstart", service});
        }
        if (ops.now() >= deadline)
            break;
        ops.sleep(std::chrono::milliseconds(500));
    }
    out << "launchd did not report a new pid for " << service << " within "
        << ops.confirm_timeout.count() << "s";
    if (!st.loaded)
        out << " (job not loaded)";
    else if (!st.last_exit_reason.empty())
        out << " (last exit reason = " << st.last_exit_reason << ")";
    out << ".\n";
    return LaunchdRestartResult::Failed;
}

std::string detached_restart_script(const std::string& launchctl_path, const ScrubbedEnv& env,
                                    const std::string& domain, const std::string& label,
                                    const fs::path& plist) {
    const std::string lc = "/usr/bin/env -i HOME=" + sh_q(env.home) + " USER=" + sh_q(env.user) +
                           " PATH=" + kScrubbedPath + " " + sh_q(launchctl_path);
    const std::string svc = sh_q(domain + "/" + label);
    std::ostringstream s;
    s << "echo \"den restart helper: pid $$ $(date)\"\n"
      // Let the caller finish reporting before its job is torn down.
      << "sleep 1\n"
      << lc << " bootout " << svc << "\n"
      << "i=0\n"
      << "until " << lc << " bootstrap " << sh_q(domain) << " " << sh_q(plist.string()) << "; do\n"
      << "  i=$((i + 1)); [ \"$i\" -ge 3 ] && { echo 'bootstrap failed'; exit 1; }\n"
      << "  sleep 1\n"
      << "done\n"
      << "t=0\n"
      << "while [ \"$t\" -lt " << kRestartConfirmTimeout.count() << " ]; do\n"
      << "  pid=$(" << lc << " print " << svc
      << " 2>/dev/null | sed -n 's/^[[:space:]]*pid = //p' | head -1)\n"
      << "  [ -n \"$pid\" ] && { echo \"restarted: pid $pid\"; exit 0; }\n"
      << "  sleep 1; t=$((t + 1))\n"
      << "done\n"
      << "echo 'no pid after restart'\n"
      << "exit 1\n";
    return s.str();
}

std::optional<pid_t> spawn_detached_restart(const std::string& script, const fs::path& log,
                                            const ScrubbedEnv& env) {
    // Everything the grandchild needs is prepared before fork: after fork
    // only async-signal-safe calls are made.
    std::vector<std::string> argv = {"/bin/sh", "-c", script};
    std::vector<std::string> envs = {"HOME=" + env.home, "USER=" + env.user,
                                     std::string("PATH=") + kScrubbedPath};
    auto cargv = c_argv(argv);
    auto cenv = c_argv(envs);
    const std::string log_path = log.string();

    int fds[2];
    if (::pipe(fds) != 0)
        return std::nullopt;
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    pid_t child = ::fork();
    if (child < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return std::nullopt;
    }
    if (child == 0) {
        ::close(fds[0]);
        if (::setsid() < 0)
            ::_exit(1);
        pid_t gc = ::fork();
        if (gc < 0)
            ::_exit(1);
        if (gc > 0) {
            // Report the grandchild's pid, then let it be reparented to launchd/init.
            (void)::write(fds[1], &gc, sizeof(gc));
            ::_exit(0);
        }
        ::chdir("/");
        int in = ::open("/dev/null", O_RDONLY);
        int lf = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (in >= 0)
            ::dup2(in, 0);
        if (lf >= 0) {
            ::dup2(lf, 1);
            ::dup2(lf, 2);
        }
        // Close everything else the caller had open (its sockets, locks).
        long maxfd = ::sysconf(_SC_OPEN_MAX);
        if (maxfd < 0 || maxfd > 4096)
            maxfd = 4096;
        for (int fd = 3; fd < maxfd; ++fd)
            if (fd != fds[1])
                ::close(fd);
        ::execve("/bin/sh", cargv.data(), cenv.data());
        ::_exit(127);
    }
    ::close(fds[1]);
    pid_t gc = -1;
    ssize_t n;
    do {
        n = ::read(fds[0], &gc, sizeof(gc));
    } while (n < 0 && errno == EINTR);
    ::close(fds[0]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (n != static_cast<ssize_t>(sizeof(gc)) || gc <= 0)
        return std::nullopt;
    return gc;
}

bool running_inside_launchd_job(const std::string& label) {
    const char* svc = std::getenv("XPC_SERVICE_NAME");
    return svc && label == svc;
}

} // namespace den

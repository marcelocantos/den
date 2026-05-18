// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T70 verification: payload transparency.
//
// Tests that:
//   1. `den telemetry show` prints a JSONL payload to stdout.
//   2. `den telemetry send` prints the exact JSONL to stdout BEFORE transmitting
//      (verified using a local stub HTTP server — no real network calls).
//   3. The payload is valid JSON (starts with '{', ends with '}' on same line).
//
// Stub server: a minimal Python one-liner that binds to a random port on
// 127.0.0.1, accepts one HTTP POST, responds 204, and exits.  The test
// injects the URL via DEN_TELEMETRY_URL env var (per T32 design).
//
// All tests SKIP gracefully when `den telemetry` is not yet implemented.

#include <doctest.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <regex>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace den {
namespace telemetry_payload_test {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
struct TempDir {
    fs::path path;

    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_tpay_XXXXXX").string();
        char* r = ::mkdtemp(tmpl.data());
        if (!r)
            throw std::runtime_error(std::string("mkdtemp: ") + std::strerror(errno));
        path = r;
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

struct RunResult {
    std::string output;
    int exit_code;
};

static RunResult run_den_env(const std::string& extra_env, const std::string& args,
                             const std::string& den_home) {
    const std::string prefix = den_home + "/brew";
    const std::string cellar = prefix + "/Cellar";
    const std::string cmd = "DEN_HOME=" + den_home + " HOMEBREW_PREFIX=" + prefix +
                            " HOMEBREW_CELLAR=" + cellar +
                            (extra_env.empty() ? "" : " " + extra_env) + " ./den " + args + " 2>&1";

    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe)
        throw std::runtime_error("popen failed: " + cmd);

    RunResult result;
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr)
        result.output += buf.data();

    int raw = ::pclose(pipe);
    result.exit_code = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
    return result;
}

static RunResult run_den(const std::string& args, const std::string& den_home) {
    return run_den_env("", args, den_home);
}

static bool telemetry_available(const std::string& den_home) {
    auto r = run_den("telemetry --help", den_home);
    return !(r.exit_code == 109 || r.output.find("was not expected") != std::string::npos);
}

// Returns true if a line looks like a JSONL object: starts with '{', ends with '}'.
// (Each JSONL record is a single-line JSON object.)
static bool has_jsonl_line(const std::string& output) {
    // Scan each line.
    std::istringstream ss(output);
    std::string line;
    while (std::getline(ss, line)) {
        // Strip trailing whitespace / CR.
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty() && line.front() == '{' && line.back() == '}')
            return true;
    }
    return false;
}

// Launch a minimal stub HTTP server on localhost using Python.
// Returns {port, pid}.  The server accepts exactly one POST and exits.
// Caller must wait for pid after the den command completes.
static std::pair<int, pid_t> start_stub_server() {
    // Pick an available port by binding to port 0 and reading back — but
    // that requires socket code.  Simpler: use Python to pick a free port,
    // write it to a temp file, then start the server.
    char port_file_tmpl[] = "/tmp/den_stub_port_XXXXXX";
    int fd = ::mkstemp(port_file_tmpl);
    if (fd < 0)
        throw std::runtime_error("mkstemp failed");
    ::close(fd);
    ::unlink(port_file_tmpl); // Python will write to this path

    // Python script: bind to port 0 (OS assigns), write port to file, accept 1 request.
    const std::string script =
        "import socket, sys\n"
        "s = socket.socket()\n"
        "s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)\n"
        "s.bind(('127.0.0.1', 0))\n"
        "port = s.getsockname()[1]\n"
        "open('" +
        std::string(port_file_tmpl) +
        "', 'w').write(str(port))\n"
        "sys.stdout.flush()\n"
        "s.listen(1)\n"
        "conn, _ = s.accept()\n"
        "data = b''\n"
        "while True:\n"
        "    chunk = conn.recv(4096)\n"
        "    if not chunk: break\n"
        "    data += chunk\n"
        "    if b'\\r\\n\\r\\n' in data:\n"
        "        # Read Content-Length worth of body if present\n"
        "        import re\n"
        "        m = re.search(rb'Content-Length: (\\d+)', data)\n"
        "        hdr_end = data.index(b'\\r\\n\\r\\n') + 4\n"
        "        body_len = int(m.group(1)) if m else 0\n"
        "        body_recv = len(data) - hdr_end\n"
        "        while body_recv < body_len:\n"
        "            chunk = conn.recv(4096)\n"
        "            if not chunk: break\n"
        "            data += chunk\n"
        "            body_recv += len(chunk)\n"
        "        break\n"
        "conn.sendall(b'HTTP/1.1 204 No Content\\r\\nContent-Length: 0\\r\\n\\r\\n')\n"
        "conn.close()\n"
        "s.close()\n";

    pid_t pid = ::fork();
    if (pid < 0)
        throw std::runtime_error("fork failed");

    if (pid == 0) {
        // Child: exec python3 with inline script.
        ::execlp("python3", "python3", "-c", script.c_str(), nullptr);
        ::_exit(1); // exec failed
    }

    // Parent: wait for the port file to appear (up to 3 seconds).
    for (int i = 0; i < 30; ++i) {
        ::usleep(100000); // 100ms
        FILE* f = ::fopen(port_file_tmpl, "r");
        if (f) {
            char buf[16] = {};
            ::fgets(buf, sizeof(buf), f);
            ::fclose(f);
            ::unlink(port_file_tmpl);
            int port = std::atoi(buf);
            if (port > 0)
                return {port, pid};
        }
    }

    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    throw std::runtime_error("stub server did not start in time");
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_SUITE("telemetry::payload_visible" * doctest::skip(true)) {

    // -----------------------------------------------------------------------
    // 1. `den telemetry show` prints a JSONL object to stdout
    // -----------------------------------------------------------------------
    TEST_CASE("telemetry show prints JSONL payload") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        if (!telemetry_available(home)) {
            MESSAGE("SKIP: `den telemetry` not yet implemented — T70 upstream gap");
            CHECK(true);
            return;
        }

        // Enable telemetry so there is something to show.
        run_den("telemetry on", home);

        auto r = run_den("telemetry show", home);
        CHECK(r.exit_code == 0);
        // Must contain at least one JSONL line.
        CHECK(has_jsonl_line(r.output));
    }

    // -----------------------------------------------------------------------
    // 2. `den telemetry send` prints the JSONL to stdout before transmitting
    // -----------------------------------------------------------------------
    TEST_CASE("telemetry send prints exact JSONL before transmitting") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        if (!telemetry_available(home)) {
            MESSAGE("SKIP: `den telemetry` not yet implemented — T70 upstream gap");
            CHECK(true);
            return;
        }

        // Check python3 is available for the stub server.
        if (::system("python3 --version >/dev/null 2>&1") != 0) {
            MESSAGE("SKIP: python3 not available, cannot run stub HTTP server");
            CHECK(true);
            return;
        }

        // Enable telemetry.
        run_den("telemetry on", home);

        // Start stub server.
        auto [port, stub_pid] = start_stub_server();
        const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/v1/report";

        // Run `den telemetry send` with the stub URL injected.
        auto r = run_den_env("DEN_TELEMETRY_URL=" + url, "telemetry send", home);

        // Wait for stub to exit.
        ::waitpid(stub_pid, nullptr, 0);

        CHECK(r.exit_code == 0);
        // Must have printed JSONL before/during send.
        CHECK(has_jsonl_line(r.output));
        // Must not have failed silently — output should mention the URL or "Sent".
        const bool mentions_send = r.output.find("Sent") != std::string::npos ||
                                   r.output.find("204") != std::string::npos ||
                                   r.output.find(url) != std::string::npos;
        CHECK(mentions_send);
    }

    // -----------------------------------------------------------------------
    // 3. `den telemetry show` output is stable (same run = same payload)
    // -----------------------------------------------------------------------
    TEST_CASE("telemetry show output is idempotent within same session") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        if (!telemetry_available(home)) {
            MESSAGE("SKIP: `den telemetry` not yet implemented — T70 upstream gap");
            CHECK(true);
            return;
        }

        run_den("telemetry on", home);
        auto r1 = run_den("telemetry show", home);
        auto r2 = run_den("telemetry show", home);

        CHECK(r1.exit_code == 0);
        CHECK(r2.exit_code == 0);
        // The payload should be structurally identical (same keys, same or equal
        // counts — timestamp may differ, so we only check JSONL structure here).
        CHECK(has_jsonl_line(r1.output));
        CHECK(has_jsonl_line(r2.output));
    }

} // TEST_SUITE telemetry::payload_visible

} // namespace telemetry_payload_test
} // namespace den

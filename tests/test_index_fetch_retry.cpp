// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// The index download retries transient network/TLS errors (issue #197).
// Previously a single bad read (LibreSSL "bad decrypt" from formula.json)
// failed `den update` outright. These tests run a local HTTP server that
// fails the first N connections in a chosen way, then serves normally.

#include "core/error.h"
#include "download/http.h"
#include "index/index.h"

#include <curl/curl.h>
#include <doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace den;

namespace {

enum class FailMode {
    Reset,    ///< read the request, then reset the connection (RST)
    Truncate, ///< promise more body than is sent, then close
    Status503,
};

/// Serves `body` (or `status` with no body) on 127.0.0.1, failing the first
/// `fail_first` connections with `mode`. One connection per request.
class FlakyServer {
  public:
    FlakyServer(int fail_first, FailMode mode, std::string body, int status = 200)
        : fail_first_(fail_first), mode_(mode), body_(std::move(body)), status_(status) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listen_fd_ >= 0);
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        REQUIRE(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
        REQUIRE(::listen(listen_fd_, 16) == 0);
        socklen_t len = sizeof addr;
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~FlakyServer() {
        stop_ = true;
        thread_.join();
        ::close(listen_fd_);
    }
    FlakyServer(const FlakyServer&) = delete;
    FlakyServer& operator=(const FlakyServer&) = delete;

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }
    int connections() const { return connections_.load(); }
    std::vector<std::string> paths() const {
        std::lock_guard<std::mutex> lock(mu_);
        return paths_;
    }

  private:
    void serve() {
        while (!stop_) {
            pollfd p{listen_fd_, POLLIN, 0};
            if (::poll(&p, 1, 20) <= 0)
                continue;
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0)
                continue;
#ifdef SO_NOSIGPIPE
            int one = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            handle(fd, connections_++);
        }
    }

    void send_all(int fd, const std::string& s) {
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        for (std::size_t off = 0; off < s.size();) {
            ssize_t n = ::send(fd, s.data() + off, s.size() - off, flags);
            if (n <= 0)
                return;
            off += static_cast<std::size_t>(n);
        }
    }

    void handle(int fd, int index) {
        std::string req;
        char buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = ::recv(fd, buf, sizeof buf, 0);
            if (n <= 0)
                break;
            req.append(buf, static_cast<std::size_t>(n));
        }
        {
            auto sp1 = req.find(' ');
            auto sp2 = req.find(' ', sp1 + 1);
            std::lock_guard<std::mutex> lock(mu_);
            paths_.push_back(sp1 == std::string::npos ? "" : req.substr(sp1 + 1, sp2 - sp1 - 1));
        }
        if (index < fail_first_) {
            switch (mode_) {
            case FailMode::Reset: {
                linger l{1, 0}; // close with RST
                ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
                break;
            }
            case FailMode::Truncate:
                send_all(
                    fd, "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body_.size() + 100) +
                            "\r\nConnection: close\r\n\r\n" + body_);
                break;
            case FailMode::Status503:
                send_all(fd, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                             "Connection: close\r\n\r\n");
                break;
            }
            ::close(fd);
            return;
        }
        const std::string payload = status_ == 200 ? body_ : std::string{};
        send_all(fd, "HTTP/1.1 " + std::to_string(status_) +
                         " X\r\nContent-Length: " + std::to_string(payload.size()) +
                         "\r\nConnection: close\r\n\r\n" + payload);
        ::close(fd);
    }

    int fail_first_;
    FailMode mode_;
    std::string body_;
    int status_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<int> connections_{0};
    mutable std::mutex mu_;
    std::vector<std::string> paths_;
    std::thread thread_;
};

/// Keeps a proxy in the environment (CI, a dev box behind squid) from
/// intercepting requests to the local server.
struct NoProxyForLoopback {
    std::optional<std::string> saved_upper, saved_lower;
    NoProxyForLoopback() {
        if (const char* v = std::getenv("NO_PROXY"))
            saved_upper = v;
        if (const char* v = std::getenv("no_proxy"))
            saved_lower = v;
        ::setenv("NO_PROXY", "127.0.0.1,localhost", 1);
        ::setenv("no_proxy", "127.0.0.1,localhost", 1);
    }
    ~NoProxyForLoopback() {
        saved_upper ? ::setenv("NO_PROXY", saved_upper->c_str(), 1) : ::unsetenv("NO_PROXY");
        saved_lower ? ::setenv("no_proxy", saved_lower->c_str(), 1) : ::unsetenv("no_proxy");
    }
};

/// No sleeping between tries; records each retry message.
struct FastPolicy {
    std::vector<std::string> retries;
    FetchRetryPolicy policy;
    explicit FastPolicy(int max_attempts = 3) {
        policy.max_attempts = max_attempts;
        policy.base_delay = std::chrono::milliseconds(0);
        policy.on_retry = [this](const std::string& m) { retries.push_back(m); };
    }
};

std::string error_of(auto&& fn) {
    try {
        fn();
    } catch (const DownloadError& e) {
        return e.what();
    }
    return {};
}

} // namespace

TEST_SUITE("download::fetch_retry") {
    TEST_CASE("transient failures are classified like install.sh's download retry") {
        for (CURLcode c :
             {CURLE_COULDNT_RESOLVE_PROXY, CURLE_COULDNT_RESOLVE_HOST, CURLE_COULDNT_CONNECT,
              CURLE_HTTP2, CURLE_PARTIAL_FILE, CURLE_OPERATION_TIMEDOUT, CURLE_SSL_CONNECT_ERROR,
              CURLE_GOT_NOTHING, CURLE_SEND_ERROR, CURLE_RECV_ERROR, CURLE_HTTP2_STREAM}) {
            CAPTURE(c);
            CHECK(is_transient_fetch_failure(c, 0));
        }
        CHECK(is_transient_fetch_failure(CURLE_OK, 500));
        CHECK(is_transient_fetch_failure(CURLE_OK, 503));
        CHECK_FALSE(is_transient_fetch_failure(CURLE_OK, 200));
        CHECK_FALSE(is_transient_fetch_failure(CURLE_OK, 404));
        CHECK_FALSE(is_transient_fetch_failure(CURLE_OK, 403));
        CHECK_FALSE(is_transient_fetch_failure(CURLE_OK, 429));
        CHECK_FALSE(is_transient_fetch_failure(CURLE_FILESIZE_EXCEEDED, 200));
        CHECK_FALSE(is_transient_fetch_failure(CURLE_URL_MALFORMAT, 0));
        CHECK_FALSE(is_transient_fetch_failure(CURLE_PEER_FAILED_VERIFICATION, 0));
    }

    TEST_CASE("the default policy is a bounded, short backoff") {
        FetchRetryPolicy p;
        CHECK(p.max_attempts == 3);
        CHECK(p.base_delay == std::chrono::milliseconds(2000));
    }

    TEST_CASE("succeeds when fewer than max attempts fail, logging each retry") {
        NoProxyForLoopback np;
        FlakyServer srv(2, FailMode::Reset, "hello");
        FastPolicy fp(3);
        CHECK(fetch_url_retrying(srv.url("/formula.json"), fp.policy) == "hello");
        CHECK(srv.connections() == 3);
        REQUIRE(fp.retries.size() == 2);
        CHECK(fp.retries[0].find("HTTP fetch failed for " + srv.url("/formula.json")) == 0);
        CHECK(fp.retries[0].find("retrying (1 of 2)") != std::string::npos);
        CHECK(fp.retries[1].find("retrying (2 of 2)") != std::string::npos);
    }

    TEST_CASE("gives up with a clear error after max attempts") {
        NoProxyForLoopback np;
        FlakyServer srv(10, FailMode::Reset, "hello");
        FastPolicy fp(3);
        const std::string err =
            error_of([&] { fetch_url_retrying(srv.url("/formula.json"), fp.policy); });
        CHECK(srv.connections() == 3);
        CHECK(fp.retries.size() == 2);
        CHECK(err.find("HTTP fetch failed for " + srv.url("/formula.json")) == 0);
        CHECK(err.find("(gave up after 3 attempts)") != std::string::npos);
    }

    TEST_CASE("a 404 is not retried") {
        NoProxyForLoopback np;
        FlakyServer srv(0, FailMode::Reset, "", 404);
        FastPolicy fp(3);
        const std::string err =
            error_of([&] { fetch_url_retrying(srv.url("/formula.json"), fp.policy); });
        CHECK(err == "HTTP 404 fetching " + srv.url("/formula.json"));
        CHECK(srv.connections() == 1);
        CHECK(fp.retries.empty());
    }

    TEST_CASE("a truncated body is retried") {
        NoProxyForLoopback np;
        FlakyServer srv(1, FailMode::Truncate, "the whole body");
        FastPolicy fp(3);
        CHECK(fetch_url_retrying(srv.url("/cask.json"), fp.policy) == "the whole body");
        CHECK(srv.connections() == 2);
        CHECK(fp.retries.size() == 1);
    }

    TEST_CASE("a 5xx is retried") {
        NoProxyForLoopback np;
        FlakyServer srv(1, FailMode::Status503, "ok");
        FastPolicy fp(3);
        CHECK(fetch_url_retrying(srv.url("/formula.json"), fp.policy) == "ok");
        CHECK(srv.connections() == 2);
        REQUIRE(fp.retries.size() == 1);
        CHECK(fp.retries[0].find("HTTP 503") == 0);
    }

    TEST_CASE("plain fetch_url still makes a single attempt") {
        NoProxyForLoopback np;
        FlakyServer srv(1, FailMode::Reset, "hello");
        CHECK_FALSE(error_of([&] { fetch_url(srv.url("/x")); }).empty());
        CHECK(srv.connections() == 1);
    }
}

TEST_SUITE("index::fetch_retry") {
    TEST_CASE("den update's index download retries a transient failure") {
        NoProxyForLoopback np;
        // The first read (formula.json) is cut short; both payloads then load.
        FlakyServer srv(1, FailMode::Truncate, "[]");
        FastPolicy fp(3);
        PackageIndex idx;
        CHECK_NOTHROW(idx = fetch_and_transform_from(srv.url("/formula.json"),
                                                     srv.url("/cask.json"), fp.policy));
        CHECK(idx.packages.empty());
        CHECK(srv.paths() ==
              std::vector<std::string>{"/formula.json", "/formula.json", "/cask.json"});
        CHECK(fp.retries.size() == 1);
    }
}

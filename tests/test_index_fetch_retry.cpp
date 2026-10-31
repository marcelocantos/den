// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// The index download retries transient network/TLS errors (issue #197).
// Previously a single bad read (LibreSSL "bad decrypt" from formula.json)
// failed `den update` outright. These tests run a local HTTP server that
// fails the first N connections in a chosen way, then serves normally.
// Self-update's downloads and the GHCR token request use the same retrying
// fetch; their tests are at the end.

#include "core/config.h"
#include "core/error.h"
#include "download/fetch.h"
#include "download/http.h"
#include "download/sha256.h"
#include "index/index.h"
#include "selfupdate/selfupdate.h"

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
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace den;

namespace {

enum class FailMode {
    Reset,     ///< read the request, then reset the connection (RST)
    Truncate,  ///< promise more body than is sent, then close
    Status503, ///< with the server's `retry_after` header, if non-empty
    Status408,
    Status429, ///< with the server's `retry_after` header, if non-empty
};

/// One path a routed FlakyServer serves: `body`, after failing its first
/// `fail_first` requests.
struct Route {
    std::string body;
    int fail_first = 0;
};

/// Serves `body` (or `status` with no body) on 127.0.0.1, failing the first
/// `fail_first` connections with `mode`. One connection per request.
/// Alternatively, serves `routes` keyed by exact path (query string ignored),
/// each failing its own first requests; any other path gets a 404.
class FlakyServer {
  public:
    FlakyServer(std::map<std::string, Route> routes, FailMode mode, std::string retry_after = {})
        : FlakyServer(0, mode, {}, 200, std::move(retry_after)) {
        std::lock_guard<std::mutex> lock(mu_);
        routes_ = std::move(routes);
    }
    FlakyServer(int fail_first, FailMode mode, std::string body, int status = 200,
                std::string retry_after = {})
        : fail_first_(fail_first), mode_(mode), body_(std::move(body)), status_(status),
          retry_after_(std::move(retry_after)) {
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
    /// Each request's line and headers, in arrival order.
    std::vector<std::string> requests() const {
        std::lock_guard<std::mutex> lock(mu_);
        return requests_;
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
        bool fail = index < fail_first_;
        std::string body = body_;
        int status = status_;
        {
            auto sp1 = req.find(' ');
            auto sp2 = req.find(' ', sp1 + 1);
            std::string path = sp1 == std::string::npos ? "" : req.substr(sp1 + 1, sp2 - sp1 - 1);
            std::lock_guard<std::mutex> lock(mu_);
            paths_.push_back(path);
            requests_.push_back(req);
            if (!routes_.empty()) {
                path = path.substr(0, path.find('?'));
                auto it = routes_.find(path);
                if (it == routes_.end()) {
                    fail = false;
                    status = 404;
                } else {
                    fail = route_hits_[path]++ < it->second.fail_first;
                    body = it->second.body;
                    status = 200;
                }
            }
        }
        if (fail) {
            switch (mode_) {
            case FailMode::Reset: {
                linger l{1, 0}; // close with RST
                ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
                break;
            }
            case FailMode::Truncate:
                send_all(fd,
                         "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size() + 100) +
                             "\r\nConnection: close\r\n\r\n" + body);
                break;
            case FailMode::Status503:
                send_all(fd, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n" +
                                 (retry_after_.empty() ? std::string{}
                                                       : "Retry-After: " + retry_after_ + "\r\n") +
                                 "Connection: close\r\n\r\n");
                break;
            case FailMode::Status408:
                send_all(fd, "HTTP/1.1 408 Request Timeout\r\nContent-Length: 0\r\n"
                             "Connection: close\r\n\r\n");
                break;
            case FailMode::Status429:
                send_all(fd, "HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\n" +
                                 (retry_after_.empty() ? std::string{}
                                                       : "Retry-After: " + retry_after_ + "\r\n") +
                                 "Connection: close\r\n\r\n");
                break;
            }
            ::close(fd);
            return;
        }
        const std::string payload = status == 200 ? body : std::string{};
        send_all(fd, "HTTP/1.1 " + std::to_string(status) +
                         " X\r\nContent-Length: " + std::to_string(payload.size()) +
                         "\r\nConnection: close\r\n\r\n" + payload);
        ::close(fd);
    }

    int fail_first_;
    FailMode mode_;
    std::string body_;
    int status_;
    std::string retry_after_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<int> connections_{0};
    mutable std::mutex mu_;
    std::vector<std::string> paths_;
    std::vector<std::string> requests_;
    std::map<std::string, Route> routes_;
    std::map<std::string, int> route_hits_;
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

using std::chrono::milliseconds;

/// Never sleeps: records each retry message and each computed delay instead.
/// Zero backoff by default; pass real values to check the delay arithmetic.
struct FastPolicy {
    std::vector<std::string> retries;
    std::vector<milliseconds> delays;
    FetchRetryPolicy policy;
    explicit FastPolicy(int max_attempts = 3, milliseconds base = milliseconds(0),
                        milliseconds max = milliseconds(4000)) {
        policy.max_attempts = max_attempts;
        policy.base_delay = base;
        policy.max_delay = max;
        policy.on_retry = [this](const std::string& m) { retries.push_back(m); };
        policy.sleep = [this](milliseconds d) { delays.push_back(d); };
    }
};

/// An RFC 7231 HTTP-date `secs` seconds from now.
std::string http_date_in(long secs) {
    std::time_t t = std::time(nullptr) + secs;
    std::tm tm{};
    ::gmtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return buf;
}

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
        for (long s : {408L, 429L, 500L, 503L, 599L}) {
            CAPTURE(s);
            CHECK(is_transient_fetch_failure(CURLE_OK, s));
        }
        for (long s :
             {200L, 304L, 400L, 401L, 403L, 404L, 407L, 409L, 410L, 428L, 430L, 451L, 600L}) {
            CAPTURE(s);
            CHECK_FALSE(is_transient_fetch_failure(CURLE_OK, s));
        }
        CHECK_FALSE(is_transient_fetch_failure(CURLE_FILESIZE_EXCEEDED, 200));
        CHECK_FALSE(is_transient_fetch_failure(CURLE_URL_MALFORMAT, 0));
        CHECK_FALSE(is_transient_fetch_failure(CURLE_PEER_FAILED_VERIFICATION, 0));
    }

    TEST_CASE("the default policy is a bounded, short backoff") {
        FetchRetryPolicy p;
        CHECK(p.max_attempts == 3);
        CHECK(p.base_delay == std::chrono::milliseconds(2000));
        CHECK(p.max_delay == std::chrono::milliseconds(4000));
        CHECK_FALSE(p.sleep);
    }

    TEST_CASE("the backoff is linear and capped at max_delay") {
        NoProxyForLoopback np;
        FlakyServer srv(3, FailMode::Reset, "hello");
        FastPolicy fp(4, milliseconds(2000), milliseconds(3000));
        CHECK(fetch_url_retrying(srv.url("/formula.json"), fp.policy) == "hello");
        CHECK(fp.delays == std::vector<milliseconds>{milliseconds(2000), milliseconds(3000),
                                                     milliseconds(3000)});
        REQUIRE(fp.retries.size() == 3);
        CHECK(fp.retries[0].find("in 2000 ms") != std::string::npos);
        CHECK(fp.retries[1].find("in 3000 ms") != std::string::npos);
    }

    TEST_CASE("a 408 is retried") {
        NoProxyForLoopback np;
        FlakyServer srv(1, FailMode::Status408, "ok");
        FastPolicy fp(3);
        CHECK(fetch_url_retrying(srv.url("/formula.json"), fp.policy) == "ok");
        CHECK(srv.connections() == 2);
        REQUIRE(fp.retries.size() == 1);
        CHECK(fp.retries[0].find("HTTP 408") == 0);
    }

    TEST_CASE("a 429's Retry-After is honoured, capped at max_delay") {
        NoProxyForLoopback np;
        SUBCASE("seconds form, over the cap") {
            FlakyServer srv(2, FailMode::Status429, "ok", 200, "120");
            FastPolicy fp(3, milliseconds(2000), milliseconds(4000));
            CHECK(fetch_url_retrying(srv.url("/formula.json"), fp.policy) == "ok");
            CHECK(srv.connections() == 3);
            CHECK(fp.delays == std::vector<milliseconds>{milliseconds(4000), milliseconds(4000)});
            REQUIRE(fp.retries.size() == 2);
            CHECK(fp.retries[0].find("HTTP 429") == 0);
            CHECK(fp.retries[0].find("in 4000 ms (Retry-After: 120 s, capped)") !=
                  std::string::npos);
        }
        SUBCASE("seconds form, under the cap, replaces the backoff") {
            FlakyServer srv(1, FailMode::Status429, "ok", 200, "1");
            FastPolicy fp(3, milliseconds(2000), milliseconds(4000));
            CHECK(fetch_url_retrying(srv.url("/formula.json"), fp.policy) == "ok");
            CHECK(fp.delays == std::vector<milliseconds>{milliseconds(1000)});
            REQUIRE(fp.retries.size() == 1);
            CHECK(fp.retries[0].find("in 1000 ms (Retry-After: 1 s)") != std::string::npos);
        }
        SUBCASE("HTTP-date form, over the cap") {
            FlakyServer srv(1, FailMode::Status429, "ok", 200, http_date_in(3600));
            FastPolicy fp(3, milliseconds(2000), milliseconds(4000));
            CHECK(fetch_url_retrying(srv.url("/formula.json"), fp.policy) == "ok");
            CHECK(fp.delays == std::vector<milliseconds>{milliseconds(4000)});
            REQUIRE(fp.retries.size() == 1);
            CHECK(fp.retries[0].find(", capped)") != std::string::npos);
        }
    }

    TEST_CASE("a 429 without Retry-After uses the normal backoff") {
        NoProxyForLoopback np;
        FlakyServer srv(2, FailMode::Status429, "ok");
        FastPolicy fp(3, milliseconds(2000), milliseconds(4000));
        CHECK(fetch_url_retrying(srv.url("/formula.json"), fp.policy) == "ok");
        CHECK(srv.connections() == 3);
        CHECK(fp.delays == std::vector<milliseconds>{milliseconds(2000), milliseconds(4000)});
        REQUIRE(fp.retries.size() == 2);
        CHECK(fp.retries[0].find("Retry-After") == std::string::npos);
    }

    TEST_CASE("a 429 that persists gives up after max attempts") {
        NoProxyForLoopback np;
        FlakyServer srv(10, FailMode::Status429, "ok", 200, "120");
        FastPolicy fp(3, milliseconds(2000), milliseconds(4000));
        const std::string err =
            error_of([&] { fetch_url_retrying(srv.url("/formula.json"), fp.policy); });
        CHECK(srv.connections() == 3);
        CHECK(err ==
              "HTTP 429 fetching " + srv.url("/formula.json") + " (gave up after 3 attempts)");
    }

    TEST_CASE("a 503's Retry-After is honoured, capped at max_delay") {
        NoProxyForLoopback np;
        SUBCASE("under the cap, replaces the backoff") {
            FlakyServer srv(1, FailMode::Status503, "ok", 200, "1");
            FastPolicy fp(3, milliseconds(2000), milliseconds(4000));
            CHECK(fetch_url_retrying(srv.url("/formula.json"), fp.policy) == "ok");
            CHECK(srv.connections() == 2);
            CHECK(fp.delays == std::vector<milliseconds>{milliseconds(1000)});
            REQUIRE(fp.retries.size() == 1);
            CHECK(fp.retries[0].find("HTTP 503") == 0);
            CHECK(fp.retries[0].find("in 1000 ms (Retry-After: 1 s)") != std::string::npos);
        }
        SUBCASE("over the cap") {
            FlakyServer srv(2, FailMode::Status503, "ok", 200, "120");
            FastPolicy fp(3, milliseconds(2000), milliseconds(4000));
            CHECK(fetch_url_retrying(srv.url("/formula.json"), fp.policy) == "ok");
            CHECK(srv.connections() == 3);
            CHECK(fp.delays == std::vector<milliseconds>{milliseconds(4000), milliseconds(4000)});
            REQUIRE(fp.retries.size() == 2);
            CHECK(fp.retries[0].find("in 4000 ms (Retry-After: 120 s, capped)") !=
                  std::string::npos);
        }
        SUBCASE("HTTP-date form, over the cap") {
            FlakyServer srv(1, FailMode::Status503, "ok", 200, http_date_in(3600));
            FastPolicy fp(3, milliseconds(2000), milliseconds(4000));
            CHECK(fetch_url_retrying(srv.url("/formula.json"), fp.policy) == "ok");
            CHECK(fp.delays == std::vector<milliseconds>{milliseconds(4000)});
            REQUIRE(fp.retries.size() == 1);
            CHECK(fp.retries[0].find(", capped)") != std::string::npos);
        }
    }

    TEST_CASE("a 503 without Retry-After uses the normal backoff") {
        NoProxyForLoopback np;
        FlakyServer srv(2, FailMode::Status503, "ok");
        FastPolicy fp(3, milliseconds(2000), milliseconds(4000));
        CHECK(fetch_url_retrying(srv.url("/formula.json"), fp.policy) == "ok");
        CHECK(srv.connections() == 3);
        CHECK(fp.delays == std::vector<milliseconds>{milliseconds(2000), milliseconds(4000)});
        REQUIRE(fp.retries.size() == 2);
        CHECK(fp.retries[0].find("Retry-After") == std::string::npos);
    }

    TEST_CASE("other 4xx statuses are not retried") {
        NoProxyForLoopback np;
        for (int s : {400, 403, 410}) {
            CAPTURE(s);
            FlakyServer srv(0, FailMode::Reset, "", s);
            FastPolicy fp(3);
            const std::string err =
                error_of([&] { fetch_url_retrying(srv.url("/formula.json"), fp.policy); });
            CHECK(err == "HTTP " + std::to_string(s) + " fetching " + srv.url("/formula.json"));
            CHECK(srv.connections() == 1);
            CHECK(fp.retries.empty());
        }
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

namespace {

/// A GitHub release JSON for `tag` carrying this platform's tarball and
/// checksum, at URLs on `srv`.
std::string release_json(const FlakyServer& srv, const std::string& tag) {
    const std::string tarball =
        "den-" + asset_version_for_tag(tag) + "-" + platform_suffix(Config{}) + ".tar.gz";
    return R"({"tag_name":")" + tag + R"(","assets":[{"name":")" + tarball +
           R"(","browser_download_url":")" + srv.url("/den.tar.gz") + R"("},{"name":")" + tarball +
           R"(.sha256","browser_download_url":")" + srv.url("/den.sha256") + R"("}]})";
}

ReleaseSource source_for(const FlakyServer& srv, const FastPolicy& fp) {
    ReleaseSource src;
    src.api_base = srv.url("/repos/marcelocantos/den");
    src.retry = fp.policy;
    return src;
}

} // namespace

TEST_SUITE("selfupdate::fetch_retry") {
    // `assets` only supplies the URLs that release_json names; nothing
    // fetches them here.
    TEST_CASE("check_for_update retries a 503, honouring Retry-After") {
        NoProxyForLoopback np;
        FlakyServer assets(0, FailMode::Reset, "");
        FlakyServer srv({{"/repos/marcelocantos/den/releases/latest",
                          Route{release_json(assets, "v999.0.0"), 1}}},
                        FailMode::Status503, "1");
        FastPolicy fp(3, milliseconds(2000), milliseconds(4000));
        auto info = check_for_update(Config{}, source_for(srv, fp));
        REQUIRE(info);
        CHECK(info->latest_version == "999.0.0");
        CHECK(info->download_url == assets.url("/den.tar.gz"));
        CHECK(info->checksum_url == assets.url("/den.sha256"));
        CHECK(srv.connections() == 2);
        CHECK(fp.delays == std::vector<milliseconds>{milliseconds(1000)});
        REQUIRE(fp.retries.size() == 1);
        CHECK(fp.retries[0].find("HTTP 503") == 0);
    }

    TEST_CASE("resolve_latest_version retries a reset connection") {
        NoProxyForLoopback np;
        FlakyServer srv(
            {{"/repos/marcelocantos/den/releases/latest", Route{R"({"tag_name":"v999.0.0"})", 2}}},
            FailMode::Reset);
        FastPolicy fp(3);
        CHECK(resolve_latest_version(source_for(srv, fp)) == "999.0.0");
        CHECK(srv.connections() == 3);
        CHECK(fp.retries.size() == 2);
    }

    TEST_CASE("fetch_release_for_version retries, and keeps its error when it gives up") {
        NoProxyForLoopback np;
        FlakyServer assets(0, FailMode::Reset, "");
        const std::string path = "/repos/marcelocantos/den/releases/tags/v999.0.0-rc.1";
        SUBCASE("a 503 then the release") {
            FlakyServer srv({{path, Route{release_json(assets, "v999.0.0-rc.1"), 2}}},
                            FailMode::Status503);
            FastPolicy fp(3);
            auto info = fetch_release_for_version(Config{}, "999.0.0-rc.1", source_for(srv, fp));
            CHECK(info.tag == "v999.0.0-rc.1");
            CHECK(srv.connections() == 3);
            CHECK(fp.retries.size() == 2);
        }
        SUBCASE("a 503 every time") {
            FlakyServer srv({{path, Route{"", 10}}}, FailMode::Status503);
            FastPolicy fp(3);
            std::string err;
            try {
                fetch_release_for_version(Config{}, "999.0.0-rc.1", source_for(srv, fp));
            } catch (const UserError& e) {
                err = e.what();
            }
            CHECK(srv.connections() == 3);
            CHECK(err == "could not fetch release v999.0.0-rc.1: HTTP 503 fetching " +
                             srv.url(path) + " (gave up after 3 attempts)");
        }
        SUBCASE("a 404 is not retried") {
            FlakyServer srv({{"/elsewhere", Route{}}}, FailMode::Status503); // 404 for `path`
            FastPolicy fp(3);
            CHECK_THROWS_AS(
                fetch_release_for_version(Config{}, "999.0.0-rc.1", source_for(srv, fp)),
                UserError);
            CHECK(srv.connections() == 1);
            CHECK(fp.retries.empty());
        }
    }

    TEST_CASE("apply_update retries the checksum and tarball downloads") {
        NoProxyForLoopback np;
        // The checksum never matches the tarball, so apply_update stops at
        // verification and never touches the running binary.
        const std::string tarball = "not a real den release";
        const std::string wrong = std::string(64, '0');
        REQUIRE(hash_string(tarball) != wrong);
        FlakyServer srv({{"/den.sha256", Route{wrong + "  den.tar.gz\n", 1}},
                         {"/den.tar.gz", Route{tarball, 2}}},
                        FailMode::Status503, "1");
        FastPolicy fp(3, milliseconds(2000), milliseconds(4000));
        UpdateInfo info;
        info.latest_version = "999.0.0";
        info.tag = "v999.0.0";
        info.checksum_url = srv.url("/den.sha256");
        info.download_url = srv.url("/den.tar.gz");
        std::string err;
        try {
            apply_update(info, Config{}, false, fp.policy);
        } catch (const UserError& e) {
            err = e.what();
        }
        CHECK(err == "SHA256 mismatch: expected " + wrong + ", got " + hash_string(tarball));
        CHECK(srv.paths() == std::vector<std::string>{"/den.sha256", "/den.sha256", "/den.tar.gz",
                                                      "/den.tar.gz", "/den.tar.gz"});
        CHECK(fp.delays == std::vector<milliseconds>{milliseconds(1000), milliseconds(1000),
                                                     milliseconds(1000)});
    }

    TEST_CASE("apply_update's checksum check still fails closed after retrying") {
        NoProxyForLoopback np;
        FlakyServer srv({{"/den.sha256", Route{"not-a-hash\n", 1}}}, FailMode::Reset);
        FastPolicy fp(3);
        UpdateInfo info;
        info.latest_version = "999.0.0";
        info.checksum_url = srv.url("/den.sha256");
        info.download_url = srv.url("/den.tar.gz");
        CHECK_THROWS_WITH_AS(
            apply_update(info, Config{}, false, fp.policy),
            "self-update refused: checksum asset is missing or not a 64-char hex SHA256",
            UserError);
        CHECK(srv.paths() == std::vector<std::string>{"/den.sha256", "/den.sha256"});
    }
}

TEST_SUITE("ghcr::fetch_retry") {
    TEST_CASE("the GHCR token request retries, and the blob still gets the token") {
        NoProxyForLoopback np;
        const std::string blob = "bottle bytes";
        const std::string sha = hash_string(blob);
        const std::string repo = "homebrew/core/example";
        FlakyServer srv({{"/token", Route{R"({"token":"tok123"})", 2}},
                         {"/v2/" + repo + "/blobs/sha256:" + sha, Route{blob, 0}}},
                        FailMode::Status503);
        FastPolicy fp(3, milliseconds(2000), milliseconds(4000));
        const auto cache = std::filesystem::temp_directory_path() /
                           ("den-test-ghcr-retry-" + std::to_string(::getpid()));
        std::filesystem::remove_all(cache);
        const auto path = fetch_ghcr_blob(repo, sha, cache, fp.policy, srv.url(""));
        std::ifstream in(path, std::ios::binary);
        std::stringstream got;
        got << in.rdbuf();
        CHECK(got.str() == blob);
        CHECK(fp.delays == std::vector<milliseconds>{milliseconds(2000), milliseconds(4000)});
        const auto reqs = srv.requests();
        REQUIRE(reqs.size() == 4);
        for (int i = 0; i < 3; ++i) {
            CAPTURE(i);
            CHECK(reqs[i].find("GET /token?scope=repository:" + repo + ":pull&service=ghcr.io ") ==
                  0);
            CHECK(reqs[i].find("Authorization:") == std::string::npos);
        }
        CHECK(reqs[3].find("Authorization: Bearer tok123\r\n") != std::string::npos);
        std::filesystem::remove_all(cache);
    }
}

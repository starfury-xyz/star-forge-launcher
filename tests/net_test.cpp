// Tests sf::crypto::Sha256 and sf::launch::net (downloads). Downloads go to a small HTTP server
// this test runs on 127.0.0.1, whose routes each act out one way a download can go: fine, missing,
// server trouble (once or for good), too big, cut short, redirected, cancelled. Groups marked
// "Rust:" port net.rs's tests.

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define close_socket close
#define INVALID_SOCKET (-1)
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // Windows: no SIGPIPE to begin with
#endif

#include "crypto/sha256.hpp"
#include "launch/net.hpp"

#include "check.hpp"

namespace fs = std::filesystem;
namespace net = sf::launch::net;
using net::FetchError;
using net::Kind;
using sf::crypto::Sha256;
using sf::crypto::sha256_hex;
using test::check;
using test::contains;

namespace {

void send_all(socket_t s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), MSG_NOSIGNAL);  // a client that hung up is no crash
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

std::string response(const std::string& status, const std::string& body, const std::string& headers = "",
                     bool length = true) {
    std::string r = "HTTP/1.1 " + status + "\r\nConnection: close\r\n" + headers;
    if (length) r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    return r + "\r\n" + body;
}

std::string pattern(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>(i * 7 + i / 251);
    return s;
}

// One request per connection, then close. Counts the requests per path.
class Server {
public:
    Server() {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        listen(listener_, 16);
        socklen_t len = sizeof addr;
        getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~Server() {
        stopping_ = true;
        // Wake accept() with one last connection.
        socket_t s = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port_);
        connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        close_socket(s);
        thread_.join();
        close_socket(listener_);
    }

    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
    int hits(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        return hits_[path];
    }
    std::string user_agent() {
        std::lock_guard<std::mutex> lock(mutex_);
        return user_agent_;
    }

    static const std::string& ok_body() {
        static const std::string body = "hello, star forge\n";
        return body;
    }
    static const std::string& stream_body() {
        static const std::string body = pattern(300000);
        return body;
    }

private:
    void serve() {
        for (;;) {
            socket_t c = accept(listener_, nullptr, nullptr);
            if (c == INVALID_SOCKET || stopping_) {
                if (c != INVALID_SOCKET) close_socket(c);
                return;
            }
            handle(c);
            close_socket(c);
        }
    }

    void handle(socket_t c) {
        std::string req;
        char buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos) {
            int n = recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            req.append(buf, static_cast<size_t>(n));
            if (req.size() >= 4 && req.rfind("GET ", 0) != 0) break;  // not HTTP: answer at once, as servers do
        }
        std::string path = "?";  // not HTTP (say, a TLS hello)
        if (req.rfind("GET ", 0) == 0) path = req.substr(4, req.find(' ', 4) - 4);
        int hit;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hit = ++hits_[path];
            size_t ua = req.find("User-Agent: ");
            if (ua != std::string::npos) user_agent_ = req.substr(ua + 12, req.find("\r\n", ua) - ua - 12);
        }
        if (path == "/ok.txt") send_all(c, response("200 OK", ok_body()));
        else if (path == "/stream.bin") {
            // No Content-Length: the body ends when the connection does. Sent in pieces.
            send_all(c, response("200 OK", "", "", false));
            const std::string& body = stream_body();
            for (size_t at = 0; at < body.size(); at += 50000) send_all(c, body.substr(at, 50000));
        } else if (path == "/empty") send_all(c, response("200 OK", ""));
        else if (path == "/missing.lua") send_all(c, response("404 Not Found", "nope"));
        else if (path == "/gone") send_all(c, response("410 Gone", ""));
        else if (path == "/forbidden") send_all(c, response("403 Forbidden", ""));
        else if (path == "/broken") send_all(c, response("500 Internal Server Error", ""));
        else if (path == "/busy") send_all(c, response("429 Too Many Requests", ""));
        else if (path == "/flaky") send_all(c, hit == 1 ? response("503 Service Unavailable", "") : response("200 OK", ok_body()));
        else if (path == "/big") send_all(c, response("200 OK", std::string(100, 'x')));
        else if (path == "/big-stream") send_all(c, response("200 OK", std::string(100, 'x'), "", false));
        else if (path == "/short") send_all(c, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 100\r\n\r\nhello");
        else if (path == "/redirect") send_all(c, response("302 Found", "", "Location: /ok.txt\r\n"));
        else if (path == "?") send_all(c, response("400 Bad Request", "plain HTTP here"));
        else send_all(c, response("404 Not Found", ""));
    }

    socket_t listener_;
    uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::map<std::string, int> hits_;
    std::string user_agent_;
};

// The error fetch() threw, or nullopt if it worked.
std::optional<FetchError> fetch_error(const std::string& url, const fs::path& dest, uint64_t limit = 1 << 20,
                                      Sha256* hasher = nullptr, const net::OnBytes& on_bytes = nullptr) {
    try {
        net::fetch(url, dest, limit, hasher, on_bytes);
        return std::nullopt;
    } catch (const FetchError& e) {
        return e;
    } catch (const std::exception& e) {
        check(false, std::string("fetch threw something other than FetchError: ") + e.what());
        return FetchError(Kind::Stopped, url, "(not a FetchError)");
    }
}

void sha256() {
    check(sha256_hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 of nothing");
    check(sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 of abc");
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";  // 56 bytes: padding takes a second block
    check(sha256_hex(two.data(), two.size()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "sha256 of 56 bytes (two blocks)");
    Sha256 million;
    const std::string as(1000, 'a');
    for (int i = 0; i < 1000; ++i) million.update(as.data(), as.size());
    check(million.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "sha256 of a million a's");
    // Fed in uneven pieces, it comes out the same as all at once.
    const std::string data = pattern(1000);
    Sha256 pieces;
    for (size_t at = 0, step = 1; at < data.size(); at += step, step = step * 3 % 97 + 1)
        pieces.update(data.data() + at, std::min(step, data.size() - at));
    check(pieces.hex() == sha256_hex(data.data(), data.size()), "sha256 in uneven pieces matches");
}

// Rust: names_hosts_and_files, stopping_is_not_retried.
void names() {
    check(net::host("https://a.example/x/y.lua?v=1") == "a.example", "host");
    check(net::file_name("https://a.example/x/y.lua?v=1") == "y.lua", "file name");
    check(net::file_name("https://a.example/x/") == "x", "file name of a folder URL");
    check(net::host("https://a.example:8443") == "a.example:8443", "host with a port, no path");
    check(net::file_name("https://a.example") == "a.example", "file name with no path");

    FetchError stopped(Kind::Stopped, "https://a/b", "download cancelled");
    check(std::string(stopped.what()) == "download cancelled", "a stop's message is the caller's");
    check(!stopped.passing(), "a stop isn't retried");
    check(FetchError(Kind::Status, "https://a/b", "503", 503).passing(), "503 is retried");
    check(!FetchError(Kind::NotFound, "https://a/b", "404").passing(), "404 isn't retried");
    check(FetchError(Kind::Status, "https://a/b", "429", 429).passing(), "429 is retried");
    check(!FetchError(Kind::Status, "https://a/b", "403", 403).passing(), "403 isn't retried");

    const std::string u = "https://cdn.example/packs/x.zip";
    check(std::string(FetchError(Kind::Offline, u, "").what()) == "Can't reach cdn.example. Check your internet connection.", "offline message");
    check(std::string(FetchError(Kind::Timeout, u, "").what()) == "cdn.example stopped responding.", "timeout message");
    check(std::string(FetchError(Kind::NotFound, u, "").what()) == "cdn.example doesn't have x.zip.", "not found message");
    check(std::string(FetchError(Kind::Status, u, "", 502).what()) == "cdn.example had a problem (error 502). Try again later.", "5xx message");
    check(std::string(FetchError(Kind::Status, u, "", 403).what()) == "cdn.example refused the download (error 403).", "4xx message");
    check(std::string(FetchError(Kind::Tls, u, "").what()) == "Couldn't make a secure connection to cdn.example.", "tls message");
    check(std::string(FetchError(Kind::TooLarge, u, "").what()) == "x.zip from cdn.example is larger than allowed.", "too large message");
    check(std::string(net::corrupt(u, "sha256 mismatch").what()) == "x.zip from cdn.example arrived damaged.", "corrupt message");
    check(std::string(FetchError(Kind::Disk, u, "disk full").what()) == "Couldn't save x.zip: disk full", "disk message");
}

void downloads(Server& srv, const fs::path& dir) {
    const fs::path dest = dir / "out";

    // Fine: the file, its hash, the progress callback, the user agent.
    test::write_file(dest, "an older, longer file that gets replaced");
    Sha256 hasher;
    hasher.update("junk", 4);  // reset first
    std::vector<std::pair<uint64_t, std::optional<uint64_t>>> progress;
    auto e = fetch_error(srv.url("/ok.txt"), dest, 1 << 20, &hasher, [&](uint64_t got, std::optional<uint64_t> announced) {
        progress.emplace_back(got, announced);
        return std::optional<std::string>();
    });
    check(!e, "ok.txt downloads" + (e ? std::string(": ") + e->what() : ""));
    check(test::read_file(dest) == Server::ok_body(), "ok.txt replaces what was there");
    check(hasher.hex() == sha256_hex(Server::ok_body().data(), Server::ok_body().size()), "ok.txt hashed as it arrived");
    check(!progress.empty() && progress.back().first == Server::ok_body().size() &&
              progress.back().second == std::optional<uint64_t>(Server::ok_body().size()),
          "progress ends at the full, announced size");
    check(srv.user_agent() == std::string("star-forge-launcher/") + SF_VERSION, "user agent is star-forge-launcher/" SF_VERSION);

    // No Content-Length: read to the end of the connection, in several pieces.
    progress.clear();
    e = fetch_error(srv.url("/stream.bin"), dest, 1 << 20, &hasher, [&](uint64_t got, std::optional<uint64_t> announced) {
        progress.emplace_back(got, announced);
        return std::optional<std::string>();
    });
    check(!e, "a stream with no length downloads");
    check(test::read_file(dest) == Server::stream_body(), "the stream arrives whole");
    check(hasher.hex() == sha256_hex(Server::stream_body().data(), Server::stream_body().size()), "the stream's hash");
    check(progress.size() > 1 && !progress.back().second, "progress in several steps, with no announced size");

    e = fetch_error(srv.url("/empty"), dest);
    check(!e && fs::exists(dest) && fs::file_size(dest) == 0, "an empty file downloads as an empty file");

    e = fetch_error(srv.url("/redirect"), dest);
    check(!e && test::read_file(dest) == Server::ok_body(), "a redirect is followed");

    e = fetch_error(srv.url("/flaky"), dest);
    check(!e && test::read_file(dest) == Server::ok_body() && srv.hits("/flaky") == 2, "a 503 once is retried, then works");

    // Failures that aren't retried.
    fs::remove(dest);
    e = fetch_error(srv.url("/missing.lua"), dest);
    check(e && e->kind == Kind::NotFound && contains(e->what(), "doesn't have missing.lua."), "404 is not found");
    check(srv.hits("/missing.lua") == 1, "404 isn't retried");
    check(!fs::exists(dest), "404 leaves no file");
    e = fetch_error(srv.url("/gone"), dest);
    check(e && e->kind == Kind::NotFound, "410 is not found");
    e = fetch_error(srv.url("/forbidden"), dest);
    check(e && e->kind == Kind::Status && e->status == 403 && srv.hits("/forbidden") == 1, "403: refused, once");

    e = fetch_error(srv.url("/big"), dest, 10);
    check(e && e->kind == Kind::TooLarge && contains(e->what(), "big from 127.0.0.1"), "an announced size over the limit is too large");
    check(!fs::exists(dest) && srv.hits("/big") == 1, "... found before saving anything, not retried");
    e = fetch_error(srv.url("/big-stream"), dest, 10);
    check(e && e->kind == Kind::TooLarge, "a stream going over the limit is too large");
    e = fetch_error(srv.url("/big"), dest, 100);
    check(!e, "exactly the limit is fine");

    int calls = 0;
    e = fetch_error(srv.url("/stream.bin"), dest, 1 << 20, nullptr, [&](uint64_t, std::optional<uint64_t>) {
        ++calls;
        return std::optional<std::string>("download cancelled");
    });
    check(e && e->kind == Kind::Stopped && std::string(e->what()) == "download cancelled", "the callback stops it");
    check(calls == 1 && srv.hits("/stream.bin") == 2, "... at once, not retried");

    e = fetch_error("https://127.0.0.1:" + srv.url("").substr(17) + "/ok.txt", dest);
    check(e && e->kind == Kind::Tls, std::string("https to a plain server is a TLS failure") + (e ? std::string(": ") + e->what() : ""));
    check(srv.hits("?") == 1, "... not retried");

    e = fetch_error(srv.url("/ok.txt"), dir / "no such folder" / "out");
    check(e && e->kind == Kind::Disk && contains(e->what(), "Couldn't save ok.txt: "), "an unwritable destination is a disk error");

    // Passing trouble: three tries in all (after 1 s, then 3 s).
    e = fetch_error(srv.url("/broken"), dest);
    check(e && e->kind == Kind::Status && e->status == 500 && srv.hits("/broken") == 3, "500: three tries, then had a problem");
    e = fetch_error(srv.url("/busy"), dest);
    check(e && e->status == 429 && srv.hits("/busy") == 3, "429: three tries");
    e = fetch_error(srv.url("/short"), dest);
    check(e && e->kind == Kind::Offline && srv.hits("/short") == 3, "cut short: three tries, then offline");
}

// Nothing listening: offline, after three tries.
void offline() {
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    socklen_t len = sizeof addr;
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    close_socket(s);  // the port is free again, and nothing listens on it
    const std::string url = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port)) + "/x";
    auto start = std::chrono::steady_clock::now();
    auto e = fetch_error(url, fs::temp_directory_path() / "sf-net-offline");
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
    check(e && e->kind == Kind::Offline, "nothing listening is offline");
    check(secs >= 4, "... after waiting 1 s and 3 s between tries");
}

}  // namespace

int main() {
    test::exit_on_crash();
    sha256();
    names();
    const fs::path dir = test::fresh_dir(fs::temp_directory_path() / "sf-net-test");
    {
        Server srv;
        downloads(srv, dir);
    }
    offline();
    std::printf("%s (%d failed)\n", test::failures ? "FAILED" : "ok", test::failures);
    return test::failures ? 1 : 0;
}

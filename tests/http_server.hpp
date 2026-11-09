#pragma once

// A tiny HTTP/1.1 server on 127.0.0.1 for tests, on its own thread: one request per connection,
// then it closes. A handler turns (path, how many times that path was asked for) into the whole
// response; response() builds the usual ones. Portable (Winsock on Windows).

#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace test {

#ifdef _WIN32
using socket_t = SOCKET;
inline void close_socket(socket_t s) { closesocket(s); }
inline constexpr int kNoSigpipe = 0;  // Windows: no SIGPIPE to begin with
#else
using socket_t = int;
inline void close_socket(socket_t s) { close(s); }
inline constexpr socket_t INVALID_SOCKET = -1;
inline constexpr int kNoSigpipe = MSG_NOSIGNAL;
#endif

inline void send_all(socket_t s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), kNoSigpipe);  // a client that hung up is no crash
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

inline std::string response(const std::string& status, const std::string& body, const std::string& headers = "",
                            bool length = true) {
    std::string r = "HTTP/1.1 " + status + "\r\nConnection: close\r\n" + headers;
    if (length) r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    return r + "\r\n" + body;
}

class Server {
public:
    // Gets the path ("?" for a request that isn't HTTP) and the hit count for it; sends what it
    // returns, then closes.
    using Handler = std::function<std::string(const std::string& path, int hit)>;

    explicit Server(Handler handler) : handler_(std::move(handler)) {
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

    std::string base() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::string url(const std::string& path) const { return base() + path; }
    uint16_t port() const { return port_; }
    int hits(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        return hits_[path];
    }
    std::string user_agent() {
        std::lock_guard<std::mutex> lock(mutex_);
        return user_agent_;
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
        std::string path = "?";
        if (req.rfind("GET ", 0) == 0) path = req.substr(4, req.find(' ', 4) - 4);
        int hit;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hit = ++hits_[path];
            size_t ua = req.find("User-Agent: ");
            if (ua != std::string::npos) user_agent_ = req.substr(ua + 12, req.find("\r\n", ua) - ua - 12);
        }
        send_all(c, handler_(path, hit));
    }

    Handler handler_;
    socket_t listener_;
    uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::map<std::string, int> hits_;
    std::string user_agent_;
};

// Serves fixed files (path -> body), 404 for anything else; files can be added while it runs.
class FileServer {
public:
    FileServer() : server_([this](const std::string& path, int) { return serve(path); }) {}
    void put(const std::string& path, const std::string& body) {
        std::lock_guard<std::mutex> lock(mutex_);
        files_[path] = body;
    }
    std::string base() const { return server_.base(); }
    Server& server() { return server_; }

private:
    std::string serve(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = files_.find(path);
        return it == files_.end() ? response("404 Not Found", "") : response("200 OK", it->second);
    }
    std::mutex mutex_;
    std::map<std::string, std::string> files_;
    Server server_;  // last: its thread may call serve() as soon as it starts
};

}  // namespace test

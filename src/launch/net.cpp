#include "launch/net.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <thread>

#ifdef _WIN32
#include <io.h>
#include <windows.h>

#include <winhttp.h>
#else
#include <curl/curl.h>
#include <mutex>
#include <unistd.h>
#endif

namespace sf::launch::net {
namespace {

constexpr int kAttempts = 3;            // the first try plus two retries
constexpr int kConnectSeconds = 15;
constexpr int kStallSeconds = 20;       // no data for this long counts as a stall (and is retried)
const std::string kUserAgent = std::string("star-forge-launcher/") + SF_VERSION;

std::string message(Kind kind, const std::string& url, const std::string& detail, int status) {
    const std::string h = host(url), f = file_name(url);
    switch (kind) {
    case Kind::Offline: return "Can't reach " + h + ". Check your internet connection.";
    case Kind::Timeout: return h + " stopped responding.";
    case Kind::NotFound: return h + " doesn't have " + f + ".";
    case Kind::Status:
        if (status >= 500) return h + " had a problem (error " + std::to_string(status) + "). Try again later.";
        return h + " refused the download (error " + std::to_string(status) + ").";
    case Kind::Tls: return "Couldn't make a secure connection to " + h + ".";
    case Kind::TooLarge: return f + " from " + h + " is larger than allowed.";
    case Kind::Corrupt: return f + " from " + h + " arrived damaged.";
    case Kind::Disk: return "Couldn't save " + f + ": " + detail;
    case Kind::Stopped: return detail;
    }
    return detail;
}

// What both backends feed: the final response's status and announced size, then the body in
// chunks, then the end. Each step throws FetchError when the download must stop.
class Sink {
public:
    Sink(const std::string& url, const fs::path& dest, uint64_t limit, crypto::Sha256* hasher, const OnBytes& on_bytes)
        : url_(url), dest_(dest), limit_(limit), hasher_(hasher), on_bytes_(on_bytes) {}
    ~Sink() {
        if (file_) std::fclose(file_);
    }

    void begin(int status, std::optional<uint64_t> announced) {
        if (status == 404 || status == 410) throw FetchError(Kind::NotFound, url_, std::to_string(status));
        if (status >= 400) throw FetchError(Kind::Status, url_, std::to_string(status), status);
        if (announced && *announced > limit_)
            throw FetchError(Kind::TooLarge, url_, std::to_string(*announced) + " bytes announced");
        announced_ = announced;
#ifdef _WIN32
        file_ = _wfopen(dest_.wstring().c_str(), L"wb");
#else
        file_ = std::fopen(dest_.c_str(), "wb");
#endif
        if (!file_) disk(errno);
        begun_ = true;
    }
    bool begun() const { return begun_; }

    void data(const void* p, size_t n) {
        total_ += n;
        if (total_ > limit_) throw FetchError(Kind::TooLarge, url_, "over " + std::to_string(limit_) + " bytes");
        if (hasher_) hasher_->update(p, n);
        if (std::fwrite(p, 1, n, file_) != n) disk(errno);
        if (on_bytes_)
            if (auto stop = on_bytes_(total_, announced_)) throw FetchError(Kind::Stopped, url_, *stop);
    }

    void end() {
        if (announced_ && *announced_ != total_)
            throw FetchError(Kind::Offline, url_,
                             "connection closed after " + std::to_string(total_) + " of " + std::to_string(*announced_) + " bytes");
        // Flushed to disk, like Rust's sync_all, before the caller trusts the file.
        if (std::fflush(file_) != 0) disk(errno);
#ifdef _WIN32
        FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file_))));
#else
        fsync(fileno(file_));
#endif
        int rc = std::fclose(file_);
        file_ = nullptr;
        if (rc != 0) disk(errno);
    }

    uint64_t total() const { return total_; }
    std::optional<uint64_t> announced() const { return announced_; }

private:
    [[noreturn]] void disk(int err) {
        throw FetchError(Kind::Disk, url_, ustr(dest_) + ": " + std::generic_category().message(err));
    }
    const std::string& url_;
    const fs::path& dest_;
    uint64_t limit_;
    crypto::Sha256* hasher_;
    const OnBytes& on_bytes_;
    std::optional<uint64_t> announced_;
    uint64_t total_ = 0;
    FILE* file_ = nullptr;
    bool begun_ = false;
};

#ifdef _WIN32

// --- WinHTTP ---

std::wstring wide(const std::string& s) { return upath(s).wstring(); }

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

[[noreturn]] void winhttp_fail(const std::string& url, DWORD err) {
    Kind kind = Kind::Offline;
    switch (err) {
    case ERROR_WINHTTP_TIMEOUT: kind = Kind::Timeout; break;
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED: kind = Kind::Tls; break;
    default: break;
    }
    throw FetchError(kind, url, "WinHTTP error " + std::to_string(err));
}

void fetch_once(const std::string& url, Sink& sink) {
    const bool https = url.rfind("http://", 0) != 0;
    std::wstring wurl = wide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = uc.dwSchemeLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) winhttp_fail(url, GetLastError());
    if (https && uc.nScheme != INTERNET_SCHEME_HTTPS) throw FetchError(Kind::Offline, url, "not an https URL");
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    path += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    Handle session{WinHttpOpen(wide(kUserAgent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) winhttp_fail(url, GetLastError());
    WinHttpSetTimeouts(session.h, 0, kConnectSeconds * 1000, kStallSeconds * 1000, kStallSeconds * 1000);
    Handle conn{WinHttpConnect(session.h, host.c_str(), uc.nPort, 0)};
    if (!conn.h) winhttp_fail(url, GetLastError());
    Handle req{WinHttpOpenRequest(conn.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)};
    if (!req.h) winhttp_fail(url, GetLastError());
    // Never from https to plain http (WinHTTP's default too, but say it).
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr))
        winhttp_fail(url, GetLastError());

    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &size, WINHTTP_NO_HEADER_INDEX);
    std::optional<uint64_t> announced;
    wchar_t len[32];
    size = sizeof len;
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &size,
                            WINHTTP_NO_HEADER_INDEX)) {
        wchar_t* end = nullptr;
        unsigned long long n = wcstoull(len, &end, 10);
        if (end != len) announced = n;
    }
    sink.begin(static_cast<int>(status), announced);
    char buf[64 * 1024];
    for (;;) {
        DWORD n = 0;
        if (!WinHttpReadData(req.h, buf, sizeof buf, &n)) winhttp_fail(url, GetLastError());
        if (n == 0) break;
        sink.data(buf, n);
    }
    sink.end();
}

#else

// --- libcurl ---

struct Transfer {
    Sink* sink;
    CURL* curl;
    std::exception_ptr error;  // a FetchError thrown in a callback, rethrown after the transfer
};

// Body data. The first call starts the file: by then the final response's headers are in.
size_t on_write(char* p, size_t size, size_t n, void* user) {
    auto* t = static_cast<Transfer*>(user);
    try {
        if (!t->sink->begun()) {
            long status = 0;
            curl_off_t len = -1;
            curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, &status);
            curl_easy_getinfo(t->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
            t->sink->begin(static_cast<int>(status), len >= 0 ? std::optional<uint64_t>(len) : std::nullopt);
        }
        t->sink->data(p, size * n);
        return size * n;
    } catch (...) {
        t->error = std::current_exception();
        return 0;  // makes curl stop with CURLE_WRITE_ERROR
    }
}

void fetch_once(const std::string& url, Sink& sink) {
    static std::once_flag init;
    std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    const bool https = url.rfind("http://", 0) != 0;
    CURL* curl = curl_easy_init();
    if (!curl) throw FetchError(Kind::Offline, url, "curl_easy_init failed");
    Transfer t{&sink, curl, nullptr};
    char errbuf[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, https ? "https" : "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, https ? "https" : "http,https");
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);  // 4xx/5xx: no body, CURLE_HTTP_RETURNED_ERROR
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(kConnectSeconds));
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);  // stalled: under 1 byte/s ...
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, static_cast<long>(kStallSeconds));  // ... for this long
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (t.error) std::rethrow_exception(t.error);
    const std::string detail = errbuf[0] ? errbuf : curl_easy_strerror(rc);
    switch (rc) {
    case CURLE_OK:
        if (!sink.begun()) sink.begin(static_cast<int>(status), 0);  // an empty body: no write calls
        sink.end();
        return;
    case CURLE_HTTP_RETURNED_ERROR: sink.begin(static_cast<int>(status), std::nullopt); break;  // throws
    case CURLE_OPERATION_TIMEDOUT: throw FetchError(Kind::Timeout, url, detail);
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
    case CURLE_SSL_INVALIDCERTSTATUS:
    case CURLE_USE_SSL_FAILED: throw FetchError(Kind::Tls, url, detail);
    case CURLE_PARTIAL_FILE:
        throw FetchError(Kind::Offline, url,
                         "connection closed after " + std::to_string(sink.total()) +
                             (sink.announced() ? " of " + std::to_string(*sink.announced()) : std::string()) + " bytes");
    default: break;
    }
    throw FetchError(Kind::Offline, url, detail);
}

#endif

}  // namespace

FetchError::FetchError(Kind k, const std::string& url, const std::string& detail, int s)
    : std::runtime_error(message(k, url, detail, s)), kind(k), status(s) {
    if (k != Kind::Stopped) std::fprintf(stderr, "download %s: %s\n", url.c_str(), detail.c_str());
}

bool FetchError::passing() const {
    return kind == Kind::Offline || kind == Kind::Timeout || (kind == Kind::Status && (status >= 500 || status == 429));
}

void fetch(const std::string& url, const fs::path& dest, uint64_t limit, crypto::Sha256* hasher, const OnBytes& on_bytes) {
    for (int attempt = 1;; ++attempt) {
        if (hasher) *hasher = crypto::Sha256();
        try {
            Sink sink(url, dest, limit, hasher, on_bytes);
            fetch_once(url, sink);
            return;
        } catch (const FetchError& e) {
            if (!e.passing() || attempt >= kAttempts) throw;
            std::this_thread::sleep_for(std::chrono::seconds(2 * attempt - 1));
        }
    }
}

FetchError corrupt(const std::string& url, const std::string& detail) { return FetchError(Kind::Corrupt, url, detail); }

std::string host(const std::string& url) {
    size_t s = url.find("://");
    std::string rest = s == std::string::npos ? url : url.substr(s + 3);
    return rest.substr(0, rest.find_first_of("/?#"));
}

std::string file_name(const std::string& url) {
    std::string path = url.substr(0, url.find_first_of("?#"));
    size_t end = path.size();
    while (end > 0) {
        size_t start = path.rfind('/', end - 1);
        size_t from = start == std::string::npos ? 0 : start + 1;
        if (end > from) return path.substr(from, end - from);
        if (start == std::string::npos) break;
        end = start;
    }
    return path;
}

}  // namespace sf::launch::net

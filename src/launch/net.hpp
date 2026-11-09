#pragma once

// Port of sc-launch's net.rs: downloads, with retries for passing network trouble and errors a
// player can read (the technical detail goes to stderr and the log).
//
// HTTP(S) comes from the OS: libcurl on Linux, WinHTTP on Windows (built in, nothing to ship).

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

#include "crypto/sha256.hpp"
#include "launch/paths.hpp"

namespace sf::launch::net {

// Why a download failed, as far as a player cares.
enum class Kind {
    Offline,   // no connection: offline, DNS, refused, proxy
    Timeout,   // connected, then nothing arrived for a while
    NotFound,  // the server says there's no such file (404, 410)
    Status,    // another HTTP error status (FetchError::status)
    Tls,       // the secure connection couldn't be set up (certificate, TLS)
    TooLarge,  // bigger than allowed or expected
    Corrupt,   // arrived, but doesn't match its checksum or size
    Disk,      // saving it on this computer failed
    Stopped,   // stopped by the caller (say, the player cancelled)
};

struct FetchError : std::runtime_error {
    Kind kind;
    int status = 0;  // Kind::Status: the HTTP status
    // what() is the message a player sees; `detail` is logged to stderr when it's made.
    FetchError(Kind kind, const std::string& url, const std::string& detail, int status = 0);
    // Worth trying again: offline, timeout, 5xx, 429.
    bool passing() const;
};

// Called after each chunk with the bytes received so far and the size the server announced (if it
// did). Returning a message stops the download with that error (Kind::Stopped).
using OnBytes = std::function<std::optional<std::string>(uint64_t got, std::optional<uint64_t> announced)>;

// Downloads `url` to `dest` (created or replaced), at most `limit` bytes, hashing it into
// `hasher` if given (reset first). Passing network trouble is retried twice, from the start
// (after 1 s, then 3 s). `dest` is flushed to disk before this returns. http:// only works for an
// http:// URL: an https:// download never follows a redirect to plain http. Throws FetchError.
void fetch(const std::string& url, const fs::path& dest, uint64_t limit, crypto::Sha256* hasher = nullptr,
           const OnBytes& on_bytes = nullptr);

// A damaged-download error for `url` (checksum or size mismatch).
FetchError corrupt(const std::string& url, const std::string& detail);

// The host of `url`, for messages ("https://a.example/x?y" -> "a.example").
std::string host(const std::string& url);
// The last path part of `url` ("https://a/x/y.lua?v=1" -> "y.lua", "https://a/x/" -> "x").
std::string file_name(const std::string& url);

}  // namespace sf::launch::net

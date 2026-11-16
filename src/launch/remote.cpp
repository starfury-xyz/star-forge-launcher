#include "launch/remote.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "crypto/sha256.hpp"
#include "launch/net.hpp"
#include "launch/packs.hpp"
#include "launch/profiles.hpp"
#include "pack/serde.hpp"

namespace sf::launch::remote {
namespace {

using namespace pack::serde;

constexpr uint64_t kMaxListBytes = 8ull << 20;  // largest file list we read
constexpr uint64_t kMaxFileBytes = 4ull << 30;  // largest single file we download
constexpr size_t kMaxEntries = 100000;

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n\v\f";
    size_t a = s.find_first_not_of(ws);
    return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(ws) - a + 1);
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::vector<std::string> split(const std::string& s, const char* seps) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t at = s.find_first_of(seps, start);
        out.push_back(s.substr(start, at - start));
        if (at == std::string::npos) return out;
        start = at + 1;
    }
}

// A game-relative path with / separators, or throws why it isn't one.
std::string clean_path(const std::string& raw) {
    const std::vector<std::string> parts = split(trim(raw), "/\\");
    if (!std::all_of(parts.begin(), parts.end(), plain_part))
        throw std::runtime_error(debug_str(raw) + " is not a plain path inside the game folder");
    std::string path;
    for (size_t i = 0; i < parts.size(); ++i) path += (i ? "/" : "") + parts[i];
    for (const std::string& g : pack::kGeneratedFiles)
        if (lower(g) == lower(path))
            throw std::runtime_error(path + " is written by the launcher from server.address; set that instead");
    return path;
}

std::string clean_hash(const std::string& raw, const std::string& path) {
    const std::string h = lower(trim(raw));
    if (!is_hex(h, 64)) throw std::runtime_error(path + ": not a SHA-256");
    return h;
}

// https://a.org/x/list.json -> https://a.org/x/
std::string folder_of(const std::string& url) {
    const std::string head = url.substr(0, url.find_first_of("?#"));
    size_t slash = head.rfind('/');
    return slash == std::string::npos ? url : head.substr(0, slash + 1);
}

// Appends a path to a base URL, percent-encoding each segment.
std::string join_url(const std::string& base, const std::string& path) {
    std::string b = base;
    while (!b.empty() && b.back() == '/') b.pop_back();
    std::string out;
    for (char c : b) out += c == ' ' ? std::string("%20") : std::string(1, c);
    for (const std::string& seg : split(path, "/")) {
        out += '/';
        for (unsigned char c : seg) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                c == '_' || c == '~') {
                out += static_cast<char>(c);
            } else {
                char buf[4];
                std::snprintf(buf, sizeof buf, "%%%02X", c);
                out += buf;
            }
        }
    }
    return out;
}

std::optional<uint64_t> size_of(const fs::path& p) {
    std::error_code ec;
    uint64_t n = fs::file_size(p, ec);
    if (ec) return std::nullopt;
    return n;
}

bool path_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// --- what a profile downloaded last time (remote-files.json) ---

struct Recorded {
    std::string sha256;
    uint64_t size = 0;
};

struct Record {
    std::string source;
    std::optional<std::string> release;
    std::map<std::string, Recorded> files;
};

std::optional<Record> load_record(const fs::path& profile_dir) {
    auto text = read_text(profile_dir / pack::kRemoteRecord);
    if (!text) return std::nullopt;
    try {
        json j = json::parse(*text);
        Obj o(j, "Record", {"source", "release", "files"});
        Record r;
        r.source = str(o.req("source"));
        if (const json* v = o.opt("release")) r.release = str(*v);
        const json& files = o.req("files");
        if (!files.is_object()) invalid(files, "a map");
        for (auto it = files.begin(); it != files.end(); ++it) {
            Obj e(*it, "Recorded", {"sha256", "size"});
            r.files[it.key()] = Recorded{str(e.req("sha256")), u64(e.req("size"))};
        }
        return r;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void save_record(const fs::path& profile_dir, const Record& record) {
    ojson j;
    j["source"] = record.source;
    if (record.release) j["release"] = *record.release;
    j["files"] = ojson::object();
    for (const auto& [path, f] : record.files) {
        j["files"][path]["sha256"] = f.sha256;
        j["files"][path]["size"] = f.size;
    }
    profiles::write_atomic(profile_dir / pack::kRemoteRecord, j.dump(2) + "\n");
}

fs::path target(const fs::path& profile_dir, const std::string& rel) { return profile_dir / pack::kFilesDir / upath(rel); }

std::optional<std::string> file_hash(const fs::path& p) {
    std::error_code ec;
    return signing::sha256_file(p, nullptr, ec);
}

// Throws the progress callback's message, if it gives one.
void tell(const Progress& progress, const SyncEvent& e) {
    if (auto stop = progress(e)) throw std::runtime_error(*stop);
}

// The size of a file a previous try already staged, if it's the right one.
std::optional<uint64_t> staged_size(const fs::path& staged, const Entry& e) {
    auto size = size_of(staged);
    if (!size) return std::nullopt;
    if (e.size && *e.size != *size) return std::nullopt;
    if (file_hash(staged) != e.sha256) return std::nullopt;
    return size;
}

// Downloads one entry to `staged`, checking it against the list.
uint64_t download(const Entry& e, const fs::path& staged, const Progress& progress) {
    const fs::path dir = staged.parent_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw std::runtime_error(io_error(dir, ec));
    const fs::path tmp = dir / upath("." + ustr(staged.filename()) + ".download");
    crypto::Sha256 hasher;
    try {
        net::fetch(e.url, tmp, e.size.value_or(kMaxFileBytes), &hasher, [&](uint64_t got, std::optional<uint64_t> announced) {
            SyncEvent ev{SyncEvent::Bytes};
            ev.done = got;
            ev.total = e.size ? e.size : announced;
            return progress(ev);
        });
        const uint64_t size = size_of(tmp).value_or(0);
        if (hasher.hex() != e.sha256 || (e.size && *e.size != size))
            throw net::corrupt(e.url, e.path + ": does not match the file list");
        fs::rename(tmp, staged, ec);
        if (ec) throw std::runtime_error(io_error(staged, ec));
        return size;
    } catch (const net::FetchError&) {
        fs::remove(tmp, ec);
        throw;
    }
}

}  // namespace

std::optional<uint64_t> FileList::bytes() const {
    uint64_t total = 0;
    for (const Entry& e : entries) {
        if (!e.size) return std::nullopt;
        total += *e.size;
    }
    return total;
}

std::string FileList::host() const { return net::host(source); }

std::vector<Entry> parse_json(const std::string& bytes, const std::string& list_url) {
    try {
        const json j = json::parse(bytes);
        Obj o(j, "JsonList", {"base_url", "files"}, false);
        std::optional<std::string> base;
        if (const json* b = o.opt("base_url")) base = str(*b);
        const json& files = o.req("files");
        if (!files.is_object()) invalid(files, "a map");
        // Every entry's fields are read first (serde fails on any of them before paths are checked).
        struct Raw {
            std::string path, sha256;
            std::optional<uint64_t> size;
            std::optional<std::string> url;
        };
        std::vector<Raw> raws;
        for (auto it = files.begin(); it != files.end(); ++it) {
            Obj e(*it, "JsonEntry", {"sha256", "size", "url"}, false);
            Raw r{it.key(), str(e.req("sha256")), std::nullopt, std::nullopt};
            if (const json* s = e.opt("size")) r.size = u64(*s);
            if (const json* u = e.opt("url")) r.url = str(*u);
            raws.push_back(std::move(r));
        }
        std::vector<Entry> out;
        const std::string b = base ? *base : folder_of(list_url);
        for (Raw& r : raws) {
            Entry e;
            e.path = clean_path(r.path);
            e.sha256 = clean_hash(r.sha256, e.path);
            e.size = r.size;
            e.url = r.url ? *r.url : join_url(b, e.path);
            out.push_back(std::move(e));
        }
        return out;
    } catch (const json::parse_error& e) {
        std::string m = e.what();
        size_t at = m.find("] ");
        throw std::runtime_error("file list: " + (at == std::string::npos ? m : m.substr(at + 2)));
    } catch (const pack::Error& e) {
        throw std::runtime_error(std::string("file list: ") + e.what());
    }
}

std::vector<Entry> parse_scmod1(const std::string& text) {
    // Rust's str::lines: split at \n, dropping one \r before it; no empty last line.
    std::vector<std::string> lines = split(text, "\n");
    if (!lines.empty() && lines.back().empty()) lines.pop_back();
    for (std::string& l : lines)
        if (!l.empty() && l.back() == '\r') l.pop_back();
    std::string first = lines.empty() ? "" : lines[0];
    while (first.rfind("\xEF\xBB\xBF", 0) == 0) first = first.substr(3);  // byte order marks
    if (lines.empty() || trim(first) != "SCMOD1")
        throw std::runtime_error("not an SCMOD1 list (its first line must be SCMOD1)");
    std::vector<Entry> out;
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string line = trim(lines[i]);
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> fields = split(line, "\t");
        if (fields.size() != 3)
            throw std::runtime_error("SCMOD1 line " + std::to_string(i + 1) + ": expected path, SHA-256 and URL separated by tabs");
        Entry e;
        e.path = clean_path(fields[0]);
        e.sha256 = clean_hash(fields[1], e.path);
        e.url = trim(fields[2]);
        out.push_back(std::move(e));
    }
    return out;
}

FileList fetch_list(const pack::FilesFrom& from, const signing::PublicKey* key) {
    const std::string& url = from.url();
    fs::path dir;
    try {
        packs::check_url(url);
        dir = packs::new_staging_dir();
    } catch (const std::runtime_error& e) {
        throw ListError(false, e.what());
    }
    struct Cleanup {
        fs::path dir;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    } cleanup{dir};

    const fs::path path = dir / "list";
    try {
        net::fetch(url, path, kMaxListBytes);
    } catch (const net::FetchError& e) {
        throw ListError(true, e.what());
    }
    std::error_code ec;
    auto bytes = read_text(path, &ec);
    if (!bytes) throw ListError(true, io_msg(ec));
    bool is_signed = false;
    if (key) {
        const fs::path sig_path = dir / "list.sig";
        try {
            net::fetch(url + ".sig", sig_path, 4096);
        } catch (const net::FetchError& e) {
            // A missing signature is the list's fault; a network hiccup is not.
            if (e.kind == net::Kind::NotFound || e.kind == net::Kind::TooLarge)
                throw ListError(false, std::string("the pack is signed but its file list has no signature (") + e.what() + ")");
            throw ListError(true, e.what());
        }
        const std::string sig = read_text(sig_path).value_or("");
        try {
            signing::verify_bytes(*key, *bytes, sig);
        } catch (const std::runtime_error& e) {
            throw ListError(false, url + ": " + e.what() + "; it was not signed with the pack's key");
        }
        is_signed = true;
    }
    std::vector<Entry> entries;
    try {
        entries = from.scmod1 ? parse_scmod1(*bytes) : parse_json(*bytes, url);
    } catch (const std::runtime_error& e) {
        throw ListError(false, url + ": " + e.what());
    }
    if (entries.size() > kMaxEntries) throw ListError(false, url + ": too many files");
    std::set<std::string> seen;
    for (const Entry& e : entries) {
        try {
            packs::check_url(e.url);
        } catch (const std::runtime_error& err) {
            throw ListError(false, err.what());
        }
        if (!seen.insert(lower(e.path)).second) throw ListError(false, url + ": " + e.path + " is listed twice");
    }
    FileList list{url, std::move(entries), is_signed};
    if (!from.scmod1) {
        // Already parsed once, so this can't fail; a version that isn't a short string is ignored.
        const json j = json::parse(*bytes, nullptr, false);
        if (j.is_object() && j.contains("version") && j["version"].is_string()) {
            const std::string v = j["version"].get<std::string>();
            if (!v.empty() && v.size() <= 32 && std::all_of(v.begin(), v.end(), [](char ch) { return ch > ' ' && ch < 127; }))
                list.release = v;
        }
    }
    return list;
}

std::string build_list(const fs::path& folder, const std::optional<std::string>& base_url, bool scmod1) {
    std::vector<std::string> paths;
    std::function<void(const fs::path&, const std::string&)> walk = [&](const fs::path& dir, const std::string& rel) {
        std::error_code ec;
        fs::directory_iterator it(dir, ec);
        if (ec) throw std::runtime_error(io_error(dir, ec));
        for (; it != fs::directory_iterator(); it.increment(ec)) {
            const std::string name = ustr(it->path().filename());
            if (name[0] == '.') continue;
            const std::string child = rel.empty() ? name : rel + "/" + name;
            const fs::file_status st = it->symlink_status(ec);
            if (fs::is_directory(st)) walk(it->path(), child);
            else if (fs::is_regular_file(st)) paths.push_back(child);
        }
    };
    walk(folder, "");
    std::sort(paths.begin(), paths.end());
    std::map<std::string, std::pair<std::string, uint64_t>> files;
    for (const std::string& rel : paths) {
        clean_path(rel);
        std::error_code ec;
        uint64_t size = 0;
        auto sha = signing::sha256_file(folder / upath(rel), &size, ec);
        if (!sha) throw std::runtime_error(rel + ": " + io_msg(ec));
        files[rel] = {*sha, size};
    }
    if (scmod1) {
        if (!base_url) throw std::runtime_error("an SCMOD1 list needs --base-url (where the files will be hosted)");
        std::string out = "SCMOD1\n";
        for (const auto& [rel, f] : files) out += rel + "\t" + f.first + "\t" + join_url(*base_url, rel) + "\n";
        return out;
    }
    // serde_json's json! map: keys sorted, as nlohmann::json keeps them.
    json list = {{"format", 1}, {"files", json::object()}};
    if (base_url) list["base_url"] = *base_url;
    for (const auto& [rel, f] : files) list["files"][rel] = {{"sha256", f.first}, {"size", f.second}};
    return list.dump(2) + "\n";
}

std::optional<std::string> quiet(const SyncEvent&) { return std::nullopt; }

// --- the sync lock ---

ProfileLock::ProfileLock(ProfileLock&& o) noexcept {
#ifdef _WIN32
    std::swap(handle_, o.handle_);
#else
    std::swap(fd_, o.fd_);
#endif
}

ProfileLock::~ProfileLock() {
#ifdef _WIN32
    if (handle_) CloseHandle(handle_);
#else
    if (fd_ >= 0) close(fd_);
#endif
}

ProfileLock lock_profile(const fs::path& profile_dir) {
    const fs::path path = profile_dir / kLockFile;
    const char* busy = "This profile's game files are already being updated. Wait for that to finish, then try again.";
    ProfileLock lock;
#ifdef _WIN32
    HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error(io_error(path, std::error_code(static_cast<int>(GetLastError()), std::system_category())));
    lock.handle_ = h;
    OVERLAPPED ov{};
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &ov)) {
        DWORD err = GetLastError();
        if (err == ERROR_LOCK_VIOLATION || err == ERROR_IO_PENDING) throw std::runtime_error(busy);
        throw std::runtime_error(io_error(path, std::error_code(static_cast<int>(err), std::system_category())));
    }
#else
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    if (fd < 0) throw std::runtime_error(io_error(path, std::error_code(errno, std::generic_category())));
    lock.fd_ = fd;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) throw std::runtime_error(busy);
        throw std::runtime_error(io_error(path, std::error_code(errno, std::generic_category())));
    }
#endif
    return lock;
}

// --- syncing ---

Synced sync(const fs::path& profile_dir, const FileList& list, const Progress& progress) {
    ProfileLock lock = lock_profile(profile_dir);
    const fs::path staging = profile_dir / kStagingDir;
    Record record = load_record(profile_dir).value_or(Record{});
    Synced done;

    // What needs downloading; files already right on disk are just recorded.
    std::vector<const Entry*> wanted;
    for (const Entry& e : list.entries) {
        const fs::path path = target(profile_dir, e.path);
        const auto size_now = size_of(path);
        auto r = record.files.find(e.path);
        if (r != record.files.end() && r->second.sha256 == e.sha256 && size_now == r->second.size) continue;
        if (size_now && file_hash(path) == e.sha256) {
            record.files[e.path] = Recorded{e.sha256, *size_now};
            continue;
        }
        wanted.push_back(&e);
    }
    std::set<std::string> listed;
    for (const Entry& e : list.entries) listed.insert(e.path);
    std::vector<std::string> dropped;
    for (const auto& [path, f] : record.files)
        if (!listed.count(path)) dropped.push_back(path);

    SyncEvent plan{SyncEvent::Plan};
    plan.list = &list;
    plan.files = wanted.size();
    plan.bytes = uint64_t(0);
    for (const Entry* e : wanted) plan.bytes = e->size && plan.bytes ? std::optional<uint64_t>(*plan.bytes + *e->size) : std::nullopt;
    plan.kept = list.entries.size() - wanted.size();
    plan.remove = dropped.size();
    tell(progress, plan);

    // Download everything into staging first.
    std::vector<uint64_t> sizes;
    for (const Entry* e : wanted) {
        SyncEvent file{SyncEvent::File};
        file.path = e->path;
        file.size = e->size;
        tell(progress, file);
        const fs::path staged = staging / upath(e->path);
        auto size = staged_size(staged, *e);
        if (!size) size = download(*e, staged, progress);
        sizes.push_back(*size);
        SyncEvent ok{SyncEvent::Verified};
        ok.path = e->path;
        ok.sha256 = e->sha256;
        ok.done = *size;
        tell(progress, ok);
    }

    // Everything arrived: move it in, drop what the list dropped.
    const fs::path mark = profile_dir / kSwappingMark;
    write_or_throw(mark, "");
    record.source = list.source;
    record.release = list.release;
    for (size_t i = 0; i < wanted.size(); ++i) {
        const Entry& e = *wanted[i];
        const fs::path path = target(profile_dir, e.path);
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) throw std::runtime_error(io_error(path.parent_path(), ec));
        fs::rename(staging / upath(e.path), path, ec);
        if (ec) throw std::runtime_error(io_error(path, ec));
        record.files[e.path] = Recorded{e.sha256, sizes[i]};
        ++done.downloaded;
    }
    for (const std::string& rel : dropped) {
        std::error_code ec;
        const bool removed = fs::remove(target(profile_dir, rel), ec);
        if (ec && !not_found(ec)) throw std::runtime_error(rel + ": " + io_msg(ec));
        record.files.erase(rel);
        if (removed) {
            ++done.removed;
            SyncEvent gone{SyncEvent::Removed};
            gone.path = rel;
            tell(progress, gone);
        }
    }
    save_record(profile_dir, record);
    std::error_code ec;
    fs::remove(mark, ec);
    fs::remove_all(staging, ec);
    return done;
}

std::optional<std::string> recorded_release(const fs::path& profile_dir) {
    auto r = load_record(profile_dir);
    return r ? r->release : std::nullopt;
}

bool intact(const fs::path& profile_dir) {
    if (path_exists(profile_dir / kSwappingMark)) return false;
    auto r = load_record(profile_dir);
    if (!r || r->files.empty()) return false;
    return std::all_of(r->files.begin(), r->files.end(),
                       [&](const auto& f) { return size_of(target(profile_dir, f.first)) == f.second.size; });
}

std::optional<std::string> launch_sync(const fs::path& profile_dir, const pack::Pack& pack, const signing::PublicKey* key,
                                       const Progress& progress) {
    if (!pack.files_from) return std::nullopt;
    FileList list;
    try {
        list = fetch_list(*pack.files_from, key);
    } catch (const ListError& e) {
        if (e.network && intact(profile_dir))
            return "Could not check " + pack.name + "'s game files (" + e.what() + "); using the ones from last time.";
        throw std::runtime_error(pack.name + "'s game files: " + e.what());
    }
    Synced done = sync(profile_dir, list, progress);
    if (done.downloaded + done.removed == 0) return std::nullopt;
    return pack.name + ": updated " + std::to_string(done.downloaded + done.removed) + " game file(s) from " + list.host();
}

void forget(const fs::path& profile_dir) {
    std::error_code ec;
    fs::remove(profile_dir / pack::kRemoteRecord, ec);
}

}  // namespace sf::launch::remote

#include "launch/diagnostics.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include <miniz.h>

#include "launch/profiles.hpp"
#include "launch/signing.hpp"
#include "pack/pack.hpp"

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <restartmanager.h>
#else
#include <sys/utsname.h>
#endif

namespace sf::launch::diagnostics {
namespace {

constexpr uint64_t kMaxFileBytes = 2ull << 20;   // a file's last 2 MB
constexpr uint64_t kMaxZipInput = 40ull << 20;   // what all the files may add up to
constexpr size_t kGameLogFiles = 8;              // from the newest game log folder

std::string bytes(uint64_t n) {
    if (n < 10 * 1024) return std::to_string(n) + " B";
    if (n < 10ull << 20) return std::to_string(n >> 10) + " KB";
    if (n < 10ull << 30) return std::to_string(n >> 20) + " MB";
    return std::to_string(n >> 30) + " GB";
}

std::string now_text(const char* format) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof buf, format, &tm);
    return buf;
}

std::optional<fs::path> home() {
#ifdef _WIN32
    if (auto v = get_env("USERPROFILE"); v && !v->empty()) return upath(*v);
#else
    if (auto v = get_env("HOME"); v && !v->empty()) return upath(*v);
#endif
    return std::nullopt;
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
}

struct Out {
    std::string text;
    void line(const std::string& s = "") { text += s + "\n"; }
    void kv(const char* key, const std::string& value) { line(std::string("  ") + key + ": " + value); }
};

#ifdef _WIN32

std::string os_version() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    std::string out = "Windows";
    if (fn && fn(&v) == 0)
        out += " " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + " build " +
               std::to_string(v.dwBuildNumber);
    // Wine says so: a Windows build running on Linux or macOS.
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) out += " (under Wine)";
    return out;
}

bool elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

std::string attributes(const fs::path& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return "";
    std::string out;
    if (a & FILE_ATTRIBUTE_READONLY) out += " read-only";
    if (a & FILE_ATTRIBUTE_HIDDEN) out += " hidden";
    if (a & FILE_ATTRIBUTE_SYSTEM) out += " system";
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) out += " link";
    if (a & FILE_ATTRIBUTE_ENCRYPTED) out += " encrypted";
    if (a & FILE_ATTRIBUTE_OFFLINE) out += " offline";
    if (a & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) out += " cloud-only";
    return out;
}

// The account that owns a file: "you" for the signed-in user (whose name stays out of the
// report), else the account, such as BUILTIN\Administrators for a file made by an elevated program.
std::string owner(const fs::path& p) {
    PSID sid = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(p.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &sid, nullptr, nullptr, nullptr, &sd) !=
        ERROR_SUCCESS)
        return "";
    wchar_t name[256], domain[256];
    DWORD nn = 256, dn = 256;
    SID_NAME_USE use;
    std::string out;
    if (LookupAccountSidW(nullptr, sid, name, &nn, domain, &dn, &use)) {
        wchar_t me[256];
        DWORD mn = 256;
        if (GetUserNameW(me, &mn) && _wcsicmp(me, name) == 0) out = "you";
        else out = ustr(fs::path(std::wstring(domain) + L"\\" + std::wstring(name)));
    }
    LocalFree(sd);
    return out;
}

// The programs that have a file open, from the Restart Manager (what Explorer asks when a
// file is "in use").
std::vector<std::string> holders(const fs::path& p) {
    std::vector<std::string> out;
    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1] = {};
    if (RmStartSession(&session, 0, key) != ERROR_SUCCESS) return out;
    const std::wstring w = p.wstring();
    LPCWSTR files[] = {w.c_str()};
    if (RmRegisterResources(session, 1, files, 0, nullptr, 0, nullptr) == ERROR_SUCCESS) {
        UINT needed = 0, count = 0;
        DWORD reasons = 0;
        DWORD r = RmGetList(session, &needed, &count, nullptr, &reasons);
        if (r == ERROR_MORE_DATA && needed) {
            std::vector<RM_PROCESS_INFO> info(needed);
            count = needed;
            if (RmGetList(session, &needed, &count, info.data(), &reasons) == ERROR_SUCCESS)
                for (UINT i = 0; i < count; ++i)
                    out.push_back(ustr(fs::path(std::wstring(info[i].strAppName))) + " (pid " +
                                  std::to_string(info[i].Process.dwProcessId) + ")");
        }
    }
    RmEndSession(session);
    return out;
}

#else

std::string os_version() {
    std::string out;
    if (auto text = read_text("/etc/os-release")) {
        size_t at = text->find("PRETTY_NAME=");
        if (at != std::string::npos) {
            std::string v = text->substr(at + 12, text->find('\n', at) - at - 12);
            v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
            out = v;
        }
    }
    utsname u{};
    if (uname(&u) == 0) out += std::string(out.empty() ? "" : ", ") + u.sysname + " " + u.release + " " + u.machine;
    if (get_env("APPIMAGE")) out += " (AppImage)";
    if (get_env("FLATPAK_ID")) out += " (Flatpak)";
    if (get_env("SteamDeck")) out += " (Steam Deck)";
    return out.empty() ? "unknown" : out;
}

#endif

std::string space_of(const fs::path& p) {
    std::error_code ec;
    const fs::space_info s = fs::space(p, ec);
    if (ec) return io_msg(ec);
    return bytes(s.available) + " free of " + bytes(s.capacity);
}

// Writes a scratch file and replaces it the way pack.json and local.json are saved.
std::string write_probe(const fs::path& dir) {
    const fs::path a = dir / ".sf-diagnostics.tmp", b = dir / ".sf-diagnostics-probe";
    std::string result = "OK";
    try {
        write_or_throw(a, "probe\n");
        write_or_throw(b, "old\n");
        std::error_code ec;
        rename_replacing(a, b, ec);
        if (ec) result = "replacing a file failed: " + io_msg(ec);
    } catch (const std::exception& e) {
        result = std::string("writing a file failed: ") + e.what();
    }
    std::error_code ec;
    fs::remove(a, ec);
    fs::remove(b, ec);
    return result;
}

// What a file in a profile is like: opened, read and (for JSON) parsed.
void probe_file(Out& o, const fs::path& path, const char* label, bool pack) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(path, ec))) {
        o.kv(label, "none");
        return;
    }
    if (fs::is_directory(path, ec)) {
        o.kv(label, "is a folder, not a file");
        return;
    }
    std::string head;
    auto text = read_text(path, &ec);
    if (!text) {
        head = "CANNOT READ: " + io_msg(ec);
    } else {
        head = "read " + bytes(text->size());
        if (text->size() >= 3 && std::memcmp(text->data(), "\xEF\xBB\xBF", 3) == 0) head += ", starts with a UTF-8 BOM";
        try {
            if (pack) {
                const pack::Pack p = pack::Pack::from_json(*text);
                head += ", parsed: " + p.id + " " + p.version + " \"" + p.name + "\"";
                try {
                    p.validate();
                } catch (const std::exception& e) {
                    head += std::string(", INVALID: ") + e.what();
                }
            } else {
                pack::Local::from_json(*text);
                head += ", parsed";
            }
        } catch (const std::exception& e) {
            head += std::string(", CANNOT PARSE: ") + e.what();
        }
    }
    o.kv(label, head);
#ifdef _WIN32
    std::string extra = attributes(path);
    if (!extra.empty()) o.kv("  attributes", extra.substr(1));
    if (auto who = owner(path); !who.empty()) o.kv("  owner", who);
    auto open_by = holders(path);
    if (!open_by.empty()) {
        std::string list;
        for (const auto& h : open_by) list += (list.empty() ? "" : ", ") + h;
        o.kv("  open in", list);
    }
#endif
}

// The newest of the game's log folders (named by date and time) under a LOCALAPPDATA.
std::optional<fs::path> newest_game_logs(const fs::path& local_appdata) {
    const fs::path logs = local_appdata / upath(profiles::kGameDataFolder) / "logs";
    std::optional<fs::path> best;
    std::error_code ec;
    for (fs::directory_iterator it(logs, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec) && (!best || it->path().filename() > best->filename())) best = it->path();
    return best;
}

void add_game_logs(Report& r, const std::string& prefix, const std::optional<fs::path>& local_appdata) {
    if (!local_appdata) return;
    auto dir = newest_game_logs(*local_appdata);
    if (!dir) return;
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(*dir, ec), end; !ec && it != end; it.increment(ec))
        // Not chat.log: what players wrote to each other is theirs.
        if (it->is_regular_file(ec) && it->path().filename() != "chat.log") files.push_back(it->path());
    std::sort(files.begin(), files.end());
    if (files.size() > kGameLogFiles) files.resize(kGameLogFiles);
    for (const auto& f : files) r.files.emplace_back(prefix + "game-logs/" + ustr(f.filename()), f);
}

void profile_section(Out& o, Report& r, const Input& in, const fs::path& dir) {
    const std::string name = ustr(dir.filename());
    o.line();
    o.line("[profile " + name + "]");
    o.kv("folder", ustr(dir));
    if (!profiles::valid_name(name)) o.kv("note", "the launcher skips this folder (its name starts with '.' or has a bad character)");
    std::error_code ec;
    std::vector<std::string> entries;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const std::string n = ustr(it->path().filename());
        if (it->is_directory(e2)) entries.push_back(n + "/");
        else entries.push_back(n + " (" + bytes(it->file_size(e2)) + ")");
    }
    std::sort(entries.begin(), entries.end());
    std::string listing;
    for (const auto& e : entries) listing += " " + e;
    if (ec) o.kv("files", "CANNOT LIST: " + io_msg(ec));
    else o.kv("files", listing.empty() ? "(empty)" : listing.substr(1));
    probe_file(o, dir / pack::kPackFile, "pack.json", true);
    probe_file(o, dir / pack::kLocalFile, "local.json", false);
    try {
        const pack::Profile p = profiles::load(dir);
        o.kv("loads", std::string("OK, server ") + (p.server ? *p.server : "game default") +
                          (p.local_server ? ", starts the local server" : "") +
                          (p.own_game_data ? ", own game data" : ", shared game data"));
        add_game_logs(r, "profiles/" + name + "/",
                      p.own_game_data ? std::optional<fs::path>(profiles::profile_local_appdata(dir))
                      : in.setup      ? profiles::shared_local_appdata(*in.setup)
                                      : std::nullopt);
    } catch (const std::exception& e) {
        o.kv("loads", std::string("FAILED: ") + e.what());
    }
    o.kv("save test", write_probe(dir));
    for (const char* f : {pack::kPackFile, pack::kLocalFile, "agent.log", "inject.log"})
        if (fs::is_regular_file(dir / f, ec)) r.files.emplace_back("profiles/" + name + "/" + f, dir / f);
}

}  // namespace

std::string redact(std::string text) {
    auto h = home();
    if (!h) return text;
    std::string native = ustr(*h);
    while (native.size() > 1 && (native.back() == '/' || native.back() == '\\')) native.pop_back();
    if (native.size() < 4) return text;  // "/" or "C:\": nothing worth hiding, and would eat every path
    std::string back = native, fwd = native;
    std::replace(back.begin(), back.end(), '/', '\\');
    std::replace(fwd.begin(), fwd.end(), '\\', '/');
    replace_all(text, "Z:" + back, "~");  // Wine's view of a Unix home
    replace_all(text, back, "~");
    replace_all(text, fwd, "~");
    return text;
}

Report collect(const Input& in) {
    Report r;
    Out o;
    o.line("STAR FORGE diagnostics, " + now_text("%Y-%m-%d %H:%M:%S %z"));
    o.line();
    o.line("[launcher]");
    o.kv("program", in.app + " " + SF_VERSION);
    o.kv("system", os_version());
#ifdef _WIN32
    o.kv("run as administrator", elevated() ? "YES" : "no");
#endif
    o.kv("data folder", in.data_dir ? ustr(*in.data_dir) : "unknown");
    if (in.data_dir) {
        if (auto loc = read_text(*in.data_dir / "profiles-location")) o.kv("profiles folder chosen in setup", *loc);
        for (const char* f : {"launcher.json", "game-launch.log", "servers.json", "trust.json"}) {
            std::error_code ec;
            if (fs::is_regular_file(*in.data_dir / f, ec)) r.files.emplace_back(std::string("launcher/") + f, *in.data_dir / f);
        }
    }

    o.line();
    o.line("[game]");
    if (in.setup) {
        const Setup& s = *in.setup;
        o.kv("folder", ustr(s.game_dir));
        std::error_code ec;
        uint64_t size = 0;
        if (auto hash = signing::sha256_file(s.game_exe(), &size, ec))
            o.kv("win64/game.exe", bytes(size) + ", sha256 " + *hash);
        else
            o.kv("win64/game.exe", "CANNOT READ: " + io_msg(ec));
        o.kv("drive", space_of(s.game_dir));
        o.kv("tools", ustr(s.tools_dir));
        for (const char* t : {"sc-inject.exe", "sc_agent.dll"}) {
            const auto sz = fs::file_size(s.tools_dir / t, ec);
            o.kv(t, ec ? "MISSING: " + io_msg(ec) : bytes(sz));
        }
        if (s.proton) {
            o.kv("runner", "steam-proton");
            o.kv("proton", ustr(s.proton->dir));
            o.kv("prefix", ustr(s.proton->compat_data));
        }
        if (s.wine) {
            o.kv("runner", ustr(s.wine->program) + (s.wine->umu ? " (umu)" : ""));
            if (s.wine->umu) o.kv("proton", s.wine->proton ? ustr(*s.wine->proton) : "umu's own");
            o.kv("prefix", ustr(s.wine->prefix));
        }
        add_game_logs(r, "shared/", profiles::shared_local_appdata(s));
    } else {
        o.kv("not found", in.setup_error.empty() ? "the game setup hasn't been found yet" : in.setup_error);
    }

    o.line();
    o.line("[profiles]");
    o.kv("folder", ustr(in.profiles_dir));
    std::error_code ec;
    if (!fs::is_directory(in.profiles_dir, ec)) {
        o.kv("problem", ec ? io_msg(ec) : "the folder doesn't exist");
    } else {
        o.kv("drive", space_of(in.profiles_dir));
        o.kv("save test", write_probe(in.profiles_dir));
        std::vector<fs::path> dirs;
        for (fs::directory_iterator it(in.profiles_dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (it->is_directory(e2)) dirs.push_back(it->path());
        }
        if (ec) o.kv("problem", "CANNOT LIST: " + io_msg(ec));
        std::sort(dirs.begin(), dirs.end());
        for (const auto& d : dirs) {
            const std::string n = ustr(d.filename());
            if (n == ".staging") {
                o.line();
                o.line("[.staging] packs being reviewed: " + ustr(d));
                continue;
            }
            profile_section(o, r, in, d);
        }
    }
    r.text = redact(o.text);
    return r;
}

std::string zip(const Report& report) {
    mz_zip_archive z;
    mz_zip_zero_struct(&z);
    if (!mz_zip_writer_init_heap(&z, 0, 0)) throw std::runtime_error("cannot start the diagnostics zip");
    struct Ender {
        mz_zip_archive* z;
        ~Ender() { mz_zip_writer_end(z); }
    } ender{&z};
    auto add = [&](const std::string& name, const std::string& data) {
        if (!mz_zip_writer_add_mem(&z, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION))
            throw std::runtime_error("cannot add " + name + " to the diagnostics zip");
    };
    std::string notes;
    uint64_t total = 0;
    for (const auto& [name, path] : report.files) {
        std::error_code ec;
        auto text = read_text(path, &ec);
        if (!text) {
            notes += name + ": " + io_msg(ec) + "\n";
            continue;
        }
        if (text->size() > kMaxFileBytes) {
            *text = "[... first " + bytes(text->size() - kMaxFileBytes) + " left out ...]\n" + text->substr(text->size() - kMaxFileBytes);
        }
        if (total + text->size() > kMaxZipInput) {
            notes += name + ": left out (the zip is full)\n";
            continue;
        }
        total += text->size();
        add(name, redact(*text));
    }
    add("report.txt", report.text + (notes.empty() ? "" : "\n[files that couldn't be added]\n" + redact(notes)));
    void* buf = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&z, &buf, &size)) throw std::runtime_error("cannot finish the diagnostics zip");
    std::string out(static_cast<const char*>(buf), size);
    mz_free(buf);
    return out;
}

fs::path save(const Report& report, const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw std::runtime_error(io_error(dir, ec));
    const fs::path out = dir / upath("starforge-diagnostics-" + now_text("%Y%m%d-%H%M%S") + ".zip");
    write_or_throw(out, zip(report));
    return out;
}

std::optional<fs::path> default_dir() {
    auto h = home();
    if (!h) return std::nullopt;
    std::error_code ec;
    if (fs::is_directory(*h / "Downloads", ec)) return *h / "Downloads";
    return h;
}

}  // namespace sf::launch::diagnostics

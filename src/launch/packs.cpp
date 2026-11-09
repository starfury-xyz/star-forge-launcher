#include "launch/packs.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <functional>
#include <set>
#include <chrono>
#include <stdexcept>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include <miniz.h>

#include "crypto/sha256.hpp"
#include "launch/net.hpp"
#include "launch/patching.hpp"
#include "launch/profiles.hpp"
#include "launch/signing.hpp"
#include "pack/serde.hpp"
#include "patch/core.hpp"

namespace sf::launch::packs {
namespace {

constexpr uint64_t kMaxPackBytes = 8ull << 30;      // largest pack we unpack or download (packs may carry .pak archives)
constexpr uint64_t kMaxManifestBytes = 1ull << 20;  // largest published pack.json we read
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

bool starts_with(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
bool path_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

bool valid_utf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        uint32_t cp = n == 1 ? c : c & (0x7f >> n);
        for (size_t k = 1; k < n; ++k) {
            unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d >> 6) != 2) return false;
            cp = cp << 6 | (d & 0x3f);
        }
        // No overlong forms, surrogates or values past U+10FFFF.
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff))
            return false;
        i += n;
    }
    return true;
}

std::string percent_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '%') {
            if (i + 3 > s.size() || !std::isxdigit(static_cast<unsigned char>(s[i + 1])) ||
                !std::isxdigit(static_cast<unsigned char>(s[i + 2])))
                throw std::runtime_error("bad %-escape in link");
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 3;
        } else {
            out += s[i] == '+' ? ' ' : s[i];
            ++i;
        }
    }
    if (!valid_utf8(out)) throw std::runtime_error("link is not valid UTF-8");
    return out;
}

FILE* open_file(const fs::path& p, const char* mode) {
#ifdef _WIN32
    return _wfopen(p.wstring().c_str(), std::wstring(mode, mode + std::strlen(mode)).c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}

// Folder for packs being reviewed. It sits inside the profiles folder so installing is a rename
// on the same drive (a rename can't cross drives).
fs::path pack_staging_root(const fs::path& profiles_dir) { return profiles_dir / ".staging"; }

fs::path new_staging_dir_in(const fs::path& root) {
    static std::atomic<uint32_t> counter{0};
    const auto stamp =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const fs::path dir = root / (std::to_string(getpid()) + "-" + std::to_string(stamp) + "-" + std::to_string(counter++));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw std::runtime_error(io_error(dir, ec));
    return dir;
}

}  // namespace

void check_url(const std::string& url) {
    if (url.rfind("https://", 0) == 0) return;
    if (url.rfind("http://", 0) == 0) {
        const std::string rest = url.substr(7);
        std::string host = rest.substr(0, rest.find_first_of("/?#"));
        // Without a port, if what follows the last ':' is all digits.
        size_t colon = host.rfind(':');
        if (colon != std::string::npos &&
            host.find_first_not_of("0123456789", colon + 1) == std::string::npos)
            host = host.substr(0, colon);
        if (host == "localhost" || host == "127.0.0.1" || host == "[::1]") return;
        throw std::runtime_error(url + ": packs must be downloaded over https");
    }
    throw std::runtime_error(url + ": not an http(s) link");
}

std::optional<fs::path> staging_root() {
    auto d = profiles::data_dir();
    if (!d) return std::nullopt;
    return *d / "staging";
}

fs::path new_staging_dir() {
    auto root = staging_root();
    if (!root) throw std::runtime_error("cannot work out the launcher data folder");
    return new_staging_dir_in(*root);
}

void clear_staging(const std::optional<fs::path>& profiles_dir) {
    std::error_code ec;
    if (auto dir = staging_root()) fs::remove_all(*dir, ec);
    if (profiles_dir) fs::remove_all(pack_staging_root(*profiles_dir), ec);
}


// --- what a pack may contain ---

bool allowed(const std::string& rel) {
    const std::string top = rel.substr(0, rel.find('/'));
    const bool file = rel.find('/') == std::string::npos;
    size_t start = 0;
    for (;;) {
        if (rel.compare(start, 1, ".") == 0) return false;  // .gitkeep, .DS_Store, ...
        size_t slash = rel.find('/', start);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (rel == pack::kPackFile || rel == pack::kManifestFile || rel == pack::kSignatureFile) return true;
    if ((top == pack::kResourcesDir || top == pack::kFilesDir || top == pack::kPatchesDir) && !file) return true;
    const std::string t = lower(top);
    return file && (starts_with(t, "readme") || starts_with(t, "license") || starts_with(t, "changelog"));
}

std::optional<std::string> clean_rel(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        size_t at = path.find_first_of("/\\", start);
        std::string part = path.substr(start, at - start);
        if (!part.empty()) parts.push_back(part);
        if (at == std::string::npos) break;
        start = at + 1;
    }
    if (parts.empty() || !std::all_of(parts.begin(), parts.end(), plain_part)) return std::nullopt;
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? "/" : "") + parts[i];
    return out;
}

Source Source::parse(const std::string& raw) {
    const std::string text = trim(raw);
    const std::string scheme = std::string(kScheme) + "://";
    Source s;
    if (starts_with(text, scheme)) {
        std::string rest = text.substr(scheme.size());
        std::optional<std::string> query;
        if (starts_with(rest, "install")) {
            std::string r = rest.substr(7);
            r.erase(0, r.find_first_not_of('/'));
            if (r.empty() || r[0] != '?') r.clear(), query = std::nullopt;
            else query = r.substr(1);
        }
        std::optional<std::string> url;
        if (query) {
            size_t start = 0;
            for (;;) {
                size_t amp = query->find('&', start);
                std::string kv = query->substr(start, amp - start);
                if (starts_with(kv, "url=")) {
                    url = kv.substr(4);
                    break;
                }
                if (amp == std::string::npos) break;
                start = amp + 1;
            }
        }
        if (!url) throw std::runtime_error(std::string(kScheme) + " link without an install url: " + text);
        s.kind = Url;
        s.url = percent_decode(*url);
        check_url(s.url);
        return s;
    }
    if (starts_with(text, "http://") || starts_with(text, "https://")) {
        check_url(text);
        s.kind = Url;
        s.url = text;
        return s;
    }
    if (text.find("://") != std::string::npos) throw std::runtime_error("unsupported link: " + text);
    s.path = upath(text);
    return s;
}

Reach Reach::of(const fs::path& root, const pack::Pack& pack) {
    return Reach{file_list(root).size(), pack::patchset_files(root).size(), pack.patches.core.size(), pack.patches.plugins.size()};
}

std::vector<std::string> file_list(const fs::path& profile_dir) {
    std::vector<std::string> out;
    std::function<void(const fs::path&, const std::string&)> walk = [&](const fs::path& dir, const std::string& prefix) {
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = ustr(it->path().filename());
            if (name[0] == '.') continue;
            const std::string rel = prefix.empty() ? name : prefix + "/" + name;
            std::error_code e2;
            const fs::file_status st = it->symlink_status(e2);
            if (fs::is_directory(st)) walk(it->path(), rel);
            else if (fs::is_regular_file(st)) out.push_back(rel);
        }
    };
    walk(profile_dir / pack::kFilesDir, "");
    std::sort(out.begin(), out.end());
    return out;
}

namespace {

// --- unpacking ---

std::string zip_error(mz_zip_archive& zip) { return mz_zip_get_error_string(mz_zip_get_last_error(&zip)); }

void unzip(const fs::path& zip_path, const fs::path& dest) {
    const std::string zp = ustr(zip_path);
    FILE* f = open_file(zip_path, "rb");
    if (!f) throw std::runtime_error(io_error(zip_path, std::error_code(errno, std::generic_category())));
    struct Closer {
        FILE* f;
        mz_zip_archive* zip = nullptr;
        ~Closer() {
            if (zip) mz_zip_reader_end(zip);
            std::fclose(f);
        }
    } closer{f};
    std::error_code ec;
    const uint64_t size = fs::file_size(zip_path, ec);
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (ec || !mz_zip_reader_init_cfile(&zip, f, size, 0))
        throw std::runtime_error(zp + ": not a pack zip (" + (ec ? io_msg(ec) : zip_error(zip)) + ")");
    closer.zip = &zip;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (count > kMaxEntries) throw std::runtime_error(zp + ": too many files");
    auto name_of = [&](mz_uint i) {
        mz_uint n = mz_zip_reader_get_filename(&zip, i, nullptr, 0);
        std::string name(n, '\0');
        if (n) mz_zip_reader_get_filename(&zip, i, name.data(), n);
        if (!name.empty() && name.back() == '\0') name.pop_back();
        return name;
    };
    std::vector<std::string> names;
    for (mz_uint i = 0; i < count; ++i)
        if (auto rel = clean_rel(name_of(i))) names.push_back(*rel);
    // pack.json at the top, or everything inside a single folder.
    std::string prefix;
    if (std::find(names.begin(), names.end(), pack::kPackFile) == names.end()) {
        std::set<std::string> tops;
        for (const auto& n : names) tops.insert(n.substr(0, n.find('/')));
        if (tops.size() != 1 ||
            std::find(names.begin(), names.end(), *tops.begin() + "/" + pack::kPackFile) == names.end())
            throw std::runtime_error(zp + ": no " + pack::kPackFile + " at the top of the zip");
        prefix = *tops.begin() + "/";
    }
    uint64_t total = 0;
    for (mz_uint i = 0; i < count; ++i) {
        auto cleaned = clean_rel(name_of(i));
        if (!cleaned || !starts_with(*cleaned, prefix)) continue;
        const std::string rel = cleaned->substr(prefix.size());
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) throw std::runtime_error(zp + ": " + zip_error(zip));
        if (mz_zip_reader_is_file_a_directory(&zip, i) || !allowed(rel)) continue;
        if ((st.m_version_made_by >> 8) == 3 && ((st.m_external_attr >> 16) & 0170000) == 0120000) continue;  // symlink
        const fs::path out = dest / upath(rel);
        fs::create_directories(out.parent_path(), ec);
        if (ec) throw std::runtime_error(io_error(out, ec));
        FILE* of = open_file(out, "wb");
        if (!of) throw std::runtime_error(io_error(out, std::error_code(errno, std::generic_category())));
        struct Sink {
            FILE* f;
            uint64_t total;
            bool too_big = false, write_failed = false;
            int err = 0;
        } sink{of, total};
        auto write = [](void* opaque, mz_uint64, const void* buf, size_t n) -> size_t {
            auto* s = static_cast<Sink*>(opaque);
            s->total += n;
            if (s->total > kMaxPackBytes) {
                s->too_big = true;
                return 0;
            }
            if (std::fwrite(buf, 1, n, s->f) != n) {
                s->write_failed = true;
                s->err = errno;
                return 0;
            }
            return n;
        };
        const bool ok = mz_zip_reader_extract_to_callback(&zip, i, write, &sink, 0);
        const bool closed = std::fclose(of) == 0;
        total = sink.total;
        if (sink.too_big) throw std::runtime_error(zp + ": unpacks to more than " + std::to_string(kMaxPackBytes >> 30) + " GB");
        if (sink.write_failed || !closed)
            throw std::runtime_error(rel + ": " + io_msg(std::error_code(sink.err ? sink.err : errno, std::generic_category())));
        if (!ok) throw std::runtime_error(rel + ": " + zip_error(zip));
    }
}

void copy_pack_folder(const fs::path& src, const fs::path& dest) {
    if (!is_file(src / pack::kPackFile))
        throw std::runtime_error(ustr(src) + ": no " + pack::kPackFile + " in this folder");
    size_t count = 0;
    std::function<void(const std::string&)> walk = [&](const std::string& rel) {
        const fs::path dir = rel.empty() ? src : src / upath(rel);
        std::error_code ec;
        fs::directory_iterator it(dir, ec);
        if (ec) throw std::runtime_error(io_error(dir, ec));
        for (; it != fs::directory_iterator(); it.increment(ec)) {
            const std::string name = ustr(it->path().filename());
            const std::string child = rel.empty() ? name : rel + "/" + name;
            std::error_code e2;
            const fs::file_status st = it->symlink_status(e2);
            if (e2) continue;
            if (fs::is_directory(st) && (child.find('/') != std::string::npos || child == pack::kResourcesDir ||
                                         child == pack::kFilesDir || child == pack::kPatchesDir)) {
                walk(child);
            } else if (fs::is_regular_file(st) && allowed(child)) {
                if (++count > kMaxEntries) throw std::runtime_error(ustr(src) + ": too many files");
                const fs::path out = dest / upath(child);
                fs::create_directories(out.parent_path(), e2);
                if (e2) throw std::runtime_error(io_error(out, e2));
                fs::copy_file(it->path(), out, fs::copy_options::overwrite_existing, e2);
                if (e2) throw std::runtime_error(io_error(out, e2));
            }
        }
    };
    walk("");
}

fs::path find_root(const fs::path& content) {
    if (is_file(content / pack::kPackFile)) return content;
    throw std::runtime_error(std::string("no ") + pack::kPackFile + " found in the pack");
}

pack::Pack read_published(const fs::path& path, const std::string& url) {
    std::error_code ec;
    auto text = read_text(path, &ec);
    if (!text) throw std::runtime_error(url + ": " + io_msg(ec));
    try {
        pack::Pack p = pack::Pack::from_json(*text);
        p.validate();
        return p;
    } catch (const pack::Error& e) {
        throw std::runtime_error(url + ": " + e.what());
    }
}

bool looks_like_json(const fs::path& path) {
    char buf[64];
    size_t n = 0;
    if (FILE* f = open_file(path, "rb")) {
        n = std::fread(buf, 1, sizeof buf, f);
        std::fclose(f);
    }
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\f') continue;
        return buf[i] == '{';
    }
    return false;
}

Staged finish_stage(const Source& source, const fs::path& dir, const fs::path& content, const std::string& origin,
                    bool verified, const std::optional<pack::Pack>& published) {
    const fs::path root = find_root(content);
    pack::Pack pack = profiles::read_pack(root);
    pack.validate();
    const signing::Signature signature = signing::verify(root, signing::Depth::Full);
    const trust::Trust trust = trust::check(pack.id, signature);
    std::optional<remote::FileList> list;
    if (pack.files_from) {
        if (path_exists(root / pack::kFilesDir))
            throw std::runtime_error(std::string("the pack has both a ") + pack::kFilesDir + "/ folder and files_from; use one or the other");
        list = remote::fetch_list(*pack.files_from, trust.key ? &*trust.key : nullptr);
    }
    std::vector<std::string> warnings;
    if (published) {
        if (published->id != pack.id || published->version != pack.version)
            throw std::runtime_error("the published pack.json describes " + published->id + " " + published->version +
                                     " but the download contains " + pack.id + " " + pack.version);
        // Installed from a published pack.json: check that same address for updates. A signed
        // pack.json can't be edited, so it must name it itself.
        if (!pack.update && trust.kind == trust::Trust::Unsigned && source.kind == Source::Url)
            pack.update = pack::Update{source.url, std::nullopt};
    }
    // Signed packs stay byte for byte as signed.
    if (trust.kind == trust::Trust::Unsigned) {
        pack.download.reset();
        profiles::write_pack(root, pack);
    }

    if (const auto& min = pack.requires_.launcher) {
        auto need = pack::parse_version(*min);
        if (!need) throw std::runtime_error("requires.launcher " + pack::serde::debug_str(*min) + ": expected x.y.z");
        auto have = pack::parse_version(SF_VERSION).value_or(std::tuple<uint64_t, uint64_t, uint64_t>{});
        if (have < *need)
            throw std::runtime_error(pack.name + " needs STAR FORGE Launcher " + *min + " or newer (this is " + SF_VERSION +
                                     "); update the launcher first");
    }
    const pack::Branding& b = pack.branding;
    std::vector<std::pair<const char*, std::vector<std::string>>> art{{"logo", {}}, {"icon", {}}, {"background", b.background}};
    if (b.logo) art[0].second.push_back(*b.logo);
    if (b.icon) art[1].second.push_back(*b.icon);
    for (const auto& [key, paths] : art)
        for (const std::string& p : paths)
            if (!is_file(root / upath(p)))
                warnings.push_back(std::string("branding.") + key + " points at " + p + ", which is not in the pack");
    patch::core::sets(pack.patches.core);
    for (const auto& l : patching::load_sets(root))
        if (!l.set) warnings.push_back("Broken patch file: " + l.error);
    Reach reach = Reach::of(root, pack);
    if (list) {
        reach.files = list->entries.size();
        if (!list->is_signed)
            warnings.push_back("Its game files come from " + list->host() +
                               " without a signature; they are checked against the list's hashes only.");
    }
    if (reach.runs_code() && trust.kind == trust::Trust::Unsigned)
        warnings.push_back(
            "This pack changes the game's code but isn't signed, so there is no telling who made it. Only install it from a source you trust.");
    for (const std::string& p : pack.patches.plugins) {
        auto rel = pack::patches_rel(p);
        if (!rel || !is_file(root / pack::kPatchesDir / upath(*rel)))
            warnings.push_back("patches.plugins points at " + p + ", which is not in the pack");
    }
    if (!pack.patches.plugins.empty())
        warnings.push_back(
            "This pack runs its own native code inside the game (plugins). Only install it if you trust whoever made it completely.");
    if (!pack.update) warnings.push_back("This pack has no update address, so the launcher can't check for new versions.");
    std::vector<std::string> files;
    if (list) {
        for (const auto& e : list->entries) files.push_back(e.path);
        std::sort(files.begin(), files.end());
    } else {
        files = file_list(root);
    }
    return Staged{dir, root, std::move(pack), origin, verified, trust, reach, std::move(list), std::move(files), std::move(warnings)};
}

Staged stage_into(const Source& source, const fs::path& dir) {
    const fs::path content = dir / "content";
    std::string origin;
    bool verified = false;
    std::optional<pack::Pack> published;
    std::error_code ec;
    if (source.kind == Source::Path) {
        origin = ustr(source.path);
        if (fs::is_directory(source.path, ec)) copy_pack_folder(source.path, content);
        else unzip(source.path, content);
    } else {
        const std::string& url = source.url;
        const fs::path download = dir / "download";
        net::fetch(url, download, kMaxPackBytes);
        origin = url;
        if (looks_like_json(download)) {
            pack::Pack manifest = read_published(download, url);
            if (!manifest.download) {
                fs::create_directories(content, ec);
                if (ec) throw std::runtime_error(io_error(content, ec));
                fs::rename(download, content / pack::kPackFile, ec);
                if (ec) throw std::runtime_error(io_msg(ec));
                return finish_stage(source, dir, content, url, false, manifest);
            }
            const pack::Download zip = *manifest.download;
            check_url(zip.url);
            crypto::Sha256 hasher;
            net::fetch(zip.url, download, kMaxPackBytes, &hasher);
            if (lower(hasher.hex()) != lower(zip.sha256))
                throw std::runtime_error(zip.url + ": download does not match the SHA-256 in " + url + "; not installing it");
            unzip(download, content);
            verified = true;
            published = std::move(manifest);
        } else {
            unzip(download, content);
        }
    }
    fs::remove(dir / "download", ec);
    return finish_stage(source, dir, content, origin, verified, published);
}

// The parts of a profile folder that belong to the pack.
const char* const kPackParts[] = {pack::kPackFile, pack::kManifestFile, pack::kSignatureFile,
                                  pack::kResourcesDir, pack::kFilesDir, pack::kPatchesDir};

bool lists_files(const fs::path& dir) {
    try {
        return profiles::read_pack(dir).files_from.has_value();
    } catch (const std::exception&) {
        return false;
    }
}

void replace_pack(const fs::path& new_root, const fs::path& profile) {
    // A pack that downloads its files from a list keeps the ones it has, so an update only
    // fetches what changed. Otherwise files/ is the pack's.
    const bool new_listed = lists_files(new_root), old_listed = lists_files(profile);
    const bool keep_files = new_listed && old_listed;
    if (!new_listed) remote::forget(profile);
    const fs::path old = profile / ".update-old";
    std::error_code ec;
    fs::remove_all(old, ec);
    fs::create_directories(old, ec);
    if (ec) throw std::runtime_error(io_error(old, ec));
    std::vector<const char*> moved_out, moved_in;
    std::string error;
    for (const char* part : kPackParts) {
        if (keep_files && std::strcmp(part, pack::kFilesDir) == 0) continue;
        if (!path_exists(profile / part)) continue;
        fs::rename(profile / part, old / part, ec);
        if (ec) {
            error = io_error(profile / part, ec);
            break;
        }
        moved_out.push_back(part);
    }
    if (error.empty()) {
        for (const char* part : kPackParts) {
            if (keep_files && std::strcmp(part, pack::kFilesDir) == 0) continue;
            if (!path_exists(new_root / part)) continue;
            fs::rename(new_root / part, profile / part, ec);
            if (ec) {
                error = io_error(profile / part, ec);
                break;
            }
            moved_in.push_back(part);
        }
    }
    if (!error.empty()) {
        // Put the old pack back.
        for (const char* part : moved_in) fs::remove_all(profile / part, ec);
        for (const char* part : moved_out) fs::rename(old / part, profile / part, ec);
    }
    fs::remove_all(old, ec);
    if (!error.empty()) throw std::runtime_error(error);
}

std::string install_inner(const Staged& staged, const fs::path& profiles_dir) {
    std::error_code ec;
    fs::create_directories(profiles_dir, ec);
    if (ec) throw std::runtime_error(io_error(profiles_dir, ec));
    // Checked again: another install may have pinned a key since the review.
    const trust::Trust t = trust::check(staged.pack.id, staged.trust.key);
    std::string name;
    if (auto existing = profiles::find_pack(profiles_dir, staged.pack.id)) {
        // Not while a launch is syncing its game files.
        remote::ProfileLock lock = remote::lock_profile(*existing);
        replace_pack(staged.root, *existing);
        name = ustr(existing->filename());
    } else {
        name = profiles::free_name(profiles_dir, staged.pack.name);
        const fs::path target = profiles_dir / upath(name);
        fs::rename(staged.root, target, ec);
        if (ec) throw std::runtime_error(io_error(target, ec));
    }
    if (t.kind == trust::Trust::NewKey) trust::pin(staged.pack.id, *t.key);
    return name;
}

}  // namespace

Staged stage(const Source& source, const fs::path& profiles_dir) {
    const fs::path dir = new_staging_dir_in(pack_staging_root(profiles_dir));
    try {
        return stage_into(source, dir);
    } catch (...) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        throw;
    }
}

void discard(const Staged& staged) {
    std::error_code ec;
    fs::remove_all(staged.dir, ec);
}

Installed install(const Staged& staged, const fs::path& profiles_dir, const remote::Progress& progress) {
    std::string name;
    try {
        name = install_inner(staged, profiles_dir);
    } catch (...) {
        discard(staged);
        throw;
    }
    discard(staged);
    Installed done{name, std::nullopt};
    if (staged.remote) {
        try {
            remote::sync(profiles_dir / upath(name), *staged.remote, progress);
        } catch (const std::exception& e) {
            done.warning = staged.pack.name + " is installed, but its game files didn't all download (" + e.what() +
                           "). They'll be fetched again before you play.";
        }
    }
    return done;
}

trust::Trust verify_installed(const fs::path& profile_dir) {
    const pack::Pack pack = profiles::read_pack(profile_dir);
    signing::Signature signature;
    try {
        signature = signing::verify(profile_dir, signing::Depth::Launch);
    } catch (const std::exception& e) {
        throw std::runtime_error(pack.name + ": " + e.what() + ". Reinstall the pack to repair it.");
    }
    trust::Trust t = trust::check(pack.id, signature);
    if (t.kind == trust::Trust::NewKey) {
        trust::pin(pack.id, *t.key);
        t.kind = trust::Trust::Pinned;
    }
    return t;
}

std::optional<Available> check_update(const pack::Pack& pack) {
    if (!pack.update) return std::nullopt;
    const std::string& url = pack.update->url;
    check_url(url);
    const fs::path dir = new_staging_dir();
    pack::Pack published;
    try {
        net::fetch(url, dir / "pack.json", kMaxManifestBytes);
        published = read_published(dir / "pack.json", url);
    } catch (...) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        throw;
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (published.id != pack.id) throw std::runtime_error(url + ": publishes pack " + published.id + " instead of " + pack.id);
    const auto have = pack::parse_version(pack.version);
    const bool newer = pack::parse_version(published.version) > have;
    bool required = false;
    if (published.update && published.update->min_version)
        if (auto min = pack::parse_version(*published.update->min_version)) required = have < min;
    if (!newer) return std::nullopt;
    return Available{published.version, url, required};
}

std::optional<std::string> game_build_warning(const pack::Pack& pack, const fs::path& game_exe) {
    if (pack.requires_.game.empty()) return std::nullopt;
    std::error_code ec;
    auto hash = signing::sha256_file(game_exe, nullptr, ec);
    if (!hash) return std::nullopt;
    for (const std::string& h : pack.requires_.game)
        if (lower(h) == *hash) return std::nullopt;
    return pack.name + " was made for a different game build; its files and patches may not work with this one.";
}

}  // namespace sf::launch::packs

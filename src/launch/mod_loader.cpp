#include "launch/mod_loader.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include "crypto/sha256.hpp"
#include "launch/packs.hpp"
#include "launch/profiles.hpp"
#include "launch/signing.hpp"
#include "mod/blocks.hpp"
#include "mod/sfmod_lua.hpp"
#include "mod/sfpatch.hpp"
#include "patch/core.hpp"

namespace sf::launch::mod_loader {
namespace {

using nlohmann::json;
using ojson = nlohmann::ordered_json;

constexpr std::size_t kMaxEntries = 50000;
constexpr uint64_t kMaxModBytes = 4ull << 30;
constexpr const char* kComplete = ".complete";  // in a cache folder once it's fully unpacked
const std::string kUiResources = "data/ui/uiresources.txt";  // lowercase: also a key of `merged`

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool is_zip(const fs::path& p) { return lower(ustr(p.extension())) == ".zip"; }

FILE* open_file(const fs::path& p, const char* mode) {
#ifdef _WIN32
    std::wstring m(mode, mode + std::char_traits<char>::length(mode));
    return _wfopen(p.wstring().c_str(), m.c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}

// --- zips ---------------------------------------------------------------------------------

class Zip {
public:
    explicit Zip(const fs::path& path) : path_(ustr(path)) {
        f_ = open_file(path, "rb");
        if (!f_) throw std::runtime_error(io_error(path, std::error_code(errno, std::generic_category())));
        std::error_code ec;
        const uint64_t size = fs::file_size(path, ec);
        mz_zip_zero_struct(&zip_);
        if (ec || !mz_zip_reader_init_cfile(&zip_, f_, size, 0)) {
            std::fclose(f_);
            throw std::runtime_error("not a zip (" + (ec ? io_msg(ec) : std::string(err())) + ")");
        }
        const mz_uint count = mz_zip_reader_get_num_files(&zip_);
        if (count > kMaxEntries) {
            close();
            throw std::runtime_error("too many files");
        }
        for (mz_uint i = 0; i < count; ++i) {
            mz_uint n = mz_zip_reader_get_filename(&zip_, i, nullptr, 0);
            std::string name(n, '\0');
            if (n) mz_zip_reader_get_filename(&zip_, i, name.data(), n);
            if (!name.empty() && name.back() == '\0') name.pop_back();
            names_.push_back(packs::clean_rel(name).value_or(""));
        }
        // mod.json at the top, or everything inside one folder.
        if (find(mod::kManifestFile) < 0) {
            std::set<std::string> tops;
            for (const auto& n : names_)
                if (!n.empty()) tops.insert(n.substr(0, n.find('/')));
            if (tops.size() != 1 || find(*tops.begin() + "/" + mod::kManifestFile) < 0) {
                close();
                throw std::runtime_error(std::string("no ") + mod::kManifestFile + " at the top of the zip");
            }
            prefix_ = *tops.begin() + "/";
        }
    }
    ~Zip() { close(); }
    Zip(const Zip&) = delete;
    Zip& operator=(const Zip&) = delete;

    std::string read(const std::string& rel) {
        int i = find(prefix_ + rel);
        if (i < 0) throw std::runtime_error("no " + rel);
        std::size_t size = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip_, static_cast<mz_uint>(i), &size, 0);
        if (!p) throw std::runtime_error(rel + ": " + err());
        std::string out(static_cast<const char*>(p), size);
        mz_free(p);
        return out;
    }

    void extract(const fs::path& dest) {
        uint64_t total = 0;
        for (mz_uint i = 0; i < names_.size(); ++i) {
            const std::string& n = names_[i];
            if (n.empty() || n.compare(0, prefix_.size(), prefix_) != 0) continue;
            const std::string rel = n.substr(prefix_.size());
            if (rel.empty() || mz_zip_reader_is_file_a_directory(&zip_, i)) continue;
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip_, i, &st)) throw std::runtime_error(rel + ": " + err());
            if ((st.m_version_made_by >> 8) == 3 && ((st.m_external_attr >> 16) & 0170000) == 0120000) continue;  // symlink
            total += st.m_uncomp_size;
            if (total > kMaxModBytes) throw std::runtime_error("unpacks to more than 4 GB");
            const fs::path out = dest / upath(rel);
            std::error_code ec;
            fs::create_directories(out.parent_path(), ec);
            if (ec) throw std::runtime_error(io_error(out, ec));
            if (!mz_zip_reader_extract_to_cfile_wrapper(i, out)) throw std::runtime_error(rel + ": " + err());
        }
    }

private:
    bool mz_zip_reader_extract_to_cfile_wrapper(mz_uint i, const fs::path& out) {
        FILE* of = open_file(out, "wb");
        if (!of) throw std::runtime_error(io_error(out, std::error_code(errno, std::generic_category())));
        bool ok = mz_zip_reader_extract_to_cfile(&zip_, i, of, 0);
        ok = std::fclose(of) == 0 && ok;
        return ok;
    }
    int find(const std::string& name) const {
        for (std::size_t i = 0; i < names_.size(); ++i)
            if (names_[i] == name) return static_cast<int>(i);
        return -1;
    }
    const char* err() { return mz_zip_get_error_string(mz_zip_get_last_error(&zip_)); }
    void close() {
        if (f_) {
            mz_zip_reader_end(&zip_);
            std::fclose(f_);
            f_ = nullptr;
        }
    }

    std::string path_;
    FILE* f_ = nullptr;
    mz_zip_archive zip_;
    std::vector<std::string> names_;
    std::string prefix_;
};

// --- files ----------------------------------------------------------------------------------

// Every regular file under `dir`, relative with '/' separators.
std::vector<std::string> list_files(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string rel = ustr(fs::relative(it->path(), dir, ec));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        out.push_back(rel);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// A file at game path `rel` (any case) under `root`, trying the exact spelling first.
std::optional<fs::path> find_ci(const fs::path& root, const std::string& rel) {
    std::error_code ec;
    fs::path exact = root / upath(rel);
    if (fs::is_regular_file(exact, ec)) return exact;
    fs::path cur = root;
    std::size_t p = 0;
    while (p <= rel.size()) {
        std::size_t e = rel.find('/', p);
        std::string part = rel.substr(p, e == std::string::npos ? std::string::npos : e - p);
        bool found = false;
        for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec)) {
            if (lower(ustr(it->path().filename())) == lower(part)) {
                cur = it->path();
                found = true;
                break;
            }
        }
        if (!found) return std::nullopt;
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return fs::is_regular_file(cur, ec) ? std::optional<fs::path>(cur) : std::nullopt;
}

std::string sanitize(std::string s) {
    for (char& c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'))
            c = '_';
    return s;
}

std::string rel_slash(const fs::path& p, const fs::path& base) {
    std::error_code ec;
    std::string r = ustr(fs::relative(p, base, ec));
    std::replace(r.begin(), r.end(), '\\', '/');
    return r;
}

// The loadingGroups of the script `file` in a uiresources.txt, or nullopt.
std::optional<std::string> groups_of(const std::string& uiresources, const std::string& file) {
    try {
        mod::Doc doc = mod::parse(uiresources);
        for (const mod::Node& n : doc.top) {
            if (!n.block || !mod::iequals(n.key(), "Item")) continue;
            const mod::Node* name = nullptr;
            const mod::Node* groups = nullptr;
            for (const mod::Node& c : n.children) {
                if (c.block) continue;
                if (mod::iequals(c.key(), "fileName")) name = &c;
                if (mod::iequals(c.key(), "loadingGroups")) groups = &c;
            }
            if (name && groups && mod::iequals(name->value(doc.text), file)) return groups->value(doc.text);
        }
    } catch (const mod::ParseError&) {
    }
    return std::nullopt;
}

// The uiresources.txt patch for a mod's `scripts`. A script without `groups` loads with the
// script it comes after (a hook on a FIRST_LEVEL script must run at FIRST_LEVEL too).
std::string scripts_patch(const mod::Manifest& m, const std::optional<std::string>& uiresources) {
    std::string out = "patch \"data/ui/uiresources.txt\"\n{\n";
    // Entries keep the order they're listed in: one with the same `after` as the entry before it
    // goes after that entry, not straight after the shared anchor (which would reverse them).
    std::optional<std::string> prev_after, prev_file;
    for (mod::Script s : m.scripts) {
        std::string groups = "APP_START";
        if (s.groups) groups = *s.groups;
        else if (s.after && uiresources)
            if (auto g = groups_of(*uiresources, *s.after)) groups = *g;
        const std::optional<std::string> written = s.after;
        if (s.after && prev_after && *s.after == *prev_after) s.after = prev_file;
        prev_after = written;
        prev_file = s.file;
        std::string item = "\t\tItem\n\t\t{\n\t\t\tfileName \"" + s.file + "\"\n\t\t\tresourceType \"" + s.type +
                           "\"\n\t\t\tloadingGroups \"" + groups + "\"\n\t\t}\n";
        if (s.after)
            out += "\tadd after Item[fileName=\"" + *s.after + "\"]\n\t{\n" + item + "\t}\n";
        else
            out += "\tadd\n\t{\n" + item + "\t}\n";
    }
    return out + "}\n";
}

// uiresources.txt with sfmod.lua registered first, before every script.
std::string with_sfmod(const std::string& text) {
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::size_t at = text.rfind("\xEF\xBB\xBF", 0) == 0 ? 3 : 0;
    std::string item = "Item" + eol + "{" + eol + "\tfileName \"" + mod::kSfmodScript + "\"" + eol +
                       "\tresourceType \"SCRIPT\"" + eol + "\tloadingGroups \"APP_START\"" + eol + "}" + eol + eol;
    return text.substr(0, at) + item + text.substr(at);
}

// --- report ---------------------------------------------------------------------------------

ojson to_json(const Result& r) {
    ojson j = ojson::object();
    j["fingerprint"] = r.fingerprint;
    ojson mods = ojson::array();
    for (const auto& m : r.mods) {
        ojson e = ojson::object();
        e["id"] = m.id;
        e["name"] = m.name;
        e["version"] = m.version;
        e["source"] = ustr(m.source);
        e["on"] = m.on;
        e["loaded"] = m.loaded;
        if (!m.reason.empty()) e["reason"] = m.reason;
        if (!m.notes.empty()) e["notes"] = m.notes;
        mods.push_back(e);
    }
    j["mods"] = mods;
    ojson c = ojson::array();
    for (const auto& x : r.conflicts) c.push_back(ojson{{"what", x.what}, {"mods", x.mods}});
    j["conflicts"] = c;
    j["core_patches"] = r.core_patches;
    j["layers"] = r.layers;
    j["notes"] = r.notes;
    return j;
}

std::vector<std::string> strs(const json& j, const char* key) {
    std::vector<std::string> out;
    if (auto it = j.find(key); it != j.end() && it->is_array())
        for (const auto& s : *it)
            if (s.is_string()) out.push_back(s.get<std::string>());
    return out;
}

void remove_quietly(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

}  // namespace

std::string Found::id() const { return manifest ? manifest->id : ustr(source.filename()); }

std::vector<Found> scan(const fs::path& profile_dir) {
    std::vector<Found> out;
    const fs::path dir = profile_dir / kModsDir;
    std::error_code ec;
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = ustr(it->path().filename());
        if (name.empty() || name[0] == '.') continue;
        if (it->is_directory(ec) || (it->is_regular_file(ec) && is_zip(it->path()))) entries.push_back(it->path());
    }
    std::sort(entries.begin(), entries.end());
    for (const auto& p : entries) {
        Found f;
        f.source = p;
        try {
            std::string text;
            if (is_zip(p)) {
                Zip z(p);
                text = z.read(mod::kManifestFile);
            } else {
                std::error_code e2;
                auto t = read_text(p / mod::kManifestFile, &e2);
                if (!t) throw std::runtime_error(std::string("no ") + mod::kManifestFile + " in this folder");
                text = *t;
            }
            f.manifest = mod::Manifest::from_json(text);
        } catch (const std::exception& e) {
            f.error = e.what();
        }
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<std::string> load_order(const std::vector<Found>& found, const pack::ModChoices& choices) {
    std::set<std::string> ids;
    for (const auto& f : found) ids.insert(f.id());
    std::vector<std::string> out;
    std::set<std::string> placed;
    for (const auto& id : choices.order)
        if (ids.count(id) && placed.insert(id).second) out.push_back(id);
    // The rest: a stable topological sort on the hints, smallest id first among the ready ones.
    std::set<std::string> rest;
    for (const auto& id : ids)
        if (!placed.count(id)) rest.insert(id);
    std::map<std::string, std::set<std::string>> must_follow;  // id -> ids that must load before it
    for (const auto& f : found) {
        if (!f.manifest || !rest.count(f.id())) continue;
        for (const auto& a : f.manifest->after)
            if (rest.count(a)) must_follow[f.id()].insert(a);
        for (const auto& b : f.manifest->before)
            if (rest.count(b)) must_follow[b].insert(f.id());
    }
    while (!rest.empty()) {
        std::string pick;
        for (const auto& id : rest) {
            bool ready = true;
            for (const auto& dep : must_follow[id])
                if (rest.count(dep)) ready = false;
            if (ready) {
                pick = id;
                break;
            }
        }
        if (pick.empty()) pick = *rest.begin();  // a cycle: hints can't all hold
        out.push_back(pick);
        rest.erase(pick);
    }
    return out;
}

Result build(const fs::path& profile_dir, const fs::path& game_dir, const std::function<std::string()>& game_exe_sha256) {
    Result r;
    const fs::path mods_dir = profile_dir / kModsDir;
    const fs::path layers_file = mods_dir / kLayersFile, report_file = mods_dir / kReportFile;
    const fs::path merged_dir = mods_dir / kMergedDir, cache_dir = mods_dir / kCacheDir;
    std::error_code ec;
    if (!fs::is_directory(mods_dir, ec)) return r;

    std::vector<Found> found = scan(profile_dir);
    const pack::ModChoices choices = profiles::read_local(profile_dir).mods;
    const std::vector<std::string> order = load_order(found, choices);
    const std::set<std::string> off(choices.off.begin(), choices.off.end());
    const std::vector<std::string> core_ids = patch::core::ids();
    std::optional<std::string> game_sha;

    // One Mod per id, in load order; a second mod with the same id is never loaded.
    std::map<std::string, const Found*> by_id;
    for (const auto& id : order) {
        Mod m;
        m.id = id;
        for (const auto& f : found) {
            if (f.id() != id) continue;
            if (!by_id.count(id)) {
                by_id[id] = &f;
                m.source = f.source;
                if (f.manifest) {
                    m.name = f.manifest->name;
                    m.version = f.manifest->version;
                }
            } else {
                m.notes.push_back(ustr(f.source.filename()) + " has the same id and is ignored");
            }
        }
        const Found& f = *by_id[id];
        m.on = !off.count(id);
        if (!m.on) m.reason = "turned off";
        else if (!f.manifest) m.reason = f.error;
        else if (f.manifest->server) m.reason = "changes what the server must also have; only a server's pack can turn it on";
        else {
            for (const auto& n : f.manifest->needs)
                if (std::find(core_ids.begin(), core_ids.end(), n) == core_ids.end())
                    m.reason = "needs core patch \"" + n + "\", which this launcher doesn't have";
            if (m.reason.empty() && !f.manifest->game.empty()) {
                if (!game_sha) game_sha = game_exe_sha256 ? game_exe_sha256() : "";
                if (!game_sha->empty() &&
                    std::find(f.manifest->game.begin(), f.manifest->game.end(), *game_sha) == f.manifest->game.end())
                    m.reason = "made for a different game build";
            }
        }
        m.loaded = m.reason.empty();
        r.mods.push_back(std::move(m));
    }
    // Requirements, until nothing more drops out.
    for (bool changed = true; changed;) {
        changed = false;
        std::map<std::string, const Mod*> loaded;
        for (const auto& m : r.mods)
            if (m.loaded) loaded[m.id] = &m;
        for (auto& m : r.mods) {
            if (!m.loaded) continue;
            for (const auto& [rid, range] : by_id[m.id]->manifest->requires_) {
                if (rid == mod::kSfmodId) {
                    if (!mod::version_matches(mod::kSfmodVersion, range)) {
                        m.reason = std::string("needs the Lua API (sfmod) ") + range + "; this launcher has " +
                                   mod::kSfmodVersion + ", update it";
                        m.loaded = false;
                        changed = true;
                        break;
                    }
                    continue;
                }
                auto it = loaded.find(rid);
                if (it == loaded.end()) m.reason = "needs mod \"" + rid + "\"";
                else if (!mod::version_matches(it->second->version, range))
                    m.reason = "needs mod \"" + rid + "\" " + range + " (installed: " + it->second->version + ")";
                if (!m.reason.empty()) {
                    m.loaded = false;
                    changed = true;
                    break;
                }
            }
        }
    }

    // Unpack zips; keep only cache folders still in use.
    std::map<std::string, fs::path> root;  // id -> the mod's top folder
    std::set<std::string> cache_used;
    for (auto& m : r.mods) {
        const Found& f = *by_id[m.id];
        if (!is_zip(f.source)) {
            root[m.id] = f.source;
            continue;
        }
        std::string hash;
        try {
            hash = signing::sha256_file(f.source).substr(0, 12);
        } catch (const std::exception& e) {
            if (m.loaded) {
                m.loaded = false;
                m.reason = e.what();
            }
            continue;
        }
        const std::string name = sanitize(m.id) + "-" + hash;
        cache_used.insert(name);
        const fs::path dest = cache_dir / name;
        root[m.id] = dest;
        if (!m.loaded || fs::exists(dest / kComplete, ec)) continue;
        const fs::path tmp = cache_dir / (".tmp-" + name);
        remove_quietly(tmp);
        remove_quietly(dest);
        try {
            Zip(f.source).extract(tmp);
            write_or_throw(tmp / kComplete, "");
            fs::rename(tmp, dest);
        } catch (const std::exception& e) {
            remove_quietly(tmp);
            m.loaded = false;
            m.reason = std::string("could not unpack: ") + e.what();
        }
    }
    for (fs::directory_iterator it(cache_dir, ec), end; !ec && it != end; it.increment(ec))
        if (!cache_used.count(ustr(it->path().filename()))) remove_quietly(it->path());

    std::vector<const Mod*> loaded;
    for (const auto& m : r.mods)
        if (m.loaded) loaded.push_back(&m);
    auto mod_of = [&](const std::string& id) -> Mod& {
        for (auto& m : r.mods)
            if (m.id == id) return m;
        throw std::logic_error("no mod " + id);
    };

    // Whole files: who replaces what (the last loaded wins), and the pack's files they cover.
    std::map<std::string, std::vector<std::string>> file_mods;  // lower rel -> ids in load order
    std::map<std::string, std::string> file_spelling;
    for (const Mod* m : loaded) {
        for (const auto& rel : list_files(root[m->id] / mod::kFilesDir)) {
            file_mods[lower(rel)].push_back(m->id);
            file_spelling.emplace(lower(rel), rel);
        }
    }
    for (const auto& [key, ids] : file_mods) {
        if (ids.size() > 1) r.conflicts.push_back({"file " + file_spelling[key], ids});
        if (find_ci(profile_dir / pack::kFilesDir, file_spelling[key]))
            mod_of(ids.back()).notes.push_back("replaces the pack's " + file_spelling[key]);
    }

    // Data patches, mod by mod in load order; each .sfpatch file applies whole or not at all.
    struct Merged {
        std::string rel;
        std::string text;
        std::vector<std::pair<std::string, std::string>> touched;  // address, mod id
    };
    std::map<std::string, Merged> merged;  // lower rel -> current text
    auto base_text = [&](const std::string& rel) -> std::optional<std::string> {
        auto it = file_mods.find(lower(rel));
        std::optional<fs::path> p;
        if (it != file_mods.end()) p = find_ci(root[it->second.back()] / mod::kFilesDir, rel);
        if (!p) p = find_ci(profile_dir / pack::kFilesDir, rel);
        if (!p) p = find_ci(game_dir, rel);
        if (!p) return std::nullopt;
        return read_or_throw(*p);
    };
    for (const Mod* m : loaded) {
        const mod::Manifest& man = *by_id[m->id]->manifest;
        std::vector<std::pair<std::string, std::string>> sources;  // name, text
        if (!man.scripts.empty()) {
            auto it = merged.find(kUiResources);
            sources.emplace_back("mod.json scripts",
                                 scripts_patch(man, it != merged.end() ? std::optional(it->second.text) : base_text(kUiResources)));
        }
        for (const auto& rel : list_files(root[m->id] / mod::kPatchesDir)) {
            if (lower(ustr(fs::path(rel).extension())) != ".sfpatch") continue;
            sources.emplace_back(std::string(mod::kPatchesDir) + "/" + rel,
                                 read_or_throw(root[m->id] / mod::kPatchesDir / upath(rel)));
        }
        for (const auto& [name, text] : sources) {
            try {
                mod::Patch patch = mod::parse_patch(text, m->id + "/" + name);
                std::map<std::string, Merged> work;
                for (const auto& fp : patch.files) {
                    const std::string key = lower(fp.file);
                    if (!work.count(key)) {
                        if (merged.count(key)) work[key] = merged[key];
                        else if (auto t = base_text(fp.file)) work[key] = Merged{fp.file, *t, {}};
                        else throw mod::PatchError(patch.name + ":" + std::to_string(fp.line) + ": " + fp.file +
                                                   " not found (files inside .pak archives can't be patched yet)");
                    }
                    std::vector<std::string> touched;
                    work[key].text = mod::apply(std::move(work[key].text), fp, patch.name, &touched);
                    for (auto& a : touched) work[key].touched.emplace_back(std::move(a), m->id);
                }
                for (auto& [k, v] : work) merged[k] = std::move(v);
            } catch (const std::exception& e) {
                mod_of(m->id).notes.push_back(name + " left out: " + e.what());
            }
        }
    }
    // The Lua API, before every mod's script.
    if (std::any_of(loaded.begin(), loaded.end(), [&](const Mod* m) { return !by_id[m->id]->manifest->scripts.empty(); })) {
        if (!merged.count(kUiResources))
            if (auto t = base_text(kUiResources)) merged[kUiResources] = Merged{kUiResources, *t, {}};
        if (merged.count(kUiResources)) {
            merged[kUiResources].text = with_sfmod(merged[kUiResources].text);
            merged[lower(mod::kSfmodFile)] = Merged{mod::kSfmodFile, mod::kSfmodLua, {}};
        } else {
            r.notes.push_back("data/ui/uiresources.txt not found: mods' scripts won't load");
        }
    }

    // Two mods changing the same key or block.
    for (const auto& [key, mf] : merged) {
        std::map<std::string, std::vector<std::string>> by_addr;
        std::vector<std::string> addr_order;
        for (std::size_t i = 0; i < mf.touched.size(); ++i) {
            for (std::size_t j = i + 1; j < mf.touched.size(); ++j) {
                const auto& [a, ma] = mf.touched[i];
                const auto& [b, mb] = mf.touched[j];
                if (ma == mb || !mod::overlaps(a, b)) continue;
                const std::string& at = a.size() <= b.size() ? a : b;
                if (!by_addr.count(at)) addr_order.push_back(at);
                auto& ids = by_addr[at];
                for (const std::string* id : {&ma, &mb})
                    if (std::find(ids.begin(), ids.end(), *id) == ids.end()) ids.push_back(*id);
            }
        }
        for (const auto& at : addr_order) r.conflicts.push_back({mf.rel + ": " + at, by_addr[at]});
    }
    // Lua functions two mods both replace.
    std::map<std::string, std::vector<std::string>> replaces;
    for (const Mod* m : loaded)
        for (const auto& fn : by_id[m->id]->manifest->lua_replaces) replaces[fn].push_back(m->id);
    for (const auto& [fn, ids] : replaces)
        if (ids.size() > 1) r.conflicts.push_back({"lua " + fn, ids});

    // Write it all out.
    remove_quietly(merged_dir);
    for (const auto& [key, mf] : merged) {
        const fs::path out = merged_dir / upath(mf.rel);
        fs::create_directories(out.parent_path(), ec);
        if (ec) throw std::runtime_error(io_error(out.parent_path(), ec));
        write_or_throw(out, mf.text);
    }
    if (!merged.empty()) r.layers.push_back(std::string(kModsDir) + "/" + kMergedDir);
    for (auto it = loaded.rbegin(); it != loaded.rend(); ++it) {
        const fs::path files = root[(*it)->id] / mod::kFilesDir;
        if (fs::is_directory(files, ec)) r.layers.push_back(rel_slash(files, profile_dir));
    }
    std::string fp_text;
    for (const Mod* m : loaded) {
        fp_text += m->id + "@" + m->version + "\n";
        for (const auto& n : by_id[m->id]->manifest->needs)
            if (std::find(r.core_patches.begin(), r.core_patches.end(), n) == r.core_patches.end()) r.core_patches.push_back(n);
    }
    r.fingerprint = loaded.empty() ? "" : crypto::sha256_hex(fp_text.data(), fp_text.size()).substr(0, 12);

    for (const auto& m : r.mods) {
        if (!m.loaded && m.reason != "turned off") r.notes.push_back("Mod " + m.id + " not loaded: " + m.reason);
        for (const auto& n : m.notes) r.notes.push_back("Mod " + m.id + ": " + n);
    }
    for (const auto& c : r.conflicts) {
        std::string ids;
        for (std::size_t i = 0; i < c.mods.size(); ++i) ids += (i ? ", " : "") + c.mods[i];
        r.notes.push_back("Mods " + ids + " all change " + c.what + "; " + c.mods.back() + " wins (loaded last).");
    }

    if (loaded.empty()) {
        fs::remove(layers_file, ec);
    } else {
        std::string layers = "# STAR FORGE mod layers, written by the launcher before each start\n";
        layers += "fingerprint " + r.fingerprint + "\n";
        for (const Mod* m : loaded) layers += "mod " + m->id + " " + (m->version.empty() ? "-" : m->version) + "\n";
        for (const auto& c : r.core_patches) layers += "core " + c + "\n";
        for (const auto& l : r.layers) layers += "layer " + l + "\n";
        write_or_throw(layers_file, layers);
    }
    write_or_throw(report_file, to_json(r).dump(2) + "\n");
    return r;
}

std::optional<Result> last(const fs::path& profile_dir) {
    std::error_code ec;
    auto text = read_text(profile_dir / kModsDir / kReportFile, &ec);
    if (!text) return std::nullopt;
    json j = json::parse(*text, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    Result r;
    r.fingerprint = j.value("fingerprint", "");
    if (auto it = j.find("mods"); it != j.end() && it->is_array()) {
        for (const auto& e : *it) {
            Mod m;
            m.id = e.value("id", "");
            m.name = e.value("name", "");
            m.version = e.value("version", "");
            m.source = upath(e.value("source", ""));
            m.on = e.value("on", false);
            m.loaded = e.value("loaded", false);
            m.reason = e.value("reason", "");
            m.notes = strs(e, "notes");
            r.mods.push_back(std::move(m));
        }
    }
    if (auto it = j.find("conflicts"); it != j.end() && it->is_array())
        for (const auto& e : *it) r.conflicts.push_back({e.value("what", ""), strs(e, "mods")});
    r.core_patches = strs(j, "core_patches");
    r.layers = strs(j, "layers");
    r.notes = strs(j, "notes");
    return r;
}

void set_on(const fs::path& profile_dir, const std::string& id, bool on) {
    pack::Local local = profiles::read_local(profile_dir);
    auto& off = local.mods.off;
    off.erase(std::remove(off.begin(), off.end(), id), off.end());
    if (!on) off.push_back(id);
    profiles::write_local(profile_dir, local);
}

void set_order(const fs::path& profile_dir, const std::vector<std::string>& order) {
    pack::Local local = profiles::read_local(profile_dir);
    local.mods.order = order;
    profiles::write_local(profile_dir, local);
}

}  // namespace sf::launch::mod_loader

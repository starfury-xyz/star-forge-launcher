#include "patch/patchset.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <tuple>

#include <toml.hpp>

#include "patch/sha1.hpp"

namespace sf::patch {
namespace {

std::string quoted(const std::string& s) { return "\"" + s + "\""; }  // like Rust's {:?} for these names

bool eq_ignore_ascii_case(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// --- reading the TOML ----------------------------------------------------------------------------
// Typed access with errors that say where. Like the Rust (serde deny_unknown_fields), a key the
// format doesn't have is an error: a typo such as `optinal = true` must not be ignored.

void only_keys(const toml::table& t, std::initializer_list<const char*> allowed, const std::string& where) {
    for (auto&& [key, value] : t) {
        bool known = false;
        for (const char* a : allowed) known |= key.str() == a;
        if (!known) throw Error(where + ": unknown field " + quoted(std::string(key.str())));
    }
}

const toml::node* field(const toml::table& t, const char* key, const std::string& where, bool required) {
    const toml::node* n = t.get(key);
    if (!n && required) throw Error(where + ": missing field " + quoted(key));
    return n;
}

int64_t int_field(const toml::table& t, const char* key, const std::string& where, std::optional<int64_t> fallback) {
    const toml::node* n = field(t, key, where, !fallback);
    if (!n) return *fallback;
    if (auto v = n->as_integer()) return v->get();
    throw Error(where + ": " + key + ": expected an integer");
}

std::string str_field(const toml::table& t, const char* key, const std::string& where, std::optional<std::string> fallback) {
    const toml::node* n = field(t, key, where, !fallback);
    if (!n) return *fallback;
    if (auto v = n->as_string()) return v->get();
    throw Error(where + ": " + key + ": expected a string");
}

bool bool_field(const toml::table& t, const char* key, const std::string& where, bool fallback) {
    const toml::node* n = t.get(key);
    if (!n) return fallback;
    if (auto v = n->as_boolean()) return v->get();
    throw Error(where + ": " + key + ": expected true or false");
}

// `sha1 = "..."` or `sha1 = ["...", "..."]`.
std::vector<std::string> sha1_field(const toml::table& t, const std::string& name) {
    const toml::node* n = field(t, "sha1", name, true);
    if (auto s = n->as_string()) return {s->get()};
    std::vector<std::string> out;
    if (auto a = n->as_array()) {
        for (const toml::node& item : *a) {
            auto s = item.as_string();
            if (!s) throw Error(name + ": sha1 must be strings");
            out.push_back(s->get());
        }
        return out;
    }
    throw Error(name + ": sha1 must be a string or a list of strings");
}

bool is_hex40(const std::string& s) {
    return s.size() == 40 && std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}

std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

Write parse_write(const toml::table& w, const std::string& at) {
    only_keys(w, {"va", "orig", "bytes", "midhook", "comment"}, at);
    const int64_t va = int_field(w, "va", at, std::nullopt);
    const std::string here = at + " at " + hex_va(static_cast<uint64_t>(va));
    Write out;
    auto orig = parse_hex(str_field(w, "orig", here, std::nullopt));
    if (!orig) throw Error(here + ": orig is not valid hex");
    if (va <= 0 || orig->empty()) throw Error(here + ": a write needs va and orig");
    out.va = static_cast<uint64_t>(va);
    out.orig = *orig;
    out.comment = str_field(w, "comment", here, "");
    const bool has_bytes = w.contains("bytes"), has_midhook = w.contains("midhook");
    if (has_bytes && !has_midhook) {
        auto bytes = parse_hex(str_field(w, "bytes", here, std::nullopt));
        if (!bytes) throw Error(here + ": bytes is not valid hex");
        if (bytes->size() != out.orig.size()) throw Error(here + ": orig and bytes differ in length");
        out.bytes = *bytes;
    } else if (has_midhook && !has_bytes) {
        out.midhook = str_field(w, "midhook", here, std::nullopt);
        if (out.midhook.empty() || out.orig.size() < 5)
            throw Error(here + ": a midhook needs a name and at least 5 orig bytes");
    } else {
        throw Error(here + ": exactly one of bytes / midhook is required");
    }
    return out;
}

}  // namespace

// --- Patchset --------------------------------------------------------------------------------------

Patchset Patchset::parse(const std::string& text, const std::string& name) {
    toml::table root;
    try {
        root = toml::parse(text, name);
    } catch (const toml::parse_error& e) {
        throw Error(name + ": " + std::string(e.description()) + " (line " + std::to_string(e.source().begin.line) + ")");
    }
    only_keys(root, {"format", "exe", "sha1", "build", "image_base", "patch"}, name);

    const int64_t format = int_field(root, "format", name, std::nullopt);
    if (format != kFormat)
        throw Error(name + ": patchset format " + std::to_string(format) + " is not supported (this launcher reads " +
                    std::to_string(kFormat) + ")");
    Patchset set;
    set.exe = str_field(root, "exe", name, std::nullopt);
    if (!valid_module(set.exe)) throw Error(name + ": exe must be a file name, got " + quoted(set.exe));
    set.sha1 = sha1_field(root, name);
    if (set.sha1.empty()) throw Error(name + ": sha1 needs at least one hash");
    for (auto& s : set.sha1) {
        if (!is_hex40(s)) throw Error(name + ": sha1 must be 40 hex digits");
        s = lower(s);
    }
    set.build = str_field(root, "build", name, "");
    set.image_base = static_cast<uint64_t>(int_field(root, "image_base", name, 0));

    if (const toml::node* list = root.get("patch")) {
        const toml::array* patches = list->as_array();
        if (!patches) throw Error(name + ": patch must be a list of [[patch]] tables");
        for (const toml::node& node : *patches) {
            const toml::table* t = node.as_table();
            if (!t) throw Error(name + ": patch must be a list of [[patch]] tables");
            Patch p;
            p.id = str_field(*t, "id", name + ": patch", std::nullopt);
            const std::string at = name + ": patch " + quoted(p.id);
            only_keys(*t, {"id", "title", "description", "default", "optional", "write"}, at);
            if (p.id.empty()) throw Error(name + ": patch without id");
            for (const Patch& q : set.patches)
                if (q.id == p.id) throw Error(at + ": duplicate id");
            p.title = str_field(*t, "title", at, "");
            p.description = str_field(*t, "description", at, "");
            p.default_on = bool_field(*t, "default", at, true);
            p.optional = bool_field(*t, "optional", at, false);
            if (const toml::node* writes = t->get("write")) {
                const toml::array* a = writes->as_array();
                if (!a) throw Error(at + ": write must be a list of [[patch.write]] tables");
                for (const toml::node& wn : *a) {
                    const toml::table* w = wn.as_table();
                    if (!w) throw Error(at + ": write must be a list of [[patch.write]] tables");
                    p.writes.push_back(parse_write(*w, at));
                }
            }
            if (p.writes.empty()) throw Error(at + ": no writes");
            set.patches.push_back(std::move(p));
        }
    }
    return set;
}

bool Patchset::accepts(const std::string& sha) const {
    return std::find(sha1.begin(), sha1.end(), sha) != sha1.end();
}

std::string Patchset::expected_sha1() const {
    std::string out;
    for (const auto& s : sha1) out += (out.empty() ? "" : " or ") + s;
    return out;
}

bool Patchset::targets(const std::string& module) const { return eq_ignore_ascii_case(exe, module); }

Enabled Patchset::enabled(const Choices* choices) const {
    Enabled r;
    if (choices) {
        for (const auto& choice : *choices) {
            const std::string& id = choice.first;  // (a lambda can't capture a structured binding in C++17)
            bool known = std::any_of(patches.begin(), patches.end(), [&](const Patch& p) { return p.id == id; });
            if (!known) r.warnings.push_back(exe + ": no patch called " + quoted(id));
        }
    }
    for (const Patch& p : patches) {
        bool on = p.default_on;
        if (choices)
            if (auto it = choices->find(p.id); it != choices->end()) on = it->second;
        if (on) r.on.push_back(&p);
    }
    return r;
}

std::vector<std::string> overlaps(const std::vector<const Patch*>& patches) {
    // (start, end, patch id) of every write, sorted, then each range against the ones starting inside it.
    std::vector<std::tuple<uint64_t, uint64_t, std::string>> ranges;
    for (const Patch* p : patches)
        for (const Write& w : p->writes) ranges.emplace_back(w.va, w.va + w.orig.size(), p->id);
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::string> out;
    for (size_t i = 0; i < ranges.size(); ++i) {
        const auto& [a_start, a_end, a_id] = ranges[i];
        for (size_t j = i + 1; j < ranges.size() && std::get<0>(ranges[j]) < a_end; ++j) {
            const auto& [b_start, b_end, b_id] = ranges[j];
            out.push_back(quoted(a_id) + " and " + quoted(b_id) + " both write " + hex_va(b_start) + ".." +
                          hex_va(std::min(a_end, b_end)));
        }
    }
    return out;
}

bool overlap(const Patch& a, const Patch& b) {
    for (const Write& x : a.writes)
        for (const Write& y : b.writes)
            if (x.va < y.va + y.orig.size() && y.va < x.va + x.orig.size()) return true;
    return false;
}

// --- checking and applying --------------------------------------------------------------------------

State check(Target& t, const Patch& p) {
    bool all_orig = true, all_new = true;
    for (const Write& w : p.writes) {
        Bytes cur(w.orig.size());
        if (!t.read(w.va, cur.data(), cur.size()))
            return {State::Mismatch, "cannot read " + hex_va(w.va) + " (+" + std::to_string(cur.size()) + ")"};
        const bool is_orig = cur == w.orig;
        const bool is_new = w.midhook.empty() ? cur == w.bytes : cur[0] == 0xe9;  // a midhook starts with jmp
        if (!is_orig && !is_new)
            return {State::Mismatch, "bytes at " + hex_va(w.va) + " are " + to_hex(cur) + ", expected " + to_hex(w.orig)};
        all_orig &= is_orig;
        all_new &= is_new;
    }
    if (all_orig) return {State::Pristine, ""};
    if (all_new) return {State::Applied, ""};
    return {State::Mismatch, "partly applied"};
}

std::string apply(Target& t, const Patch& p, const MidhookInstaller& midhook) {
    for (const Write& w : p.writes) {
        if (!w.midhook.empty()) {
            if (!midhook) return "midhook " + quoted(w.midhook) + " is not supported by this patch method";
            if (std::string err = midhook(w); !err.empty()) return "midhook " + quoted(w.midhook) + ": " + err;
            uint8_t jmp = 0;
            if (!t.read(w.va, &jmp, 1) || jmp != 0xe9) return "midhook " + quoted(w.midhook) + " left no jmp at " + hex_va(w.va);
            continue;
        }
        if (!t.write(w.va, w.bytes.data(), w.bytes.size())) return "write failed at " + hex_va(w.va);
        Bytes back(w.bytes.size());
        if (!t.read(w.va, back.data(), back.size()) || back != w.bytes) return "readback mismatch at " + hex_va(w.va);
    }
    return "";
}

// --- FileImage ----------------------------------------------------------------------------------------

FileImage FileImage::parse(Bytes data) {
    const Error bad("not a valid PE file");
    auto u16_at = [&](size_t o) -> uint64_t {
        if (o + 2 > data.size()) throw bad;
        return uint64_t(data[o]) | uint64_t(data[o + 1]) << 8;
    };
    auto u32_at = [&](size_t o) -> uint64_t {
        if (o + 4 > data.size()) throw bad;
        return u16_at(o) | u16_at(o + 2) << 16;
    };
    auto u64_at = [&](size_t o) -> uint64_t { return u32_at(o) | u32_at(o + 4) << 32; };

    if (data.size() < 2 || data[0] != 'M' || data[1] != 'Z') throw bad;
    const size_t nt = static_cast<size_t>(u32_at(0x3c));
    if (nt + 4 > data.size() || std::memcmp(&data[nt], "PE\0\0", 4) != 0) throw bad;
    const size_t section_count = static_cast<size_t>(u16_at(nt + 6));
    const size_t opt_size = static_cast<size_t>(u16_at(nt + 20));
    const size_t opt = nt + 24;
    FileImage img;
    switch (u16_at(opt)) {
        case 0x10b: img.image_base = u32_at(opt + 28); break;  // PE32
        case 0x20b: img.image_base = u64_at(opt + 24); break;  // PE32+ (64-bit)
        default: throw bad;
    }
    const size_t table = opt + opt_size;
    for (size_t i = 0; i < section_count; ++i) {
        const size_t s = table + i * 40;
        const uint64_t vsize = u32_at(s + 8), rva = u32_at(s + 12), raw_size = u32_at(s + 16), raw = u32_at(s + 20);
        img.sections_.push_back({rva, std::max(vsize, raw_size), raw, raw_size});
    }
    img.data = std::move(data);
    return img;
}

std::optional<size_t> FileImage::offset(uint64_t va, size_t n) const {
    if (va < image_base) return std::nullopt;
    const uint64_t rva = va - image_base;
    for (const Section& s : sections_) {
        if (rva >= s.rva && rva < s.rva + s.virtual_size) {
            const uint64_t into = rva - s.rva;
            if (into + n > s.raw_size) return std::nullopt;  // not all backed by the file
            const uint64_t o = s.file_offset + into;
            if (o + n > data.size()) return std::nullopt;
            return static_cast<size_t>(o);
        }
    }
    return std::nullopt;
}

bool FileImage::read(uint64_t va, uint8_t* out, size_t n) {
    auto o = offset(va, n);
    if (!o) return false;
    std::memcpy(out, &data[*o], n);
    return true;
}

bool FileImage::write(uint64_t va, const uint8_t* in, size_t n) {
    auto o = offset(va, n);
    if (!o) return false;
    std::memcpy(&data[*o], in, n);
    return true;
}

Report verify_file(const Patchset& set, Bytes data, const Choices* choices) {
    Report r;
    const std::string sha = sha1_hex(data.data(), data.size());
    if (!set.accepts(sha)) {
        r.errors.push_back(set.exe + " is a different build than the patches were made for (sha1 " + sha + ", expected " +
                           set.expected_sha1() + ")");
        return r;
    }
    FileImage image;
    try {
        image = FileImage::parse(std::move(data));
    } catch (const Error& e) {
        r.errors.push_back(set.exe + ": " + e.what());
        return r;
    }
    image.image_base = set.image_base;  // addresses are relative to the set's base; the file is the same build
    Enabled en = set.enabled(choices);
    r.warnings = en.warnings;
    for (auto& o : overlaps(en.on)) r.errors.push_back(o);
    for (const Patch* p : en.on) {
        State s = check(image, *p);
        if (s.kind == State::Pristine) r.ok.push_back(p->id);
        else if (s.kind == State::Applied) r.errors.push_back(set.exe + ": " + p->id + " is already patched on disk");
        else if (p->optional) r.skipped.push_back(p->id + ": " + s.why);
        else r.errors.push_back(set.exe + ": " + p->id + ": " + s.why);
    }
    return r;
}

// --- helpers ---------------------------------------------------------------------------------------------

std::optional<Bytes> parse_hex(const std::string& text) {
    std::string digits;
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '_') digits += c;
    if (digits.size() % 2) return std::nullopt;
    Bytes out;
    for (size_t i = 0; i < digits.size(); i += 2) {
        int v = 0;
        for (char c : {digits[i], digits[i + 1]}) {
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return std::nullopt;
        }
        out.push_back(static_cast<uint8_t>(v));
    }
    return out;
}

std::string to_hex(const Bytes& bytes) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        s += d[b >> 4];
        s += d[b & 15];
    }
    return s;
}

std::string hex_va(uint64_t va) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(va));
    return buf;
}

bool valid_module(const std::string& name) {
    return !name.empty() && name.find_first_of("/\\:") == std::string::npos && name != "." && name != "..";
}

}  // namespace sf::patch

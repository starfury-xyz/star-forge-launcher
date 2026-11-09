#pragma once

// Strict JSON reading with serde-like error messages, shared by everything that reads the
// launcher's JSON files (pack.json, local.json, manifest.json, file lists, ...): an unknown field,
// a missing one or a wrong type is an error worded the way serde words it in the Rust launcher.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pack/pack.hpp"

namespace sf::pack::serde {

using nlohmann::json;
using ojson = nlohmann::ordered_json;  // keeps keys in insertion order, for writing

// Rust's {:?} for a string: in quotes, with ", \ and control characters escaped.
inline std::string debug_str(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7f) {
                char buf[16];
                std::snprintf(buf, sizeof buf, "\\u{%x}", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

inline bool is_hex(const std::string& s, size_t len) {
    return s.size() == len && std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
           });
}

inline std::string kind(const json& v) {
    if (v.is_null()) return "null";
    if (v.is_boolean()) return std::string("boolean `") + (v.get<bool>() ? "true" : "false") + "`";
    if (v.is_number_integer()) return "integer `" + v.dump() + "`";
    if (v.is_number_float()) return "floating point `" + v.dump() + "`";
    if (v.is_string()) return "string " + debug_str(v.get<std::string>());
    if (v.is_array()) return "sequence";
    return "map";
}

[[noreturn]] inline void invalid(const json& v, const std::string& expected) {
    throw Error("invalid type: " + kind(v) + ", expected " + expected);
}

// One JSON object being read as a struct: unknown keys are refused up front.
class Obj {
public:
    // `deny`: refuse unknown keys (serde's deny_unknown_fields); otherwise they're ignored.
    Obj(const json& j, const char* type, std::vector<const char*> fields, bool deny = true) : j_(j) {
        if (!j.is_object()) invalid(j, std::string("struct ") + type);
        for (auto it = j.begin(); deny && it != j.end(); ++it) {
            if (std::find_if(fields.begin(), fields.end(), [&](const char* f) { return it.key() == f; }) != fields.end())
                continue;
            std::string expected;
            for (size_t i = 0; i < fields.size(); ++i)
                expected += std::string(i == 0 ? "" : (i + 1 == fields.size() ? " or " : ", ")) + "`" + fields[i] + "`";
            throw Error("unknown field `" + it.key() + "`, expected " + (fields.size() > 1 ? "one of " : "") + expected);
        }
    }
    // The value, or nullptr when the key is missing.
    const json* get(const char* key) const {
        auto it = j_.find(key);
        return it == j_.end() ? nullptr : &*it;
    }
    // Optional<T> fields: missing or null both mean "not set".
    const json* opt(const char* key) const {
        const json* v = get(key);
        return v && !v->is_null() ? v : nullptr;
    }
    const json& req(const char* key) const {
        const json* v = get(key);
        if (!v) throw Error(std::string("missing field `") + key + "`");
        return *v;
    }

private:
    const json& j_;
};

inline std::string str(const json& v) {
    if (!v.is_string()) invalid(v, "a string");
    return v.get<std::string>();
}
inline bool boolean(const json& v) {
    if (!v.is_boolean()) invalid(v, "a boolean");
    return v.get<bool>();
}
inline uint32_t u32(const json& v) {
    if (!v.is_number_unsigned() || v.get<uint64_t>() > 0xffffffffull) {
        if (v.is_number_integer()) throw Error("invalid value: integer `" + v.dump() + "`, expected u32");
        invalid(v, "u32");
    }
    return static_cast<uint32_t>(v.get<uint64_t>());
}
inline uint64_t u64(const json& v) {
    if (!v.is_number_unsigned()) {
        if (v.is_number_integer()) throw Error("invalid value: integer `" + v.dump() + "`, expected u64");
        invalid(v, "u64");
    }
    return v.get<uint64_t>();
}
inline std::vector<std::string> strings(const json& v) {
    if (!v.is_array()) invalid(v, "a sequence");
    std::vector<std::string> out;
    for (const auto& e : v) out.push_back(str(e));
    return out;
}
// Parses `text`, then reads it with `read`; any error gets the file name in front.
template <class T, class F>
T parse(const std::string& text, const char* file, F read) {
    try {
        return read(json::parse(text));
    } catch (const json::parse_error& e) {
        // nlohmann's "[json.exception.parse_error.101] parse error at line 1, column 2: ..."
        std::string m = e.what();
        size_t at = m.find("] ");
        throw Error(std::string(file) + ": " + (at == std::string::npos ? m : m.substr(at + 2)));
    } catch (const Error& e) {
        throw Error(std::string(file) + ": " + e.what());
    }
}

}  // namespace sf::pack::serde

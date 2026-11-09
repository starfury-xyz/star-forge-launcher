// Unit test for sf::patch (patchsets, checking/applying, FileImage, core patches). The first
// four groups port sc-patch's Rust tests case for case.
//
// Usage: patch_test [path\to\game.exe]. With a game.exe, also checks the core patches against
// that real build, the way the launcher would before starting the game.

#include "patch/core.hpp"
#include "patch/patchset.hpp"
#include "patch/sha1.hpp"

#include <algorithm>
#include <cstring>

#include "check.hpp"

using namespace sf::patch;
using test::check;
using test::contains;

namespace {

// The Rust tests' sample set: "one" (on by default) and "two" (off, optional), overlapping at
// 0x140001003.
const std::string SET = R"(
format = 1
exe = "game.exe"
sha1 = "0000000000000000000000000000000000000000"
image_base = 0x140000000

[[patch]]
id = "one"
title = "Nop a check"
[[patch.write]]
va = 0x140001002
orig = "74 05"
bytes = "90 90"

[[patch]]
id = "two"
default = false
optional = true
[[patch.write]]
va = 0x140001003
orig = "05aa"
bytes = "0000"
)";

std::string replaced(std::string s, const std::string& from, const std::string& to) {
    size_t at = s.find(from);
    if (at == std::string::npos) {
        std::printf("test bug: '%s' not in SET\n", from.c_str());
        return s;
    }
    return s.replace(at, from.size(), to);
}

// The parse error for SET with `from` replaced by `to`, or "" if it parsed.
std::string parse_error(const std::string& from, const std::string& to) {
    try {
        Patchset::parse(replaced(SET, from, to), "t");
        return "";
    } catch (const Error& e) {
        return e.what();
    }
}

// A minimal PE32+ with one section: rva 0x1000, file offset 0x200, 0x10 bytes.
Bytes pe() {
    Bytes d(0x210, 0);
    d[0] = 'M';
    d[1] = 'Z';
    d[0x3c] = 0x40;
    std::memcpy(&d[0x40], "PE\0\0", 4);
    d[0x46] = 1;     // sections
    d[0x54] = 0xf0;  // optional header size
    d[0x58] = 0x0b;  // 0x20b: PE32+
    d[0x59] = 0x02;
    const uint64_t base = 0x140000000;
    std::memcpy(&d[0x70], &base, 8);
    const size_t s = 0x58 + 0xf0;
    const uint32_t vsize = 0x10, rva = 0x1000, raw_size = 0x10, raw = 0x200;
    std::memcpy(&d[s + 8], &vsize, 4);
    std::memcpy(&d[s + 12], &rva, 4);
    std::memcpy(&d[s + 16], &raw_size, 4);
    std::memcpy(&d[s + 20], &raw, 4);
    d[0x202] = 0x74;
    d[0x203] = 0x05;
    d[0x204] = 0xaa;
    return d;
}

std::string sha(const Bytes& b) { return sha1_hex(b.data(), b.size()); }

}  // namespace

int main(int argc, char** argv) {
    test::exit_on_crash();

    // SHA-1 itself, against the standard test vectors.
    check(sha1_hex("abc", 3) == "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1(\"abc\")");
    check(sha1_hex("", 0) == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "sha1(\"\")");
    std::string million(1000000, 'a');
    check(sha1_hex(million.data(), million.size()) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f", "sha1(a x 1e6)");

    // Rust: parses_and_selects
    {
        Patchset set = Patchset::parse(SET, "t");
        check(set.image_base == 0x140000000, "image_base");
        check(set.targets("GAME.EXE"), "targets() ignores case");
        check(set.enabled(nullptr).on.size() == 1, "default: only \"one\" is on");
        Choices choice{{"two", true}, {"nope", true}};
        Enabled en = set.enabled(&choice);
        check(en.on.size() == 2, "choices turn \"two\" on");
        check(en.warnings.size() == 1 && contains(en.warnings[0], "no patch called \"nope\""), "unknown choice warns");
        auto o = overlaps(en.on);
        check(o.size() == 1 && o[0] == "\"one\" and \"two\" both write 0x140001003..0x140001004", "overlap found");
    }

    // Rust: rejects_bad_sets (+ a few more for the strict format)
    check(contains(parse_error("format = 1", "format = 2"), "format 2 is not supported"), "rejects format 2");
    check(contains(parse_error("\"game.exe\"", "\"..\\\\game.exe\""), "exe must be a file name"), "rejects a path as exe");
    check(contains(parse_error("bytes = \"90 90\"", "bytes = \"90\""), "differ in length"), "rejects bytes of the wrong length");
    check(contains(parse_error("bytes = \"90 90\"", "midhook = \"x\""), "at least 5 orig bytes"), "rejects a short midhook");
    check(contains(parse_error("id = \"two\"", "id = \"one\""), "duplicate id"), "rejects duplicate ids");
    check(contains(parse_error("optional = true", "optinal = true"), "unknown field \"optinal\""), "rejects a misspelt key");
    check(contains(parse_error("bytes = \"90 90\"", "bytes = \"9g 90\""), "not valid hex"), "rejects bad hex");
    check(contains(parse_error("bytes = \"90 90\"", ""), "exactly one of bytes / midhook"), "rejects a write without bytes");
    check(contains(parse_error("format = 1", ""), "missing field \"format\""), "rejects a missing format");
    check(contains(parse_error("format = 1", "format = 1 ="), "t: "), "a TOML syntax error names the file");

    // Rust: checks_and_applies_a_file
    {
        const Bytes data = pe();
        Patchset set = Patchset::parse(SET, "t");
        set.sha1 = {sha(data)};
        Report r = verify_file(set, data, nullptr);
        check(r.ok.size() == 1 && r.ok[0] == "one" && r.errors.empty(), "verify_file: \"one\" would apply");

        FileImage image = FileImage::parse(data);
        const Patch& p = set.patches[0];
        check(::sf::patch::check(image, p).kind == State::Pristine, "check: pristine before");
        check(apply(image, p).empty(), "apply succeeds");
        check(::sf::patch::check(image, p).kind == State::Applied, "check: applied after");
        check(image.data[0x202] == 0x90 && image.data[0x203] == 0x90, "the bytes changed in the file");
        uint8_t b;
        check(!image.read(0x140001010, &b, 1), "can't read past the section");

        // An optional patch that doesn't match is skipped; a wrong build is an error.
        Bytes tweaked = data;
        tweaked[0x204] = 0x11;
        set.sha1 = {sha(tweaked)};
        Choices two{{"two", true}};
        Report skip = verify_file(set, tweaked, &two);
        check(skip.skipped.size() == 1, "mismatching optional patch: skipped");
        // ... and the reason says exactly which bytes differ (what agent.log shows a pack author).
        check(skip.skipped.size() == 1 && skip.skipped[0] == "two: bytes at 0x140001003 are 0511, expected 05aa",
              "the skip reason names the address and both byte strings");
        Report wrong = verify_file(set, data, nullptr);
        check(!wrong.errors.empty() && contains(wrong.errors[0], "different build"), "wrong build: error");

        // A set can list several builds; any of them is accepted.
        set.sha1 = {std::string(40, '0'), sha(data)};
        check(verify_file(set, data, nullptr).errors.empty(), "any listed build is accepted");
    }

    // Rust: sha1_is_one_or_a_list
    {
        Patchset one = Patchset::parse(SET, "t");
        check(one.sha1.size() == 1 && one.sha1[0] == std::string(40, '0'), "sha1 = \"...\"");
        const std::string old = "sha1 = \"0000000000000000000000000000000000000000\"";
        Patchset many = Patchset::parse(
            replaced(SET, old, "sha1 = [\"0000000000000000000000000000000000000000\", \"ABCDEF0123456789abcdef0123456789abcdef01\"]"), "t");
        check(many.accepts("abcdef0123456789abcdef0123456789abcdef01"), "sha1 list: second (lower-cased) accepted");
        check(many.accepts(std::string(40, '0')), "sha1 list: first accepted");
        check(!many.accepts(std::string(40, '1')), "sha1 list: others rejected");
        check(!parse_error(old, "sha1 = []").empty(), "sha1 = [] rejected");
        check(!parse_error(old, "sha1 = [\"0000\"]").empty(), "short sha1 rejected");
    }

    // Rust (core.rs): bundled_sets_parse_and_filter
    {
        auto ids = core::ids();
        check(std::find(ids.begin(), ids.end(), "loose-before-packed") != ids.end(), "core: loose-before-packed exists");
        auto sets = core::sets({"loose-before-packed"});
        check(sets.size() == 1 && sets[0].patches.size() == 1, "core: one set, one patch");
        std::vector<const Patch*> on;
        for (const auto& p : sets[0].patches) on.push_back(&p);
        check(overlaps(on).empty(), "core: no overlaps");
        check(core::sets({}).empty(), "core: none requested, none returned");
        try {
            core::sets({"nope"});
            check(false, "core: unknown id rejected");
        } catch (const Error& e) {
            check(contains(e.what(), "\"nope\""), "core: unknown id rejected");
        }
        check(sets[0].accepts("165397c745536e6c610e3d831e17ca128f5cbc2b"), "core: accepts game 1.99.0.167301 (0.2.6)");
    }

    // The core patches against the real game.exe, if given.
    const std::string game = argc > 1 ? argv[1] : "";
    if (game.empty()) {
        std::printf("SKIP: no game.exe given (configure with -DSF_GAME_EXE=...)\n");
    } else {
        Bytes data;
        {
            std::ifstream f(game, std::ios::binary);
            data.assign(std::istreambuf_iterator<char>(f), {});
        }
        check(!data.empty(), "read game.exe");
        auto sets = core::sets({"loose-before-packed"});
        Report r = verify_file(sets[0], data, nullptr);
        for (const auto& e : r.errors) std::printf("  error: %s\n", e.c_str());
        check(r.errors.empty() && r.ok.size() == 1, "core patches verify against the installed game.exe");
    }

    return test::failures ? 1 : 0;
}

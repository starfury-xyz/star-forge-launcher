// Tests sf::launch::remote (file lists: SCMOD1 and STAR FORGE JSON, fetching them, syncing a
// profile's files/ from them) against a local HTTP server. Groups marked "Rust:" port remote.rs's
// tests.

#include <algorithm>

#include "crypto/sha256.hpp"
#include "launch/env.hpp"
#include "launch/remote.hpp"

#include "check.hpp"
#include "http_server.hpp"

namespace fs = std::filesystem;
using namespace sf::launch::remote;
using sf::launch::upath;
using test::check;
using test::contains;

namespace {

const std::string HASH = "2c26b46b68ffc68ff99b453c1d30413413422d706483bfa0f98a5e886266e7ae";

std::string sha(const std::string& s) { return sf::crypto::sha256_hex(s.data(), s.size()); }

template <class F>
std::string error_of(F f) {
    try {
        f();
        return "";
    } catch (const std::exception& e) {
        return e.what();
    }
}

std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

// Rust: clean_path_rejects_escapes (through the SCMOD1 reader, which uses it).
void paths() {
    auto scmod1 = [](const std::string& path) {
        return error_of([&] { parse_scmod1("SCMOD1\n" + path + "\t" + HASH + "\thttps://a.org/x"); });
    };
    for (const char* bad : {"../x", "/x", "C:\\x", "\\\\srv\\x", "data/address.cfg.", "data/con.lua", "a//b"})
        check(contains(scmod1(bad), "is not a plain path inside the game folder"), std::string("refuses ") + bad + ": " + scmod1(bad));
    check(parse_scmod1("SCMOD1\ndata\\a.lua\t" + HASH + "\thttps://a.org/x")[0].path == "data/a.lua", "backslashes become /");
    check(scmod1("data/ADDRESS.cfg") == "data/ADDRESS.cfg is written by the launcher from server.address; set that instead",
          "address.cfg is the launcher's, in any case");
}

// Rust: reads_scmod1.
void scmod1() {
    const std::string text = "\xEF\xBB\xBFSCMOD1\r\n# comment\r\ndata\\ui\\x.lua\t" + upper(HASH) + "\thttps://a.org/x.lua\r\n\r\n";
    auto entries = parse_scmod1(text);
    check(entries.size() == 1 && entries[0] == Entry{"data/ui/x.lua", HASH, std::nullopt, "https://a.org/x.lua"},
          "SCMOD1: BOM, CRLF, comments, blank lines, upper-case hash");
    check(error_of([] { parse_scmod1("SCMOD2\n"); }) == "not an SCMOD1 list (its first line must be SCMOD1)", "SCMOD2 refused");
    check(error_of([] { parse_scmod1(""); }) == "not an SCMOD1 list (its first line must be SCMOD1)", "empty text refused");
    check(!error_of([] { parse_scmod1("SCMOD1\n../x\t" + HASH + "\thttps://a.org/x"); }).empty(), "an escaping path refused");
    check(error_of([] { parse_scmod1("SCMOD1\n# c\ndata/x " + HASH + " https://a.org/x"); }) ==
              "SCMOD1 line 3: expected path, SHA-256 and URL separated by tabs",
          "spaces instead of tabs: the line number");
    check(!error_of([] { parse_scmod1("SCMOD1\ndata/address.cfg\t" + HASH + "\thttps://a.org/x"); }).empty(), "address.cfg refused");
    check(error_of([] { parse_scmod1("SCMOD1\ndata/x\tabc\thttps://a.org/x"); }) == "data/x: not a SHA-256", "a short hash refused");
    check(parse_scmod1("SCMOD1").empty(), "just the header: no files");
}

// Rust: reads_json_lists.
void json_lists() {
    const std::string list = R"({"version":"v2","files":{"data/gamedata/a b.lua":{"sha256":")" + HASH + R"(","size":3}}})";
    auto entries = parse_json(list, "https://cdn.org/starfury/v2.json?x=1");
    check(entries.size() == 1 && entries[0].url == "https://cdn.org/starfury/data/gamedata/a%20b.lua",
          "files sit next to the list, names percent-encoded: " + (entries.empty() ? "" : entries[0].url));
    check(!entries.empty() && entries[0].size == std::optional<uint64_t>(3), "sizes are read");
    const std::string based = R"({"base_url":"https://b.org/files/","files":{"x":{"sha256":")" + HASH + R"("}}})";
    check(parse_json(based, "https://a.org/l.json")[0].url == "https://b.org/files/x", "base_url");
    const std::string own = R"({"files":{"x":{"sha256":")" + HASH + R"(","url":"https://c.org/y"}}})";
    check(parse_json(own, "https://a.org/l.json")[0].url == "https://c.org/y", "an entry's own url");
    check(error_of([] { parse_json(R"({"files":{"x":{"sha256":"nope"}}})", "https://a.org/l.json"); }) == "x: not a SHA-256",
          "a bad hash refused");
    check(error_of([] { parse_json(R"({"version":1})", "https://a.org/l.json"); }) == "file list: missing field `files`",
          "no files: serde's message");
    check(contains(error_of([] { parse_json("{", "https://a.org/l.json"); }), "file list: "), "broken JSON");
    // Sorted by path, like the Rust's BTreeMap.
    const std::string two = R"({"files":{"b":{"sha256":")" + HASH + R"("},"a":{"sha256":")" + HASH + R"("}}})";
    auto e2 = parse_json(two, "https://a.org/l.json");
    check(e2.size() == 2 && e2[0].path == "a" && e2[1].path == "b", "entries sorted by path");
    check(parse_json(R"({"files":{"x y/ä#.lua":{"sha256":")" + HASH + R"("}}})", "https://a.org/")[0].url ==
              "https://a.org/x%20y/%C3%A4%23.lua",
          "each segment percent-encoded");
}

void build(const fs::path& base) {
    const fs::path dir = test::fresh_dir(base / "build");
    test::write_file(dir / "data/b.lua", "bee");
    test::write_file(dir / "data/a b.lua", "one");
    test::write_file(dir / ".git/x", "hidden");
    const std::string json = build_list(dir, std::nullopt, false);
    check(json == "{\n  \"files\": {\n    \"data/a b.lua\": {\n      \"sha256\": \"" + sha("one") +
                      "\",\n      \"size\": 3\n    },\n    \"data/b.lua\": {\n      \"sha256\": \"" + sha("bee") +
                      "\",\n      \"size\": 3\n    }\n  },\n  \"format\": 1\n}\n",
          "a JSON list, laid out like serde_json's (keys sorted)");
    const std::string with_base = build_list(dir, std::string("https://a.org/f"), false);
    check(with_base.rfind("{\n  \"base_url\": \"https://a.org/f\",\n  \"files\"", 0) == 0, "base_url comes first");
    auto back = parse_json(with_base, "https://x.org/l.json");
    check(back.size() == 2 && back[0].url == "https://a.org/f/data/a%20b.lua" && back[1].sha256 == sha("bee"), "it reads back");
    check(build_list(dir, std::string("https://a.org/f/"), true) == "SCMOD1\ndata/a b.lua\t" + sha("one") +
                                                                       "\thttps://a.org/f/data/a%20b.lua\ndata/b.lua\t" +
                                                                       sha("bee") + "\thttps://a.org/f/data/b.lua\n",
          "an SCMOD1 list");
    check(error_of([&] { build_list(dir, std::nullopt, true); }) ==
              "an SCMOD1 list needs --base-url (where the files will be hosted)",
          "SCMOD1 needs a base url");
    test::write_file(dir / "data/address.cfg", "x");
    check(contains(error_of([&] { build_list(dir, std::nullopt, false); }), "written by the launcher"), "address.cfg can't be listed");
}

std::vector<std::string> g_seen;

// Records what a sync tells its progress callback, the way the Rust test does.
std::optional<std::string> record(const SyncEvent& e) {
    auto opt = [](const std::optional<uint64_t>& v) { return v ? "Some(" + std::to_string(*v) + ")" : std::string("None"); };
    std::string line;
    switch (e.kind) {
    case SyncEvent::Plan:
        line = "plan " + std::to_string(e.files) + " " + opt(e.bytes) + " " + std::to_string(e.kept) + " " + std::to_string(e.remove);
        break;
    case SyncEvent::File: line = "get " + e.path + " " + opt(e.size); break;
    case SyncEvent::Bytes: line = "bytes " + std::to_string(e.done) + " " + opt(e.total); break;
    case SyncEvent::Verified: line = "ok " + e.path + " " + std::to_string(e.done); break;
    case SyncEvent::Removed: line = "del " + e.path; break;
    }
    // A file may arrive in several chunks; keep the last count.
    if (line.rfind("bytes", 0) == 0 && !g_seen.empty() && g_seen.back().rfind("bytes", 0) == 0) g_seen.pop_back();
    g_seen.push_back(line);
    return std::nullopt;
}

std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : " | ") + s;
    return out;
}

// Rust: syncs_only_changes_and_checks_signatures, then more.
void syncs(const fs::path& base) {
    namespace signing = sf::launch::signing;
    auto key = signing::SecretKey::generate();
    test::FileServer files;
    const std::string url = files.base();
    auto list = [&](const std::string& name, const std::string& body, bool is_signed) {
        if (is_signed) files.put("/" + name + ".sig", signing::sign_bytes(key, body));
        files.put("/" + name, body);
    };
    files.put("/a1", "one");
    files.put("/a2", "two");
    files.put("/b", "bee");
    const std::string v1 = "SCMOD1\ndata/a.lua\t" + sha("one") + "\t" + url + "/a1\ndata/b.lua\t" + sha("bee") + "\t" + url + "/b\n";
    list("v1.txt", v1, true);
    list("unsigned.txt", v1, false);
    list("v2.txt", "SCMOD1\ndata/a.lua\t" + sha("two") + "\t" + url + "/a2\n", false);
    list("bad.txt", "SCMOD1\ndata/a.lua\t" + sha("other") + "\t" + url + "/a1\n", true);
    list("half.txt", "SCMOD1\ndata/a.lua\t" + sha("three") + "\t" + url + "/a3\ndata/c.lua\t" + sha("sea") + "\t" + url + "/gone\n", false);
    files.put("/a3", "three");
    auto from = [&](const std::string& name) {
        sf::pack::FilesFrom f;
        f.scmod1 = url + "/" + name;
        return f;
    };
    const fs::path profile = test::fresh_dir(base / "profile");
    const signing::PublicKey pub = key.public_key();

    // Unsigned packs take any list; signed ones need their key's signature.
    auto list_error = [&](const std::string& name, const signing::PublicKey* k) -> std::optional<ListError> {
        try {
            fetch_list(from(name), k);
            return std::nullopt;
        } catch (const ListError& e) {
            return e;
        }
    };
    check(!list_error("unsigned.txt", nullptr), "an unsigned pack takes an unsigned list");
    auto e = list_error("unsigned.txt", &pub);
    check(e && !e->network && contains(e->what(), "the pack is signed but its file list has no signature (127.0.0.1:"),
          "a signed pack refuses an unsigned list: " + std::string(e ? e->what() : ""));
    auto other = signing::SecretKey::generate();
    e = list_error("v1.txt", &other.public_key());
    check(e && !e->network && e->what() == url + "/v1.txt: the signature does not match; it was not signed with the pack's key",
          "another key's list is refused");
    e = list_error("missing.txt", nullptr);
    check(e && e->network, "a missing list is a network problem");

    auto synced = [&](const FileList& l, Synced* out) {
        g_seen.clear();
        std::string err = error_of([&] { *out = sync(profile, l, record); });
        return err.empty() ? joined(g_seen) : "error: " + err;
    };
    Synced done;

    FileList l = fetch_list(from("v1.txt"), &pub);
    check(l.is_signed, "the signed list is signed");
    // SCMOD1 gives no sizes, so the total comes from the server.
    check(synced(l, &done) == "plan 2 None 0 0 | get data/a.lua None | bytes 3 Some(3) | ok data/a.lua 3 | get data/b.lua None | "
                              "bytes 3 Some(3) | ok data/b.lua 3",
          "first sync: " + joined(g_seen));
    check(done == Synced{2, 0}, "two downloaded");
    check(test::read_file(profile / "files/data/a.lua") == "one", "a.lua arrived");
    check(synced(l, &done) == "plan 0 Some(0) 2 0" && done == Synced{}, "nothing changed, nothing fetched");
    check(intact(profile), "intact");

    l = fetch_list(from("v2.txt"), nullptr);
    check(synced(l, &done) == "plan 1 None 0 1 | get data/a.lua None | bytes 3 Some(3) | ok data/a.lua 3 | del data/b.lua",
          "v2: one changed, one dropped: " + joined(g_seen));
    check(done == Synced{1, 1}, "one downloaded, one removed");
    check(test::read_file(profile / "files/data/a.lua") == "two", "a.lua is v2's");
    check(!fs::exists(profile / "files/data/b.lua"), "b.lua is gone");

    // The progress callback can stop a sync mid-file (the window's Cancel).
    l = fetch_list(from("v1.txt"), &pub);
    std::string stopped = error_of([&] {
        sync(profile, l, [](const SyncEvent& ev) -> std::optional<std::string> {
            if (ev.kind == SyncEvent::Bytes) return std::string("stopped");
            return std::nullopt;
        });
    });
    check(stopped == "stopped", "the callback stops a sync: " + stopped);
    check(test::read_file(profile / "files/data/a.lua") == "two", "the old file stays");
    check(!fs::exists(profile / ".sync-staging/data/.a.lua.download"), "no half download left");

    l = fetch_list(from("bad.txt"), &pub);
    std::string bad = error_of([&] { sync(profile, l, quiet); });
    check(bad == "a1 from 127.0.0.1:" + std::to_string(files.server().port()) + " arrived damaged.",
          "a file that doesn't match its hash is refused: " + bad);
    check(test::read_file(profile / "files/data/a.lua") == "two", "and the old one is kept");

    // A sync that fails part-way changes nothing, and keeps what it got.
    FileList half = fetch_list(from("half.txt"), nullptr);
    check(!error_of([&] { sync(profile, half, quiet); }).empty(), "a missing file fails the sync");
    check(test::read_file(profile / "files/data/a.lua") == "two", "no mix of old and new");
    check(intact(profile), "still intact");
    check(fs::is_regular_file(profile / ".sync-staging/data/a.lua"), "the finished download waits");
    const int a3_hits = files.server().hits("/a3");
    files.put("/gone", "sea");
    check(error_of([&] { done = sync(profile, half, quiet); }).empty() && done == Synced{2, 0}, "the retry finishes");
    check(files.server().hits("/a3") == a3_hits, "... without downloading the waiting file again");
    check(test::read_file(profile / "files/data/a.lua") == "three", "a.lua is half's");
    check(!fs::exists(profile / ".sync-staging"), "staging cleared");

    // What was recorded, laid out like the Rust's.
    check(test::read_file(profile / "remote-files.json") == "{\n  \"source\": \"" + url + "/half.txt\",\n  \"files\": {\n    \"data/a.lua\": {\n"
                                                         "      \"sha256\": \"" + sha("three") + "\",\n      \"size\": 5\n    },\n"
                                                         "    \"data/c.lua\": {\n      \"sha256\": \"" + sha("sea") +
                                                         "\",\n      \"size\": 3\n    }\n  }\n}\n",
          "remote-files.json");

    // A file already right on disk is recorded, not downloaded.
    {
        const fs::path p2 = test::fresh_dir(base / "profile2");
        test::write_file(p2 / "files/data/a.lua", "two");
        FileList l2 = fetch_list(from("v2.txt"), nullptr);
        const int before = files.server().hits("/a2");
        check(error_of([&] { done = sync(p2, l2, quiet); }).empty() && done == Synced{0, 0} && files.server().hits("/a2") == before,
              "a file already right on disk isn't downloaded");
        check(intact(p2), "... and is recorded");
        fs::remove(p2 / "files/data/a.lua");
        check(!intact(p2), "a recorded file gone: not intact");
        test::write_file(p2 / ".sync-swapping", "");
        test::write_file(p2 / "files/data/a.lua", "two");
        check(!intact(p2), "a swap that didn't finish: not intact");
    }

    // One sync per profile at a time.
    {
        ProfileLock held = lock_profile(profile);
        check(contains(error_of([&] { sync(profile, half, quiet); }), "already being updated"), "a second sync waits its turn");
    }
    check(error_of([&] { lock_profile(profile); }).empty(), "the lock goes with its holder");

    // Before a launch.
    {
        sf::pack::Pack pack;
        pack.name = "Test";
        check(!launch_sync(profile, pack, nullptr, quiet), "a pack without files_from: nothing to do");
        pack.files_from = from("v2.txt");
        auto note = launch_sync(profile, pack, nullptr, quiet);
        check(note == "Test: updated 2 game file(s) from 127.0.0.1:" + std::to_string(files.server().port()),
              "launch sync note: " + note.value_or("(none)"));
        check(!launch_sync(profile, pack, nullptr, quiet), "nothing new: no note");
        pack.files_from = from("missing.txt");
        note = launch_sync(profile, pack, nullptr, quiet);
        check(note && contains(*note, "Could not check Test's game files (") && contains(*note, "); using the ones from last time."),
              "list unreachable, files intact: start with them");
        pack.files_from = from("unsigned.txt");
        check(contains(error_of([&] { launch_sync(profile, pack, &pub, quiet); }), "Test's game files: the pack is signed but"),
              "a list failing its checks stops the launch");
        forget(profile);
        pack.files_from = from("missing.txt");
        check(contains(error_of([&] { launch_sync(profile, pack, nullptr, quiet); }), "Test's game files: "),
              "unreachable, and nothing from last time: stops");
    }

    // A list may only name https files (or this machine's).
    list("http.txt", "SCMOD1\ndata/a.lua\t" + sha("one") + "\thttp://evil.org/a\n", false);
    e = list_error("http.txt", nullptr);
    check(e && !e->network && std::string(e->what()) == "http://evil.org/a: packs must be downloaded over https", "http files refused");
    list("twice.txt", "SCMOD1\ndata/a.lua\t" + sha("one") + "\t" + url + "/a1\nDATA/A.lua\t" + sha("one") + "\t" + url + "/a1\n", false);
    e = list_error("twice.txt", nullptr);
    check(e && std::string(e->what()) == url + "/twice.txt: DATA/A.lua is listed twice", "a file listed twice (in any case)");
}

}  // namespace

int main() {
    test::exit_on_crash();
    const fs::path base = test::fresh_dir(fs::temp_directory_path() / "sf-remote-test");
    sf::launch::set_env(sf::launch::env::kDataDir, sf::launch::ustr(base / "data"));
    paths();
    scmod1();
    json_lists();
    build(base);
    syncs(base);
    std::printf("%s (%d failed)\n", test::failures ? "FAILED" : "ok", test::failures);
    return test::failures ? 1 : 0;
}

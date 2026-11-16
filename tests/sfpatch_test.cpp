// Unit test for sf::mod (mod/blocks.hpp, mod/sfpatch.hpp): the game's block format and data
// patches.
//
// Optional arguments, for the checks against real files (skipped when missing):
//   1. the game folder: every block-format file under data/ui must parse;
//   2. a copy of coms-retro-sc-ui: diff() of each of its data files against the game's, applied
//      to the game's, must give its file back byte for byte.

#include <filesystem>

#include "check.hpp"
#include "mod/blocks.hpp"
#include "mod/sfpatch.hpp"

namespace fs = std::filesystem;
using test::check;
using test::contains;

namespace {

std::string run(const std::string& base, const std::string& patch) {
    auto p = sf::mod::parse_patch(patch, "t.sfpatch");
    return sf::mod::apply(base, p.files.at(0), p.name);
}

// The error apply() throws, or "" if it doesn't.
std::string run_err(const std::string& base, const std::string& patch) {
    try {
        run(base, patch);
    } catch (const sf::mod::PatchError& e) {
        return e.what();
    }
    return "";
}

std::string parse_err(const std::string& patch) {
    try {
        sf::mod::parse_patch(patch, "t.sfpatch");
    } catch (const sf::mod::PatchError& e) {
        return e.what();
    }
    return "";
}

const std::string kTextures =
    "\xEF\xBB\xBF// textures\r\n"
    "Texture\r\n"
    "{\r\n"
    "\tname\t\t\"Icon_A\"\r\n"
    "\tfileName\t\"ui\\textures\\a\"\r\n"
    "\trect\t\t\"0 0 70 70\"\r\n"
    "}\r\n"
    "\r\n"
    "Texture\r\n"
    "{\r\n"
    "\tname\t\t\"Icon_B\"\r\n"
    "\tfileName\t\"ui\\textures\\b\"\t// old\r\n"
    "\trect\t\t\"70 0 70 70\"\r\n"
    "}\r\n";

const std::string kSanim =
    "base_res 1920 1080\n"
    "\n"
    "objects 2\n"
    "{\n"
    "\tobject agen\n"
    "\t{\n"
    "\t\tdefaults\n"
    "\t\t{\n"
    "\t\t\tanchor \"LB\"\n"
    "\t\t\tpos 17 799\n"
    "\t\t}\n"
    "\t\tvisible_on idle\n"
    "\t}\n"
    "\tobject ship { defaults { pos 1 2 } }\n"
    "}\n";

const std::string kResources =
    "Item\n{\n\tfileName \"ui\\scripts\\work\\a.lua\"\n\tresourceType \"SCRIPT\"\n}\n\n"
    "Item\n{\n\tfileName \"ui\\scripts\\work\\b.lua\"\n\tresourceType \"SCRIPT\"\n}\n";

void parser_tests() {
    auto d = sf::mod::parse(kSanim);
    check(d.top.size() == 2 && !d.top[0].block && d.top[1].block, "top level: a line and a block");
    const auto& objs = d.top[1];
    check(objs.tokens.size() == 2 && objs.tokens[1].value == "2", "header tokens: objects 2");
    check(objs.children.size() == 2 && objs.children[1].children.size() == 1,
          "nested blocks, including { } on one line");
    check(objs.children[0].children[0].children[1].value(d.text) == "17 799", "unquoted values keep their spaces");
    check(objs.children[0].children[0].children[0].value(d.text) == "LB", "a single quoted value loses its quotes");

    auto t = sf::mod::parse(kTextures);
    check(t.eol == "\r\n" && t.top.size() == 2, "CRLF file with a BOM and a comment");
    check(t.top[1].children[1].value(t.text) == "ui\\textures\\b", "backslashes are plain characters; comment dropped");

    auto bad = [](const std::string& s) {
        try {
            sf::mod::parse(s);
        } catch (const sf::mod::ParseError& e) {
            return e.line;
        }
        return std::size_t(0);
    };
    check(bad("a\n{\nb 1\n") == 2, "missing } reported at the opening line");
    check(bad("a 1\n}\n") == 2, "stray } reported");
    check(bad("a \"oops\nb 1\n") == 1, "unterminated string reported");
}

void apply_tests() {
    // No operations: byte for byte the same.
    check(run(kTextures, "patch \"x\" { }") == kTextures, "empty patch leaves the file untouched");

    // set: a value replaced in place, separators and the comment kept, CRLF kept.
    std::string r = run(kTextures,
                        "patch \"x\"\n{\n\tset Texture[name=\"Icon_B\"]\n\t{\n\t\tfileName \"ui\\textures\\new\"\n\t}\n}\n");
    check(contains(r, "\tfileName\t\"ui\\textures\\new\"\t// old\r\n"), "set replaces only the value");
    check(r.size() == kTextures.size() + 2, "set changes nothing else");

    // set: a new key goes at the end of the block with the block's indentation and line ending.
    r = run(kTextures, "patch \"x\" { set Texture[name=Icon_A] { hidden 1 } }");
    check(contains(r, "\trect\t\t\"0 0 70 70\"\r\n\thidden 1\r\n}\r\n"), "set adds a missing key at the end of the block");

    // Path selectors with header prefixes, on a one-line block too.
    r = run(kSanim, "patch \"x\" {\n set objects/object agen/defaults { pos 17 -799 }\n set objects/object ship/defaults { pos 5 6 }\n}");
    check(contains(r, "\t\t\tpos 17 -799\n") && contains(r, "defaults { pos 5 6 }"), "set through a path, both layouts");

    // Top level.
    r = run(kSanim, "patch \"x\" { set . { base_res 3440 1440 } }");
    check(r.rfind("base_res 3440 1440\n", 0) == 0, "set . changes a top-level key");

    // Selector errors.
    check(contains(run_err(kTextures, "patch \"x\" { set Texture[name=Nope] { a 1 } }"), "matches nothing"),
          "a selector matching nothing fails");
    check(contains(run_err(kTextures, "patch \"x\" { set Texture { a 1 } }"), "matches 2 blocks"),
          "a selector matching two blocks fails without all");
    r = run(kTextures, "patch \"x\" { set all Texture { rect \"0 0 1 1\" } }");
    check(contains(r, "\"Icon_A\"") && r.find("70 70") == std::string::npos, "set all changes every match");
    check(contains(run(kSanim, "patch \"x\" { set objects/*/defaults[anchor=LB] { pos 0 0 } }"), "pos 0 0\n"),
          "* and a filter on a later segment");

    // unset and remove take whole lines.
    r = run(kSanim, "patch \"x\" { unset objects/object agen/defaults { anchor } }");
    check(!contains(r, "anchor") && contains(r, "\t\t{\n\t\t\tpos 17 799\n"), "unset removes the line");
    r = run(kSanim, "patch \"x\" { remove objects/object agen }");
    check(!contains(r, "agen") && contains(r, "objects 2\n{\n\tobject ship"), "remove takes the block's lines");

    // add after: the use for uiresources.txt.
    r = run(kResources,
            "patch \"x\"\n{\n"
            "\tadd after Item[fileName=\"ui\\scripts\\work\\a.lua\"]\n\t{\n"
            "\t\tItem\n\t\t{\n\t\t\tfileName \"ui\\mods\\m\\m.lua\"\n\t\t}\n\t}\n}\n");
    check(contains(r, "a.lua\"\n\tresourceType \"SCRIPT\"\n}\nItem\n{\n\tfileName \"ui\\mods\\m\\m.lua\"\n}\n\nItem"),
          "add after inserts the block right after, dedented to the target's column");
    r = run(kResources, "patch \"x\" { add before Item[fileName=\"ui\\scripts\\work\\a.lua\"] {\n Item { fileName z }\n} }");
    check(r.rfind("Item { fileName z }\nItem\n", 0) == 0, "add before");
    r = run(kSanim, "patch \"x\" {\n add into objects {\n  object new\n  {\n  }\n }\n}");
    check(contains(r, "\tobject new\n\t{\n\t}\n}\n"), "add into goes at the end, indented like the children");
    r = run(kResources, "patch \"x\" {\n add {\n  Item { fileName end }\n }\n}");
    check(r.size() > kResources.size() && r.substr(kResources.size()) == "Item { fileName end }\n", "add appends to the file");
    r = run(kSanim, "patch \"x\" {\n replace objects/object ship {\n  object ship\n  {\n    x 1\n  }\n }\n}");
    check(contains(r, "\tobject ship\n\t{\n\t  x 1\n\t}\n}\n"), "replace puts the new block in the old one's place");

    // Operations run in order, each on the result of the last.
    r = run(kResources,
            "patch \"x\" {\n add after Item[fileName=\"ui\\scripts\\work\\b.lua\"] {\n Item { fileName c }\n }\n"
            " set Item[fileName=c] { resourceType \"SCRIPT\" }\n}");
    check(contains(r, "Item { fileName c resourceType \"SCRIPT\" }"), "a later op sees an earlier op's block");

    // Duplicate keys are refused rather than guessed.
    check(contains(run_err("a\n{\nk 1\nk 2\n}\n", "patch \"x\" { set a { k 3 } }"), "appears 2 times"),
          "set refuses a key that appears twice");
    // Errors name the patch file and line.
    check(contains(run_err(kTextures, "patch \"x\"\n{\n\tremove Texture[name=Nope]\n}"), "t.sfpatch:3: remove Texture[name=Nope]"),
          "errors give patch file, line and operation");
}

void parse_patch_tests() {
    check(contains(parse_err("patch \"x\" { frob a { } }"), "unknown operation \"frob\""), "unknown operation");
    check(contains(parse_err("patch \"x\" { set a }"), "set needs"), "set without a body");
    check(contains(parse_err("patch \"x\" { set a { k } }"), "has no value"), "set key without value");
    check(contains(parse_err("patch \"x\" { remove a { } }"), "no { }"), "remove with a body");
    check(contains(parse_err("Texture { }"), "expected patch"), "top level must be patch blocks");
    check(contains(parse_err("patch \"x\" { set a[k] { b 1 } }"), "expected [key=value]"), "bad selector caught when parsing");
    check(contains(parse_err("patch \"x\" { remove . }"), "top level"), "remove . refused");
    auto p = sf::mod::parse_patch("patch \"data\\ui\\x.txt\" { set all a { k 1 } }", "n");
    check(p.files[0].file == "data/ui/x.txt" && p.files[0].ops[0].all, "path separators normalised; all parsed");
}

void diff_tests() {
    std::string mod = kTextures;
    mod.replace(mod.find("\"70 0 70 70\""), 12, "\"1 2 3 4\"");
    std::string patch = sf::mod::diff(kTextures, mod, "t.txt");
    check(contains(patch, "set Texture[name=\"Icon_B\"]") && contains(patch, "rect\t\t\"1 2 3 4\""),
          "diff names the block by a unique key");
    check(run(kTextures, patch) == mod, "diff's patch reproduces the change");
    check(sf::mod::diff(kTextures, kTextures, "t.txt").empty(), "no change, no patch");
    std::string less = kSanim;
    less.erase(less.find("\tobject ship"), std::string("\tobject ship { defaults { pos 1 2 } }\n").size());
    bool threw = false;
    try {
        sf::mod::diff(kSanim, less, "x");
    } catch (const sf::mod::PatchError& e) {
        threw = contains(e.what(), "added or removed");
    }
    check(threw, "diff refuses structural changes it can't express");
}

std::string read(const fs::path& p) { return test::read_file(p); }

void real_files(const fs::path& game, const fs::path& retro) {
    fs::path ui = game / "data" / "ui";
    if (!fs::is_directory(ui)) {
        std::printf("SKIP: no game folder given (data/ui)\n");
        return;
    }
    std::size_t ok = 0, total = 0;
    for (const auto& e : fs::recursive_directory_iterator(ui)) {
        auto ext = e.path().extension().string();
        if (!e.is_regular_file() || (ext != ".sanim" && e.path().filename() != "uiresources.txt" &&
                                     (e.path().parent_path().filename() != "common" || ext != ".txt")))
            continue;
        ++total;
        try {
            auto text = read(e.path());
            auto d = sf::mod::parse(text);
            if (sf::mod::apply(text, {}, "none") == text) ++ok;
        } catch (const std::exception& ex) {
            std::printf("  %s: %s\n", e.path().string().c_str(), ex.what());
        }
    }
    check(total > 100 && ok == total,
          "every .sanim, uiresources.txt and textures/common list parses (" + std::to_string(ok) + "/" +
              std::to_string(total) + ")");

    if (!fs::is_directory(retro / "textures")) {
        std::printf("SKIP: no coms-retro-sc-ui folder given\n");
        return;
    }
    for (const char* rel : {"textures/common/module_new.txt", "graphics/equip_1080/equip_main2.sanim",
                            "graphics/equip_1080/passiv_slots_line.sanim", "graphics/equip_1080/passiv_slots_main.sanim",
                            "graphics/equip_1080/passiv_upd.sanim", "graphics/equip_1080/ship_slots_main.sanim"}) {
        std::string base = read(ui / rel), mod = read(retro / rel);
        std::string what = std::string("retro UI ") + rel;
        try {
            std::string patch = sf::mod::diff(base, mod, std::string("data/ui/") + rel);
            std::size_t ops = 0;
            for (std::size_t i = patch.find("\n\tset "); i != std::string::npos; i = patch.find("\n\tset ", i + 1)) ++ops;
            check(run(base, patch) == mod, what + ": diff + apply gives the mod's file byte for byte (" +
                                               std::to_string(ops) + " set ops, " + std::to_string(patch.size()) +
                                               " bytes vs " + std::to_string(mod.size()) + ")");
        } catch (const std::exception& e) {
            check(false, what + ": " + e.what());
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    test::exit_on_crash();
    parser_tests();
    apply_tests();
    parse_patch_tests();
    diff_tests();
    real_files(argc > 1 ? fs::path(argv[1]) : fs::path(), argc > 2 ? fs::path(argv[2]) : fs::path());
    return test::failures ? 1 : 0;
}

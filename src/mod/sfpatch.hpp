#pragma once

// Data patches (.sfpatch): named changes to files in the game's block format (mod/blocks.hpp).
// The format is described in docs/mods.md#data-patches. A patch file is itself in the block
// format:
//
//     patch "data/ui/textures/common/module_new.txt"
//     {
//         set Texture[name="ItemSubtype_FocusedRay"]
//         {
//             rect    "255 294 40 40"
//         }
//         remove objects/object agen
//     }
//
// apply() runs one file's operations in order; any failure throws and the caller keeps the text it
// had, so a patch never half applies. diff() goes the other way: from a stock file and a changed
// copy it writes the patch that turns one into the other. Portable.

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sf::mod {

struct PatchError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Op {
    enum class Kind { Set, Unset, Add, AddInto, AddBefore, AddAfter, Replace, Remove };
    Kind kind = Kind::Set;
    bool all = false;      // `set all ...`: every match, instead of exactly one
    std::string selector;  // as written; "." is the top level of the file
    struct Entry {
        std::string key;
        std::string line;   // set: the whole line as written (key, separator, values)
        std::string value;  // set: the values as written
    };
    std::vector<Entry> entries;  // set, unset
    std::string body;            // add, replace: the lines inside the braces as written
    std::size_t line = 0;        // in the patch file
};

struct FilePatch {
    std::string file;  // game path with '/' separators, as written after `patch`
    std::vector<Op> ops;
    std::size_t line = 0;
};

struct Patch {
    std::string name;  // for messages, e.g. retro-layout/patches/icons.sfpatch
    std::vector<FilePatch> files;
};

// Throws PatchError (with "<name>:<line>: ...") for anything that isn't a valid patch.
Patch parse_patch(std::string_view text, const std::string& name);

// `text` with every operation of `patch` applied, in order. Throws PatchError naming the
// operation that failed (a selector matching nothing or too much, a key set twice in one block,
// the target not parsing).
//
// `touched`, if given, gets what each operation changed, for spotting two mods changing the same
// thing: a block's address (its path, each step named so it matches only that block, like diff()
// names them: `objects 18/object agen/defaults`, `Texture[name="X"]`) for replace and remove,
// the address plus "#<key>" for set and unset. Adding blocks touches nothing (adds combine).
std::string apply(std::string text, const FilePatch& patch, const std::string& patch_name,
                  std::vector<std::string>* touched = nullptr);

// Whether two touched addresses overlap: the same, or one inside the other.
bool overlaps(const std::string& a, const std::string& b);

// The .sfpatch text that turns `base` into `modded` for game path `file`: `set` and `unset` for
// changed, added and removed keys. Throws PatchError where it can't say it with those (blocks
// added, removed or renamed; a block it can't name uniquely; a duplicated key that changed).
std::string diff(std::string_view base, std::string_view modded, const std::string& file);

}  // namespace sf::mod

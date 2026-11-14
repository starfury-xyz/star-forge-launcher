#pragma once

// Patchsets: byte patches for one executable or DLL. Port of star-forge crates/sc-patch.
//
// A patchset (<exe>.patchset.toml) lists patches; each patch is a set of writes, and every write
// records the bytes it expects to replace. Nothing is written unless those bytes are there, and
// the whole set only applies to the game builds whose SHA-1 it lists.
//
// This module only knows the format and how to check and apply a patchset against an image
// (anything implementing Target). How that image is reached is up to the caller: the agent
// patches the running game's memory, the launcher can check the file on disk (FileImage).

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace sf::patch {

using Bytes = std::vector<uint8_t>;

inline constexpr int64_t kFormat = 1;                        // the resolved patchset format we read
inline constexpr const char* kSuffix = ".patchset.toml";    // file name suffix: <exe>.patchset.toml

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Write {
    uint64_t va = 0;      // virtual address, in the patchset's image_base
    Bytes orig;           // bytes that must be there before patching
    Bytes bytes;          // replacement, same length as orig (empty for a midhook)
    std::string midhook;  // or: a native mid-function hook by name (orig = the stolen bytes)
    std::string comment;
};

struct Patch {
    std::string id;
    std::string title;
    std::string description;
    bool default_on = true;  // `default` in the file
    bool optional = false;   // a verification failure skips this patch instead of failing the set
    std::vector<Write> writes;
};

// The player's on/off choices for one module's patches: patch id -> on.
using Choices = std::map<std::string, bool>;

struct Enabled {
    std::vector<const Patch*> on;
    std::vector<std::string> warnings;  // choices naming patches that don't exist
};

struct Patchset {
    std::string exe;                // module it targets, e.g. "game.exe"
    std::vector<std::string> sha1;  // lower-case hex SHA-1s of the builds the addresses hold for
    std::string build;              // informational
    uint64_t image_base = 0;        // base the addresses assume; images elsewhere are patched at the same offset
    std::vector<Patch> patches;

    // Parses a resolved <exe>.patchset.toml; `name` is used in errors. Throws Error.
    static Patchset parse(const std::string& text, const std::string& name);

    bool accepts(const std::string& sha) const;  // `sha` is one of the builds this set was made for
    std::string expected_sha1() const;           // the accepted hashes, for messages
    bool targets(const std::string& module) const;  // case-insensitive, like Windows
    // Patches that are on: each patch's `default`, unless `choices` says otherwise.
    Enabled enabled(const Choices* choices) const;
};

// One message per pair of patches that write the same bytes.
std::vector<std::string> overlaps(const std::vector<const Patch*>& patches);
// Whether two patches write any of the same bytes (so they can't both be on).
bool overlap(const Patch& a, const Patch& b);

// An image patches are checked against and written to, addressed by virtual address in the
// patchset's address space.
class Target {
public:
    virtual ~Target() = default;
    virtual bool read(uint64_t va, uint8_t* out, size_t n) = 0;
    virtual bool write(uint64_t va, const uint8_t* data, size_t n) = 0;
};

struct State {
    enum Kind {
        Pristine,  // every write's original bytes are there
        Applied,   // every write is already in place
        Mismatch,  // neither; `why` says where
    } kind;
    std::string why;
};

State check(Target& t, const Patch& p);
// Installs one midhook write (w.midhook at w.va, w.orig the stolen bytes). Returns "" or the error.
using MidhookInstaller = std::function<std::string(const Write& w)>;
// Applies a patch whose state is Pristine, reading every write back. Returns "" or the error.
// Midhook writes need `midhook` (only the server proxy has one); without it they fail.
std::string apply(Target& t, const Patch& p, const MidhookInstaller& midhook = {});

// A PE file in memory (e.g. game.exe read from disk), addressed by virtual address. For checking
// a patchset before launch, or writing a patched copy.
class FileImage : public Target {
public:
    static FileImage parse(Bytes data);  // throws Error for anything that isn't a PE file
    bool read(uint64_t va, uint8_t* out, size_t n) override;
    bool write(uint64_t va, const uint8_t* data, size_t n) override;

    Bytes data;
    uint64_t image_base = 0;

private:
    struct Section {
        uint64_t rva, virtual_size, file_offset, raw_size;
    };
    std::vector<Section> sections_;
    std::optional<size_t> offset(uint64_t va, size_t n) const;  // file offset of n bytes at va
};

// What checking a patchset against a file found.
struct Report {
    std::vector<std::string> ok;        // patches that would apply
    std::vector<std::string> skipped;   // optional patches that would be skipped, with the reason
    std::vector<std::string> errors;    // problems that stop the set from applying
    std::vector<std::string> warnings;
};

// Checks the enabled patches of `set` against the file `data`, the way a loader would.
Report verify_file(const Patchset& set, Bytes data, const Choices* choices);

// Hex like "8b80 0c2e0000" (whitespace and '_' allowed anywhere); nullopt if invalid.
std::optional<Bytes> parse_hex(const std::string& text);
std::string to_hex(const Bytes& bytes);
std::string hex_va(uint64_t va);  // "0x140001002", like Rust's {:#x}
// A module name a patchset may target: a plain file name.
bool valid_module(const std::string& name);

}  // namespace sf::patch

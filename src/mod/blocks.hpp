#pragma once

// The game's block text format: the shared syntax of uiresources.txt, texture lists
// (textures/*.txt), .sanim layouts and other data files. Lines of `key value ...`, and blocks of a
// header line followed by `{ ... }`:
//
//     Texture
//     {
//         name      "ItemSubtype_FocusedRay"
//         rect      "973 290 70 70"
//     }
//     object agen { defaults { pos 17 799 } }
//
// `//` starts a comment (at the start of a token). A quoted string runs to the next `"`, and a
// backslash in it is an ordinary character (paths are written `"ui\textures\x"`).
//
// The parser keeps byte offsets into the text and changes nothing, so an edit can splice the text
// and leave everything else as it was: comments, tabs, CRLF, the BOM, the code page (the game's
// files are cp1251/latin-1; text is treated as bytes). Portable.

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sf::mod {

struct ParseError : std::runtime_error {
    ParseError(const std::string& what, std::size_t line) : std::runtime_error(what), line(line) {}
    std::size_t line;  // 1-based
};

struct Token {
    std::size_t begin = 0, end = 0;  // the raw token, quotes included
    std::string value;               // the token without its quotes
    bool quoted = false;
};

struct Node {
    bool block = false;
    // A line: the key and its values. A block: the header (empty for a bare `{`).
    std::vector<Token> tokens;
    std::size_t begin = 0;  // first token (or the `{` of a block without a header)
    std::size_t end = 0;    // after the last token of a line, after the `}` of a block
    std::size_t open = 0, close = 0;  // blocks: offsets of `{` and `}`
    std::vector<Node> children;       // blocks only

    // The values of a line as written: from the first value to the end of the last ("" if none).
    std::string_view raw_value(std::string_view text) const;
    // The values with the quotes of a single quoted value removed: `"a b"` -> a b, `17 799` -> 17 799.
    std::string value(std::string_view text) const;
    // tokens[0].value, or "".
    const std::string& key() const;
};

struct Doc {
    std::string text;
    std::vector<Node> top;
    std::string eol;  // "\r\n" if the first line ends that way, else "\n"
};

// Throws ParseError for an unmatched brace or an unterminated string.
Doc parse(std::string text);

// 1-based line number of a byte offset.
std::size_t line_of(std::string_view text, std::size_t offset);

// ASCII case-insensitive equality (keys and headers).
bool iequals(std::string_view a, std::string_view b);

}  // namespace sf::mod

#include "mod/blocks.hpp"

namespace sf::mod {

namespace {

enum class Kind { Word, Open, Close, Newline };

struct Tok {
    Kind kind;
    Token t;
};

bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; }

std::vector<Tok> tokenize(std::string_view s) {
    std::vector<Tok> out;
    std::size_t i = 0;
    // UTF-8 byte order mark: whitespace as far as parsing goes.
    if (s.substr(0, 3) == "\xEF\xBB\xBF") i = 3;
    while (i < s.size()) {
        char c = s[i];
        if (c == '\n') {
            out.push_back({Kind::Newline, {i, i + 1, "", false}});
            ++i;
        } else if (space(c)) {
            ++i;
        } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') ++i;
        } else if (c == '{' || c == '}') {
            out.push_back({c == '{' ? Kind::Open : Kind::Close, {i, i + 1, std::string(1, c), false}});
            ++i;
        } else if (c == '"') {
            std::size_t j = s.find('"', i + 1);
            std::size_t nl = s.find('\n', i + 1);
            if (j == std::string_view::npos || (nl != std::string_view::npos && nl < j))
                throw ParseError("unterminated string", line_of(s, i));
            out.push_back({Kind::Word, {i, j + 1, std::string(s.substr(i + 1, j - i - 1)), true}});
            i = j + 1;
        } else {
            // A bare word. Quotes and [brackets] inside it may hold spaces: Item[fileName="a b"].
            std::size_t j = i;
            bool quote = false;
            int bracket = 0;
            while (j < s.size() && s[j] != '\n') {
                char d = s[j];
                if (quote) {
                    if (d == '"') quote = false;
                } else if (d == '"') {
                    quote = true;
                } else if (d == '[') {
                    ++bracket;
                } else if (d == ']') {
                    if (bracket > 0) --bracket;
                } else if (bracket == 0 && (space(d) || d == '{' || d == '}')) {
                    break;
                }
                ++j;
            }
            if (quote) throw ParseError("unterminated string", line_of(s, i));
            out.push_back({Kind::Word, {i, j, std::string(s.substr(i, j - i)), false}});
            i = j;
        }
    }
    return out;
}

struct Parser {
    std::string_view text;
    std::vector<Tok> toks;
    std::size_t i = 0;

    // Reads nodes up to a `}` (left unread) or the end.
    std::vector<Node> body(bool top) {
        std::vector<Node> nodes;
        while (i < toks.size()) {
            Kind k = toks[i].kind;
            if (k == Kind::Newline) {
                ++i;
                continue;
            }
            if (k == Kind::Close) {
                if (top) throw ParseError("unexpected }", line_of(text, toks[i].t.begin));
                return nodes;
            }
            Node n;
            std::size_t j = i;
            while (j < toks.size() && toks[j].kind == Kind::Word) n.tokens.push_back(toks[j++].t);
            // A header: the statement is followed by `{`, on its line or after blank lines.
            std::size_t k2 = j;
            while (k2 < toks.size() && toks[k2].kind == Kind::Newline) ++k2;
            if (k2 < toks.size() && toks[k2].kind == Kind::Open) {
                n.block = true;
                n.open = toks[k2].t.begin;
                n.begin = n.tokens.empty() ? n.open : n.tokens.front().begin;
                i = k2 + 1;
                n.children = body(false);
                if (i >= toks.size()) throw ParseError("missing } for the block opened here", line_of(text, n.open));
                n.close = toks[i].t.begin;
                n.end = n.close + 1;
                ++i;
            } else {
                n.begin = n.tokens.front().begin;
                n.end = n.tokens.back().end;
                i = j;
            }
            nodes.push_back(std::move(n));
        }
        return nodes;
    }
};

}  // namespace

std::string_view Node::raw_value(std::string_view text) const {
    if (tokens.size() < 2) return {};
    return text.substr(tokens[1].begin, tokens.back().end - tokens[1].begin);
}

std::string Node::value(std::string_view text) const {
    if (tokens.size() == 2 && tokens[1].quoted) return tokens[1].value;
    return std::string(raw_value(text));
}

const std::string& Node::key() const {
    static const std::string empty;
    return tokens.empty() ? empty : tokens[0].value;
}

Doc parse(std::string text) {
    Doc d;
    d.text = std::move(text);
    Parser p{d.text, tokenize(d.text)};
    d.top = p.body(true);
    std::size_t nl = d.text.find('\n');
    d.eol = (nl != std::string::npos && nl > 0 && d.text[nl - 1] == '\r') ? "\r\n" : "\n";
    return d;
}

std::size_t line_of(std::string_view text, std::size_t offset) {
    std::size_t n = 1;
    for (std::size_t i = 0; i < offset && i < text.size(); ++i)
        if (text[i] == '\n') ++n;
    return n;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

}  // namespace sf::mod

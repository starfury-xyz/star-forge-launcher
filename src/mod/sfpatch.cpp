#include "mod/sfpatch.hpp"

#include <algorithm>
#include <map>

#include "mod/blocks.hpp"

namespace sf::mod {

namespace {

// --- text helpers -----------------------------------------------------------------------------

bool blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

std::size_t line_start(std::string_view t, std::size_t pos) {
    std::size_t nl = pos == 0 ? std::string_view::npos : t.rfind('\n', pos - 1);
    return nl == std::string_view::npos ? 0 : nl + 1;
}

// Only spaces and tabs (and a BOM at the very start) between the start of pos's line and pos.
bool only_blank_before(std::string_view t, std::size_t pos) {
    std::size_t s = line_start(t, pos);
    if (s == 0 && t.substr(0, 3) == "\xEF\xBB\xBF") s = 3;
    for (std::size_t i = s; i < pos; ++i)
        if (!blank(t[i])) return false;
    return true;
}

// After pos: the offset just past the end of its line if the rest of the line is blank or a
// comment, else npos. The end of the text counts as a line end.
std::size_t rest_of_line_end(std::string_view t, std::size_t pos) {
    std::size_t i = pos;
    while (i < t.size() && blank(t[i])) ++i;
    if (i + 1 < t.size() && t[i] == '/' && t[i + 1] == '/') {
        while (i < t.size() && t[i] != '\n') ++i;
    }
    if (i == t.size()) return i;
    if (t[i] == '\n') return i + 1;
    return std::string_view::npos;
}

std::string indent_of(std::string_view t, std::size_t pos) {
    std::size_t s = line_start(t, pos);
    if (s == 0 && t.substr(0, 3) == "\xEF\xBB\xBF") s = 3;
    std::size_t e = s;
    while (e < pos && (t[e] == ' ' || t[e] == '\t')) ++e;
    return std::string(t.substr(s, e - s));
}

// `body` with its common indentation replaced by `indent`, lines ended with `eol` (the last too).
std::string reindent(std::string_view body, const std::string& indent, const std::string& eol) {
    std::vector<std::string> lines;
    std::size_t p = 0;
    while (p <= body.size()) {
        std::size_t nl = body.find('\n', p);
        std::string l(body.substr(p, nl == std::string_view::npos ? std::string_view::npos : nl - p));
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        if (nl == std::string_view::npos) break;
        p = nl + 1;
    }
    while (!lines.empty() && lines.back().find_first_not_of(" \t") == std::string::npos) lines.pop_back();
    std::size_t common = std::string::npos;
    std::string first_indent;
    for (const auto& l : lines) {
        std::size_t n = l.find_first_not_of(" \t");
        if (n == std::string::npos) continue;
        if (common == std::string::npos) {
            common = n;
            first_indent = l.substr(0, n);
        } else {
            std::size_t k = 0;
            while (k < n && k < common && l[k] == first_indent[k]) ++k;
            common = k;
        }
    }
    std::string out;
    for (const auto& l : lines) {
        if (l.find_first_not_of(" \t") != std::string::npos) out += indent + l.substr(common);
        out += eol;
    }
    return out;
}

// The lines between a block's braces, as written.
std::string_view inner(std::string_view t, const Node& n) {
    std::size_t b = n.open + 1;
    std::size_t e = rest_of_line_end(t, b);
    if (e != std::string_view::npos && e <= n.close) b = e;
    std::size_t c = n.close;
    if (only_blank_before(t, c)) c = std::max(b, line_start(t, c));
    return t.substr(b, c - b);
}

// --- selectors ------------------------------------------------------------------------------

struct Segment {
    std::vector<std::string> words;
    std::vector<std::pair<std::string, std::string>> filters;
};

std::string unquote(std::string_view s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return std::string(s.substr(1, s.size() - 2));
    return std::string(s);
}

std::vector<Segment> parse_selector(std::string_view sel) {
    std::vector<std::string> parts;
    std::string cur;
    bool quote = false;
    int bracket = 0;
    for (char c : sel) {
        if (quote) {
            if (c == '"') quote = false;
        } else if (c == '"') {
            quote = true;
        } else if (c == '[') {
            ++bracket;
        } else if (c == ']') {
            --bracket;
        } else if (c == '/' && bracket == 0) {
            parts.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    parts.push_back(cur);

    std::vector<Segment> segs;
    for (const auto& part : parts) {
        Segment s;
        std::size_t lb = std::string::npos;
        bool q = false;
        for (std::size_t i = 0; i < part.size(); ++i) {
            if (part[i] == '"') q = !q;
            if (!q && part[i] == '[') {
                lb = i;
                break;
            }
        }
        std::string head = part.substr(0, lb);
        std::size_t i = 0;
        while (i < head.size()) {
            while (i < head.size() && (head[i] == ' ' || head[i] == '\t')) ++i;
            if (i >= head.size()) break;
            std::size_t j = i;
            bool wq = false;
            while (j < head.size() && (wq || (head[j] != ' ' && head[j] != '\t'))) {
                if (head[j] == '"') wq = !wq;
                ++j;
            }
            s.words.push_back(unquote(head.substr(i, j - i)));
            i = j;
        }
        if (s.words.empty()) throw PatchError("empty selector segment in \"" + std::string(sel) + "\"");
        while (lb != std::string::npos && lb < part.size()) {
            std::size_t rb = std::string::npos;
            bool fq = false;
            for (std::size_t k = lb + 1; k < part.size(); ++k) {
                if (part[k] == '"') fq = !fq;
                if (!fq && part[k] == ']') {
                    rb = k;
                    break;
                }
            }
            if (rb == std::string::npos) throw PatchError("missing ] in selector \"" + std::string(sel) + "\"");
            std::string f = part.substr(lb + 1, rb - lb - 1);
            std::size_t eq = f.find('=');
            if (eq == std::string::npos) throw PatchError("expected [key=value] in selector \"" + std::string(sel) + "\"");
            std::string key = f.substr(0, eq);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            std::string val = f.substr(eq + 1);
            std::size_t a = val.find_first_not_of(" \t"), z = val.find_last_not_of(" \t");
            val = a == std::string::npos ? "" : val.substr(a, z - a + 1);
            s.filters.emplace_back(key, unquote(val));
            lb = rb + 1;
            if (lb < part.size() && part[lb] != '[') {
                std::string rest = part.substr(lb);
                if (rest.find_first_not_of(" \t") != std::string::npos)
                    throw PatchError("unexpected text after ] in selector \"" + std::string(sel) + "\"");
                break;
            }
        }
        segs.push_back(std::move(s));
    }
    return segs;
}

bool matches(std::string_view t, const Node& n, const Segment& s) {
    if (!n.block) return false;
    if (!(s.words.size() == 1 && s.words[0] == "*")) {
        if (s.words.size() > n.tokens.size()) return false;
        for (std::size_t i = 0; i < s.words.size(); ++i)
            if (!iequals(s.words[i], n.tokens[i].value)) return false;
    }
    for (const auto& [k, v] : s.filters) {
        bool found = false;
        for (const auto& c : n.children)
            if (!c.block && iequals(c.key(), k) && c.value(t) == v) {
                found = true;
                break;
            }
        if (!found) return false;
    }
    return true;
}

// The top level as a block, so "." works like any other selector.
Node root_node(const Doc& d) {
    Node r;
    r.block = true;
    r.children = d.top;
    r.open = std::string::npos;
    r.close = d.text.size();
    return r;
}

std::vector<const Node*> resolve(const Doc& d, const Node& root, const std::string& sel) {
    if (sel == ".") return {&root};
    std::vector<const Node*> cur{&root};
    for (const auto& seg : parse_selector(sel)) {
        std::vector<const Node*> next;
        for (const Node* p : cur)
            for (const auto& c : p->children)
                if (matches(d.text, c, seg)) next.push_back(&c);
        cur = std::move(next);
    }
    return cur;
}

// --- edits ----------------------------------------------------------------------------------

struct Edit {
    std::size_t pos, len;
    std::string text;
};

std::string splice(std::string t, std::vector<Edit> edits) {
    // Back to front, so earlier offsets stay valid; equal offsets keep their order in the output.
    std::vector<std::size_t> idx(edits.size());
    for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return edits[a].pos > edits[b].pos; });
    // Among equal positions, the later edit must be applied first: reverse those runs.
    for (std::size_t i = 0; i < idx.size();) {
        std::size_t j = i;
        while (j < idx.size() && edits[idx[j]].pos == edits[idx[i]].pos) ++j;
        std::reverse(idx.begin() + static_cast<std::ptrdiff_t>(i), idx.begin() + static_cast<std::ptrdiff_t>(j));
        i = j;
    }
    for (std::size_t k : idx) t.replace(edits[k].pos, edits[k].len, edits[k].text);
    return t;
}

// Inserting lines at the end of a block (or of the file, for the root).
Edit insert_into(const Doc& d, const Node& n, std::string_view body) {
    const std::string& t = d.text;
    if (n.open == std::string::npos) {  // root
        std::string text = reindent(body, "", d.eol);
        if (!t.empty() && t.back() != '\n') text = d.eol + text;
        return {t.size(), 0, text};
    }
    std::string ind;
    if (!n.children.empty())
        ind = indent_of(t, n.children.front().begin);
    else
        ind = indent_of(t, n.begin) + "\t";
    if (only_blank_before(t, n.close)) return {line_start(t, n.close), 0, reindent(body, ind, d.eol)};
    // A block on one line, `Item { fileName c }`: a single new line goes in on that line too.
    std::string one = reindent(body, "", "\n");
    if (std::count(one.begin(), one.end(), '\n') == 1) {
        one.pop_back();
        bool spaced = n.close > 0 && blank(t[n.close - 1]);
        return {n.close, 0, (spaced ? "" : " ") + one + " "};
    }
    return {n.close, 0, d.eol + reindent(body, ind, d.eol)};
}

Edit insert_before(const Doc& d, const Node& n, std::string_view body) {
    const std::string& t = d.text;
    std::string ind = indent_of(t, n.begin);
    std::string text = reindent(body, ind, d.eol);
    if (only_blank_before(t, n.begin)) return {line_start(t, n.begin), 0, text};
    return {n.begin, 0, d.eol + text + ind};
}

Edit insert_after(const Doc& d, const Node& n, std::string_view body) {
    const std::string& t = d.text;
    std::string text = reindent(body, indent_of(t, n.begin), d.eol);
    std::size_t e = rest_of_line_end(t, n.end);
    if (e == std::string_view::npos) return {n.end, 0, d.eol + text};
    if (e == t.size() && (t.empty() || t.back() != '\n')) return {e, 0, d.eol + text};
    return {e, 0, text};
}

// Removing a node: whole lines when it has them to itself.
Edit remove_node(const Doc& d, const Node& n, const std::string& with = "") {
    const std::string& t = d.text;
    std::size_t e = rest_of_line_end(t, n.end);
    if (with.empty() && only_blank_before(t, n.begin) && e != std::string_view::npos)
        return {line_start(t, n.begin), e - line_start(t, n.begin), ""};
    return {n.begin, n.end - n.begin, with};
}

const char* op_name(Op::Kind k) {
    switch (k) {
    case Op::Kind::Set: return "set";
    case Op::Kind::Unset: return "unset";
    case Op::Kind::Add: return "add";
    case Op::Kind::AddInto: return "add into";
    case Op::Kind::AddBefore: return "add before";
    case Op::Kind::AddAfter: return "add after";
    case Op::Kind::Replace: return "replace";
    case Op::Kind::Remove: return "remove";
    }
    return "?";
}

std::vector<Edit> edits_for(const Doc& d, const Node& root, const Op& op, std::string& err) {
    std::vector<Edit> edits;
    if (op.kind == Op::Kind::Add) {
        edits.push_back(insert_into(d, root, op.body));
        return edits;
    }
    std::vector<const Node*> targets = resolve(d, root, op.selector);
    if (targets.empty()) {
        err = "matches nothing";
        return {};
    }
    if (targets.size() > 1 && !op.all) {
        err = "matches " + std::to_string(targets.size()) + " blocks (write `" + op_name(op.kind) +
              " all` to change every one)";
        return {};
    }
    for (const Node* n : targets) {
        switch (op.kind) {
        case Op::Kind::Set:
        case Op::Kind::Unset:
            for (const auto& en : op.entries) {
                std::vector<const Node*> lines;
                for (const auto& c : n->children)
                    if (!c.block && iequals(c.key(), en.key)) lines.push_back(&c);
                if (lines.size() > 1) {
                    err = "key " + en.key + " appears " + std::to_string(lines.size()) + " times in the block";
                    return {};
                }
                if (op.kind == Op::Kind::Unset) {
                    if (!lines.empty()) edits.push_back(remove_node(d, *lines[0]));
                } else if (lines.empty()) {
                    edits.push_back(insert_into(d, *n, en.line));
                } else if (lines[0]->tokens.size() < 2) {
                    edits.push_back({lines[0]->end, 0, " " + en.value});
                } else {
                    const Node& l = *lines[0];
                    edits.push_back({l.tokens[1].begin, l.tokens.back().end - l.tokens[1].begin, en.value});
                }
            }
            break;
        case Op::Kind::AddInto: edits.push_back(insert_into(d, *n, op.body)); break;
        case Op::Kind::AddBefore: edits.push_back(insert_before(d, *n, op.body)); break;
        case Op::Kind::AddAfter: edits.push_back(insert_after(d, *n, op.body)); break;
        case Op::Kind::Replace: {
            std::string text = reindent(op.body, "", d.eol);
            // The new block takes the old one's place: first line at its column, the rest indented.
            std::string ind = indent_of(d.text, n->begin);
            std::string out;
            std::size_t p = 0;
            bool first = true;
            while (p < text.size()) {
                std::size_t e = text.find(d.eol, p);
                std::string l = text.substr(p, e - p);
                if (!first) out += d.eol + (l.empty() ? "" : ind);
                out += l;
                first = false;
                p = e + d.eol.size();
            }
            edits.push_back({n->begin, n->end - n->begin, out});
            break;
        }
        case Op::Kind::Remove: edits.push_back(remove_node(d, *n)); break;
        case Op::Kind::Add: break;
        }
    }
    return edits;
}

}  // namespace

// --- parse_patch ----------------------------------------------------------------------------

Patch parse_patch(std::string_view text, const std::string& name) {
    Doc d;
    try {
        d = parse(std::string(text));
    } catch (const ParseError& e) {
        throw PatchError(name + ":" + std::to_string(e.line) + ": " + e.what());
    }
    const std::string& t = d.text;
    auto fail = [&](std::size_t off, const std::string& what) {
        throw PatchError(name + ":" + std::to_string(line_of(t, off)) + ": " + what);
    };
    Patch p;
    p.name = name;
    for (const auto& top : d.top) {
        if (!top.block || top.tokens.size() != 2 || !iequals(top.tokens[0].value, "patch"))
            fail(top.begin, "expected patch \"<game path>\" { ... }");
        FilePatch fp;
        fp.file = top.tokens[1].value;
        std::replace(fp.file.begin(), fp.file.end(), '\\', '/');
        fp.line = line_of(t, top.begin);
        for (const auto& c : top.children) {
            Op op;
            op.line = line_of(t, c.begin);
            const std::string& w = c.key();
            std::size_t idx = 1;
            auto selector_from = [&](std::size_t i) {
                if (i >= c.tokens.size()) fail(c.begin, w + ": missing selector");
                op.selector = t.substr(c.tokens[i].begin, c.tokens.back().end - c.tokens[i].begin);
            };
            auto want_all = [&] {
                if (idx < c.tokens.size() && c.tokens[idx].value == "all" && !c.tokens[idx].quoted) {
                    op.all = true;
                    ++idx;
                }
            };
            if (iequals(w, "set") || iequals(w, "unset")) {
                op.kind = iequals(w, "set") ? Op::Kind::Set : Op::Kind::Unset;
                if (!c.block) fail(c.begin, w + " needs { key value } lines");
                want_all();
                selector_from(idx);
                for (const auto& e : c.children) {
                    if (e.block) fail(e.begin, w + " takes key lines, not blocks");
                    Op::Entry en;
                    en.key = e.key();
                    if (op.kind == Op::Kind::Set) {
                        if (e.tokens.size() < 2) fail(e.begin, "set: " + en.key + " has no value");
                        en.line = t.substr(e.begin, e.end - e.begin);
                        en.value = std::string(e.raw_value(t));
                    } else if (e.tokens.size() != 1) {
                        fail(e.begin, "unset takes keys only");
                    }
                    op.entries.push_back(std::move(en));
                }
            } else if (iequals(w, "add")) {
                if (!c.block) fail(c.begin, "add needs { blocks }");
                if (c.tokens.size() == 1) {
                    op.kind = Op::Kind::Add;
                } else {
                    const std::string& where = c.tokens[1].value;
                    if (iequals(where, "into")) op.kind = Op::Kind::AddInto;
                    else if (iequals(where, "before")) op.kind = Op::Kind::AddBefore;
                    else if (iequals(where, "after")) op.kind = Op::Kind::AddAfter;
                    else fail(c.begin, "expected add, add into, add before or add after");
                    idx = 2;
                    want_all();
                    selector_from(idx);
                }
                op.body = std::string(inner(t, c));
            } else if (iequals(w, "replace")) {
                op.kind = Op::Kind::Replace;
                if (!c.block || c.children.size() != 1 || !c.children[0].block)
                    fail(c.begin, "replace needs exactly one block inside { }");
                want_all();
                selector_from(idx);
                // From the start of its line, so the first line's indentation counts when dedenting.
                const Node& b = c.children[0];
                std::size_t from = only_blank_before(t, b.begin) ? line_start(t, b.begin) : b.begin;
                op.body = t.substr(from, b.end - from);
            } else if (iequals(w, "remove")) {
                op.kind = Op::Kind::Remove;
                if (c.block) fail(c.begin, "remove takes a selector only, no { }");
                want_all();
                selector_from(idx);
            } else {
                fail(c.begin, "unknown operation \"" + w + "\" (expected set, unset, add, replace or remove)");
            }
            if (!op.selector.empty() && op.selector != ".") {
                try {
                    parse_selector(op.selector);
                } catch (const PatchError& e) {
                    fail(c.begin, e.what());
                }
            }
            if (op.selector == "." && op.kind != Op::Kind::Set && op.kind != Op::Kind::Unset &&
                op.kind != Op::Kind::AddInto)
                fail(c.begin, std::string(op_name(op.kind)) + " can't target the top level (.)");
            fp.ops.push_back(std::move(op));
        }
        p.files.push_back(std::move(fp));
    }
    return p;
}

// --- apply ----------------------------------------------------------------------------------

std::string apply(std::string text, const FilePatch& patch, const std::string& patch_name) {
    for (const Op& op : patch.ops) {
        std::string where = patch_name + ":" + std::to_string(op.line) + ": " + op_name(op.kind) +
                            (op.selector.empty() ? "" : " " + op.selector);
        Doc d;
        try {
            d = parse(std::move(text));
        } catch (const ParseError& e) {
            throw PatchError(where + ": " + patch.file + " line " + std::to_string(e.line) + " doesn't parse: " + e.what());
        }
        Node root = root_node(d);
        std::string err;
        std::vector<Edit> edits;
        try {
            edits = edits_for(d, root, op, err);
        } catch (const PatchError& e) {
            err = e.what();
        }
        if (!err.empty()) throw PatchError(where + ": " + err);
        text = splice(std::move(d.text), std::move(edits));
    }
    return text;
}

// --- diff -----------------------------------------------------------------------------------

namespace {

std::string header_text(std::string_view t, const Node& n) {
    if (n.tokens.empty()) return "";
    return std::string(t.substr(n.tokens.front().begin, n.tokens.back().end - n.tokens.front().begin));
}

// A selector segment naming `n` among `siblings` and nothing else.
std::string name_segment(std::string_view t, const Node& n, const std::vector<Node>& siblings) {
    std::string head = header_text(t, n);
    if (head.empty()) throw PatchError("a block without a header (bare {) at line " + std::to_string(line_of(t, n.begin)) + " can't be named");
    auto count = [&](const std::string& seg) {
        Segment s = parse_selector(seg)[0];
        std::size_t k = 0;
        for (const auto& o : siblings)
            if (matches(t, o, s)) ++k;
        return k;
    };
    if (count(head) == 1) return head;
    for (const auto& c : n.children) {
        if (c.block || c.tokens.size() < 2) continue;
        std::string v = c.value(t);
        if (v.find('"') != std::string::npos || v.find(']') != std::string::npos) continue;
        std::string seg = head + "[" + c.key() + "=\"" + v + "\"]";
        if (count(seg) == 1) return seg;
    }
    throw PatchError("block \"" + head + "\" at line " + std::to_string(line_of(t, n.begin)) +
                     " has no key that tells it apart from its siblings");
}

struct Differ {
    std::string_view a, b;  // base, modded
    std::string out;

    void block(const std::vector<Node>& an, const std::vector<Node>& bn, const std::string& sel,
               const std::string& where) {
        std::vector<const Node*> ab, bb;
        std::map<std::string, std::vector<const Node*>> al, bl;
        std::vector<std::string> bkeys;  // modded key order
        for (const auto& n : an) {
            if (n.block) ab.push_back(&n);
            else al[lower(n.key())].push_back(&n);
        }
        for (const auto& n : bn) {
            if (n.block) {
                bb.push_back(&n);
            } else {
                std::string k = lower(n.key());
                if (!bl.count(k)) bkeys.push_back(k);
                bl[k].push_back(&n);
            }
        }
        std::string sets, unsets;
        for (const auto& k : bkeys) {
            const auto& bv = bl[k];
            auto it = al.find(k);
            if (it == al.end()) {
                if (bv.size() > 1) throw PatchError(where + ": key " + bv[0]->key() + " added more than once");
                sets += "\t\t" + std::string(b.substr(bv[0]->begin, bv[0]->end - bv[0]->begin)) + "\n";
                continue;
            }
            const auto& av = it->second;
            bool same = av.size() == bv.size();
            for (std::size_t i = 0; same && i < av.size(); ++i) same = av[i]->raw_value(a) == bv[i]->raw_value(b);
            if (same) continue;
            if (av.size() != 1 || bv.size() != 1)
                throw PatchError(where + ": key " + bv[0]->key() + " appears more than once and changed");
            if (bv[0]->tokens.size() < 2)
                throw PatchError(where + ": key " + bv[0]->key() + " lost its value");
            sets += "\t\t" + std::string(b.substr(bv[0]->begin, bv[0]->end - bv[0]->begin)) + "\n";
        }
        for (const auto& [k, av] : al)
            if (!bl.count(k)) unsets += "\t\t" + av[0]->key() + "\n";
        if (!sets.empty()) out += "\tset " + sel + "\n\t{\n" + sets + "\t}\n";
        if (!unsets.empty()) out += "\tunset " + sel + "\n\t{\n" + unsets + "\t}\n";

        if (ab.size() != bb.size())
            throw PatchError(where + ": blocks were added or removed (" + std::to_string(ab.size()) + " -> " +
                             std::to_string(bb.size()) + "); write those operations by hand");
        for (std::size_t i = 0; i < ab.size(); ++i) {
            if (header_text(a, *ab[i]) != header_text(b, *bb[i]))
                throw PatchError(where + ": block \"" + header_text(a, *ab[i]) + "\" became \"" +
                                 header_text(b, *bb[i]) + "\"; write that by hand");
            std::string seg = name_segment(a, *ab[i], an);
            std::string child_sel = sel == "." ? seg : sel + "/" + seg;
            block(ab[i]->children, bb[i]->children, child_sel,
                  "line " + std::to_string(line_of(a, ab[i]->begin)) + " (" + child_sel + ")");
        }
    }

    static std::string lower(std::string s) {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return s;
    }
};

}  // namespace

std::string diff(std::string_view base, std::string_view modded, const std::string& file) {
    Doc a, b;
    try {
        a = parse(std::string(base));
        b = parse(std::string(modded));
    } catch (const ParseError& e) {
        throw PatchError(file + " line " + std::to_string(e.line) + ": " + e.what());
    }
    Differ d{a.text, b.text, ""};
    d.block(a.top, b.top, ".", file);
    if (d.out.empty()) return "";
    return "patch \"" + file + "\"\n{\n" + d.out + "}\n";
}

}  // namespace sf::mod

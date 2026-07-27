#pragma once
// Minimal s-expression parser for KiCad file formats (.kicad_pcb v6+).
// A node is either an atom (bare token or "quoted string") or a list whose
// first child is conventionally the node's name atom: (segment (start 1 2) ...).
// No fallbacks: malformed input throws SExprError with position context.

#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace faraday {

struct SExprError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class SExpr {
  public:
    // Atom constructor
    explicit SExpr(std::string atom) : atom_(std::move(atom)), is_atom_(true) {}
    // List constructor
    SExpr() = default;

    bool is_atom() const { return is_atom_; }
    bool is_list() const { return !is_atom_; }

    const std::string& atom() const {
        if (!is_atom_) throw SExprError("SExpr: atom() on a list node");
        return atom_;
    }

    const std::vector<SExpr>& children() const {
        if (is_atom_) throw SExprError("SExpr: children() on an atom node");
        return children_;
    }
    std::vector<SExpr>& children() {
        if (is_atom_) throw SExprError("SExpr: children() on an atom node");
        return children_;
    }

    // Name of a list node: its first child when that child is an atom, else "".
    std::string_view name() const {
        if (is_atom_ || children_.empty() || !children_[0].is_atom_) return {};
        return children_[0].atom_;
    }

    // First direct child list named `n`, or nullptr.
    const SExpr* find(std::string_view n) const {
        if (is_atom_) return nullptr;
        for (const auto& c : children_)
            if (c.is_list() && c.name() == n) return &c;
        return nullptr;
    }

    // All direct child lists named `n`.
    std::vector<const SExpr*> find_all(std::string_view n) const {
        std::vector<const SExpr*> out;
        if (is_atom_) return out;
        for (const auto& c : children_)
            if (c.is_list() && c.name() == n) out.push_back(&c);
        return out;
    }

    // Positional atom accessor: child at index i (0 is the name atom).
    const std::string& atom_at(size_t i) const {
        const auto& cs = children();
        if (i >= cs.size() || !cs[i].is_atom_)
            throw SExprError("SExpr: no atom at index " + std::to_string(i) +
                             " in (" + std::string(name()) + " ...)");
        return cs[i].atom_;
    }

    double number_at(size_t i) const { return to_number(atom_at(i)); }

    // (name value) convenience: value atom of the first child list named n.
    // Throws when the child is absent — callers that allow absence use find().
    const std::string& value_of(std::string_view n) const {
        const SExpr* c = find(n);
        if (!c) throw SExprError("SExpr: missing required child (" + std::string(n) +
                                 " ...) in (" + std::string(name()) + " ...)");
        return c->atom_at(1);
    }

    double number_of(std::string_view n) const { return to_number(value_of(n)); }

    // strtod, not std::from_chars<double>: emscripten's libc++ has no
    // floating-point from_chars. Same strictness — the WHOLE atom must parse.
    static double to_number(const std::string& s) {
        if (s.empty()) throw SExprError("SExpr: not a number: ''");
        char* end = nullptr;
        double v = std::strtod(s.c_str(), &end);
        if (end != s.c_str() + s.size())
            throw SExprError("SExpr: not a number: '" + s + "'");
        return v;
    }

    // ---- Parsing ----
    static SExpr parse(std::string_view text) {
        size_t pos = 0, line = 1;
        skip_ws(text, pos, line);
        if (pos >= text.size() || text[pos] != '(')
            throw SExprError("SExpr: input does not start with '('");
        SExpr root = parse_list(text, pos, line);
        skip_ws(text, pos, line);
        if (pos != text.size())
            throw SExprError("SExpr: trailing content at line " + std::to_string(line));
        return root;
    }

  private:
    static void skip_ws(std::string_view t, size_t& pos, size_t& line) {
        while (pos < t.size()) {
            char c = t[pos];
            if (c == '\n') { ++line; ++pos; }
            else if (std::isspace(static_cast<unsigned char>(c))) ++pos;
            else break;
        }
    }

    static SExpr parse_list(std::string_view t, size_t& pos, size_t& line) {
        ++pos;  // consume '('
        SExpr node;
        while (true) {
            skip_ws(t, pos, line);
            if (pos >= t.size())
                throw SExprError("SExpr: unterminated list at line " + std::to_string(line));
            char c = t[pos];
            if (c == ')') { ++pos; return node; }
            if (c == '(') { node.children_.push_back(parse_list(t, pos, line)); continue; }
            node.children_.push_back(parse_atom(t, pos, line));
        }
    }

    static SExpr parse_atom(std::string_view t, size_t& pos, size_t& line) {
        if (t[pos] == '"') {
            std::string out;
            ++pos;
            while (pos < t.size() && t[pos] != '"') {
                if (t[pos] == '\\' && pos + 1 < t.size()) {
                    // KiCad escapes: \" \\ \n \t — keep others verbatim
                    char n = t[pos + 1];
                    if (n == '"' || n == '\\') { out.push_back(n); pos += 2; continue; }
                    if (n == 'n') { out.push_back('\n'); pos += 2; continue; }
                    if (n == 't') { out.push_back('\t'); pos += 2; continue; }
                }
                if (t[pos] == '\n') ++line;
                out.push_back(t[pos++]);
            }
            if (pos >= t.size())
                throw SExprError("SExpr: unterminated string at line " + std::to_string(line));
            ++pos;  // closing quote
            return SExpr(std::move(out));
        }
        size_t start = pos;
        while (pos < t.size()) {
            char c = t[pos];
            if (c == '(' || c == ')' || c == '"' ||
                std::isspace(static_cast<unsigned char>(c))) break;
            ++pos;
        }
        return SExpr(std::string(t.substr(start, pos - start)));
    }

    std::string atom_;
    std::vector<SExpr> children_;
    bool is_atom_ = false;
};

}  // namespace faraday

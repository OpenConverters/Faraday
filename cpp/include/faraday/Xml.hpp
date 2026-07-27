#pragma once
// Minimal XML reader for the IPC-2581 importer. Dependency-free so it builds
// for WASM alongside everything else. Handles elements, attributes,
// self-closing tags, comments, CDATA, the XML declaration and DOCTYPE, and
// namespace prefixes (which are stripped — IPC-2581 files vary in whether they
// use one). Malformed input throws; nothing is silently skipped.

#include <cctype>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace faraday {

struct XmlError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct XmlNode {
    std::string name;                             // namespace prefix stripped
    std::map<std::string, std::string> attrs;
    std::vector<XmlNode> children;
    std::string text;

    bool has(const std::string& k) const { return attrs.count(k) != 0; }
    const std::string& attr(const std::string& k) const {
        auto it = attrs.find(k);
        if (it == attrs.end())
            throw XmlError("xml: <" + name + "> has no attribute '" + k + "'");
        return it->second;
    }
    std::string attr_or(const std::string& k, const std::string& d) const {
        auto it = attrs.find(k);
        return it == attrs.end() ? d : it->second;
    }
    double num(const std::string& k) const {
        const std::string& s = attr(k);
        char* end = nullptr;
        double v = std::strtod(s.c_str(), &end);
        if (end != s.c_str() + s.size())
            throw XmlError("xml: <" + name + "> " + k + "='" + s +
                           "' is not a number");
        return v;
    }
    double num_or(const std::string& k, double d) const {
        return has(k) ? num(k) : d;
    }
    const XmlNode* first(std::string_view n) const {
        for (const auto& c : children)
            if (c.name == n) return &c;
        return nullptr;
    }
    std::vector<const XmlNode*> all(std::string_view n) const {
        std::vector<const XmlNode*> out;
        for (const auto& c : children)
            if (c.name == n) out.push_back(&c);
        return out;
    }
    // depth-first search for every descendant with this name
    void collect(std::string_view n, std::vector<const XmlNode*>& out) const {
        for (const auto& c : children) {
            if (c.name == n) out.push_back(&c);
            c.collect(n, out);
        }
    }
    std::vector<const XmlNode*> descendants(std::string_view n) const {
        std::vector<const XmlNode*> out;
        collect(n, out);
        return out;
    }
};

class XmlParser {
  public:
    explicit XmlParser(std::string_view t) : t_(t) {}

    XmlNode parse() {
        skip_prolog();
        if (pos_ >= t_.size() || t_[pos_] != '<')
            throw XmlError("xml: no root element");
        XmlNode root = parse_element();
        return root;
    }

  private:
    void ws() {
        while (pos_ < t_.size() &&
               std::isspace(static_cast<unsigned char>(t_[pos_]))) ++pos_;
    }
    bool starts(std::string_view s) const { return t_.compare(pos_, s.size(), s) == 0; }
    void expect_skip(std::string_view open, std::string_view close) {
        pos_ += open.size();
        size_t e = t_.find(close, pos_);
        if (e == std::string_view::npos)
            throw XmlError("xml: unterminated " + std::string(open));
        pos_ = e + close.size();
    }
    void skip_prolog() {
        while (true) {
            ws();
            if (pos_ >= t_.size()) return;
            if (starts("<?")) { expect_skip("<?", "?>"); continue; }
            if (starts("<!--")) { expect_skip("<!--", "-->"); continue; }
            if (starts("<!")) { expect_skip("<!", ">"); continue; }
            return;
        }
    }

    static std::string strip_ns(std::string n) {
        size_t c = n.find(':');
        return c == std::string::npos ? n : n.substr(c + 1);
    }

    std::string read_name() {
        size_t start = pos_;
        while (pos_ < t_.size()) {
            char c = t_[pos_];
            if (std::isspace(static_cast<unsigned char>(c)) || c == '>' ||
                c == '/' || c == '=') break;
            ++pos_;
        }
        if (pos_ == start) throw XmlError("xml: empty name");
        return strip_ns(std::string(t_.substr(start, pos_ - start)));
    }

    static std::string unescape(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '&') { out.push_back(s[i]); continue; }
            size_t sc = s.find(';', i);
            if (sc == std::string_view::npos) { out.push_back('&'); continue; }
            std::string_view e = s.substr(i + 1, sc - i - 1);
            if (e == "amp") out.push_back('&');
            else if (e == "lt") out.push_back('<');
            else if (e == "gt") out.push_back('>');
            else if (e == "quot") out.push_back('"');
            else if (e == "apos") out.push_back('\'');
            else if (!e.empty() && e[0] == '#') {
                int code = (e.size() > 1 && (e[1] == 'x' || e[1] == 'X'))
                    ? (int)std::strtol(std::string(e.substr(2)).c_str(), nullptr, 16)
                    : (int)std::strtol(std::string(e.substr(1)).c_str(), nullptr, 10);
                if (code < 128) out.push_back((char)code);
            } else { out.push_back('&'); continue; }
            i = sc;
        }
        return out;
    }

    XmlNode parse_element() {
        ++pos_;  // '<'
        XmlNode node;
        node.name = read_name();
        // attributes
        while (true) {
            ws();
            if (pos_ >= t_.size()) throw XmlError("xml: unterminated <" + node.name + ">");
            if (starts("/>")) { pos_ += 2; return node; }
            if (t_[pos_] == '>') { ++pos_; break; }
            std::string key = read_name();
            ws();
            if (pos_ >= t_.size() || t_[pos_] != '=')
                throw XmlError("xml: attribute '" + key + "' of <" + node.name +
                               "> has no value");
            ++pos_;
            ws();
            if (pos_ >= t_.size() || (t_[pos_] != '"' && t_[pos_] != '\''))
                throw XmlError("xml: unquoted value for '" + key + "'");
            char q = t_[pos_++];
            size_t start = pos_;
            while (pos_ < t_.size() && t_[pos_] != q) ++pos_;
            if (pos_ >= t_.size()) throw XmlError("xml: unterminated attribute value");
            node.attrs[key] = unescape(t_.substr(start, pos_ - start));
            ++pos_;
        }
        // content
        std::string text;
        while (true) {
            if (pos_ >= t_.size())
                throw XmlError("xml: unclosed <" + node.name + ">");
            if (starts("<!--")) { expect_skip("<!--", "-->"); continue; }
            if (starts("<![CDATA[")) {
                pos_ += 9;
                size_t e = t_.find("]]>", pos_);
                if (e == std::string_view::npos) throw XmlError("xml: unterminated CDATA");
                text.append(t_.substr(pos_, e - pos_));
                pos_ = e + 3;
                continue;
            }
            if (starts("</")) {
                pos_ += 2;
                std::string close = read_name();
                if (close != node.name)
                    throw XmlError("xml: </" + close + "> closes <" + node.name + ">");
                ws();
                if (pos_ >= t_.size() || t_[pos_] != '>')
                    throw XmlError("xml: malformed closing tag for " + node.name);
                ++pos_;
                node.text = unescape(text);
                return node;
            }
            if (t_[pos_] == '<') { node.children.push_back(parse_element()); continue; }
            size_t start = pos_;
            while (pos_ < t_.size() && t_[pos_] != '<') ++pos_;
            text.append(t_.substr(start, pos_ - start));
        }
    }

    std::string_view t_;
    size_t pos_ = 0;
};

inline XmlNode parse_xml(std::string_view text) { return XmlParser(text).parse(); }

}  // namespace faraday

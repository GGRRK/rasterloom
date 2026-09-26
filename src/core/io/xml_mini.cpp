// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/io/xml_mini.hpp"

#include <cstdint>

#include "core/io/io_error.hpp"

namespace rl::io {

namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    int depth = 0;

    [[noreturn]] void fail(const std::string& why) const {
        throw IoError("stack.xml: " + why + " at byte " + std::to_string(i));
    }
    bool starts(const char* p) const { return s.compare(i, std::char_traits<char>::length(p), p) == 0; }
    void skip_ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    void skip_until(const char* end) {
        const size_t p = s.find(end, i);
        if (p == std::string::npos) fail(std::string("unterminated construct, expected '") + end + "'");
        i = p + std::char_traits<char>::length(end);
    }
    // Skips text, comments, PIs, DOCTYPE and CDATA until the next element tag or end tag.
    void skip_misc() {
        for (;;) {
            while (i < s.size() && s[i] != '<') ++i;
            if (i >= s.size()) return;
            if (starts("<?")) {
                skip_until("?>");
            } else if (starts("<!--")) {
                skip_until("-->");
            } else if (starts("<![CDATA[")) {
                skip_until("]]>");
            } else if (starts("<!")) {
                skip_until(">");
            } else {
                return;
            }
        }
    }
    static bool name_char(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == ':' ||
               c == '-' || c == '.' || static_cast<unsigned char>(c) >= 0x80;
    }
    std::string name() {
        const size_t b = i;
        while (i < s.size() && name_char(s[i])) ++i;
        if (i == b) fail("expected a name");
        return s.substr(b, i - b);
    }
    static void put_utf8(std::string& o, uint32_t cp) {
        if (cp < 0x80) {
            o.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            o.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            o.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            o.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            o.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    std::string unescape(const std::string& v) {
        std::string o;
        for (size_t k = 0; k < v.size(); ++k) {
            if (v[k] != '&') {
                o.push_back(v[k]);
                continue;
            }
            const size_t semi = v.find(';', k);
            if (semi == std::string::npos) fail("bad entity");
            const std::string e = v.substr(k + 1, semi - k - 1);
            if (e == "amp") o.push_back('&');
            else if (e == "lt") o.push_back('<');
            else if (e == "gt") o.push_back('>');
            else if (e == "quot") o.push_back('"');
            else if (e == "apos") o.push_back('\'');
            else if (!e.empty() && e[0] == '#') {
                uint32_t cp = 0;
                try {
                    cp = static_cast<uint32_t>(e.size() > 1 && (e[1] == 'x' || e[1] == 'X') ? std::stoul(e.substr(2), nullptr, 16)
                                                                                          : std::stoul(e.substr(1)));
                } catch (const std::exception&) {
                    fail("bad character reference");
                }
                if (cp > 0x10FFFF) fail("bad character reference");
                put_utf8(o, cp);
            } else {
                fail("unknown entity &" + e + ";");
            }
            k = semi;
        }
        return o;
    }
    XmlElement element() {
        if (++depth > 256) fail("nesting too deep");
        if (i >= s.size() || s[i] != '<') fail("expected '<'");
        ++i;
        XmlElement el;
        el.name = name();
        for (;;) {
            skip_ws();
            if (i >= s.size()) fail("unterminated tag");
            if (starts("/>")) {
                i += 2;
                --depth;
                return el;
            }
            if (s[i] == '>') {
                ++i;
                break;
            }
            std::string k = name();
            skip_ws();
            if (i >= s.size() || s[i] != '=') fail("expected '='");
            ++i;
            skip_ws();
            if (i >= s.size() || (s[i] != '"' && s[i] != '\'')) fail("expected a quoted value");
            const char qch = s[i++];
            const size_t e = s.find(qch, i);
            if (e == std::string::npos) fail("unterminated attribute value");
            el.attrs.emplace_back(std::move(k), unescape(s.substr(i, e - i)));
            i = e + 1;
        }
        for (;;) {
            skip_misc();
            if (i >= s.size()) fail("unterminated element <" + el.name + ">");
            if (starts("</")) {
                i += 2;
                const std::string n = name();
                if (n != el.name) fail("mismatched end tag </" + n + "> for <" + el.name + ">");
                skip_ws();
                if (i >= s.size() || s[i] != '>') fail("expected '>'");
                ++i;
                --depth;
                return el;
            }
            el.children.push_back(element());
        }
    }
};

}  // namespace

XmlElement parse_xml(const std::string& text) {
    Parser p{text};
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        p.i = 3;
    p.skip_misc();
    return p.element();
}

std::string xml_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default: o.push_back(c);
        }
    }
    return o;
}

}  // namespace rl::io

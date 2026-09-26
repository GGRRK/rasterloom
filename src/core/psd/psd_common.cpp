// SPDX-License-Identifier: GPL-3.0-or-later
//
// Blend keys, key classification, text conversion and the adjustment-block mapping.
// Adjustment block layouts are from the Adobe PSD specification ("Adjustment Layers": Levels,
// Curves, Posterize, Threshold, Invert); psd-tools (MIT) psd/adjustments.py was read as a layout
// cross-check for Curves (version 1 channel bitmap + 'Crv ' extra) and Levels ('Lvls' extra).
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstring>

#include "core/io/bytes.hpp"
#include "core/psd/psd.hpp"
#include "core/psd/psd_internal.hpp"

namespace rl::psd {

namespace {
struct KeyMode {
    const char* key;
    BlendMode mode;
};
constexpr KeyMode kModes[] = {
    {"pass", BlendMode::Pass}, {"norm", BlendMode::Norm}, {"diss", BlendMode::Diss}, {"dark", BlendMode::Dark},
    {"mul ", BlendMode::Mul},  {"idiv", BlendMode::Idiv}, {"lbrn", BlendMode::Lbrn}, {"dkCl", BlendMode::DkCl},
    {"lite", BlendMode::Lite}, {"scrn", BlendMode::Scrn}, {"div ", BlendMode::Div},  {"lddg", BlendMode::Lddg},
    {"lgCl", BlendMode::LgCl}, {"over", BlendMode::Over}, {"sLit", BlendMode::SLit}, {"hLit", BlendMode::HLit},
    {"vLit", BlendMode::VLit}, {"lLit", BlendMode::LLit}, {"pLit", BlendMode::PLit}, {"hMix", BlendMode::HMix},
    {"diff", BlendMode::Diff}, {"smud", BlendMode::Smud}, {"fsub", BlendMode::Fsub}, {"fdiv", BlendMode::Fdiv},
    {"hue ", BlendMode::Hue},  {"sat ", BlendMode::Sat},  {"colr", BlendMode::Colr}, {"lum ", BlendMode::Lum},
};
}  // namespace

const char* blend_key(BlendMode m) {
    for (const KeyMode& k : kModes)
        if (k.mode == m) return k.key;
    return "norm";
}

std::optional<BlendMode> mode_from_key(const std::string& key) {
    for (const KeyMode& k : kModes)
        if (key == k.key) return k.mode;
    return std::nullopt;
}

bool is_psd(const std::vector<uint8_t>& b) {
    return b.size() >= 4 && std::memcmp(b.data(), "8BPS", 4) == 0;
}

namespace detail {

namespace {
bool in(const std::string& k, std::initializer_list<const char*> keys) {
    for (const char* s : keys)
        if (k == s) return true;
    return false;
}
}  // namespace

bool is_big_key(const std::string& k) {
    return in(k, {"Alph", "FELS", "FEid", "FMsk", "FXid", "LMsk", "Layr", "Lr16", "Lr32", "Mt16", "Mt32", "Mtrn",
                  "PxSD", "artd", "cinf", "extd", "extn", "lnk2", "lnk3", "lnkE", "pths"});
}

bool is_owned_layer_key(const std::string& k) { return in(k, {"luni", "lsct", "lsdk", "iOpa", "lspf", "clbl"}); }

bool is_consumed_global_key(const std::string& k) { return in(k, {"Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn"}); }

bool is_adjustment_key(const std::string& k) {
    return in(k, {"levl", "curv", "brit", "hue ", "hue2", "blnc", "blwh", "phfl", "mixr", "clrL", "nvrt", "post",
                  "thrs", "grdm", "selc", "expA", "vibA"});
}

std::vector<uint16_t> utf8_to_utf16(const std::string& s) {
    std::vector<uint16_t> out;
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0xFFFD;
        size_t len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c >> 5) == 6 && i + 1 < s.size()) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            len = 2;
        } else if ((c >> 4) == 14 && i + 2 < s.size()) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            len = 3;
        } else if ((c >> 3) == 30 && i + 3 < s.size()) {
            cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            len = 4;
        }
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<uint16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<uint16_t>(cp));
        }
    }
    return out;
}

namespace {
void put_utf8(std::string& o, uint32_t cp) {
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
}  // namespace

std::string utf16_to_utf8(const std::vector<uint16_t>& u) {
    std::string o;
    for (size_t i = 0; i < u.size(); ++i) {
        uint32_t cp = u[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < u.size() && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00u);
            ++i;
        } else if (cp >= 0xD800 && cp < 0xE000) {
            cp = 0xFFFD;
        }
        put_utf8(o, cp);
    }
    return o;
}

std::string latin1_to_utf8(const std::string& s) {
    std::string o;
    for (unsigned char c : s) put_utf8(o, c);
    return o;
}

std::string utf8_to_latin1(const std::string& s) {
    std::string o;
    for (uint16_t u : utf8_to_utf16(s)) o.push_back(u < 0x100 ? static_cast<char>(u) : '?');
    return o;
}

// ---- adjustments ------------------------------------------------------------------------------

namespace {

using nlohmann::json;
const char* kChan[4] = {"rgb", "r", "g", "b"};

bool levels_from(const std::vector<uint8_t>& d, json& params) {
    io::ByteReader r(d, "PSD levl");
    if (r.left() < 2 + (29 * 10) || r.be16() != 2) return false;
    params = json::object();
    for (int i = 0; i < 29; ++i) {
        const int ib = r.be16(), iw = r.be16(), ob = r.be16(), ow = r.be16(), g = r.be16();
        const bool identity = ib == 0 && iw == 255 && ob == 0 && ow == 255 && g == 100;
        if (i >= 4) continue;  // records 4..28: extra channels; not part of an RGB document
        if (identity) continue;
        if (ib > 254 || iw < 1 || iw > 255 || ib >= iw || ob > 255 || ow > 255 || g < 10 || g > 999) return false;
        params[kChan[i]] = json{{"in_black", ib}, {"in_white", iw}, {"gamma", g / 100.0}, {"out_black", ob},
                                {"out_white", ow}};
    }
    return true;
}

bool curves_points_ok(const json& pts) {
    if (pts.size() < 2 || pts.size() > 16) return false;
    int prev = -1;
    for (const auto& p : pts) {
        const int x = p[0].get<int>(), y = p[1].get<int>();
        if (x < 0 || x > 255 || y < 0 || y > 255 || x <= prev) return false;
        prev = x;
    }
    return true;
}

bool curves_from(const std::vector<uint8_t>& d, json& params) {
    io::ByteReader r(d, "PSD curv");
    if (r.left() < 7) return false;
    const uint8_t is_map = r.u8();
    const uint16_t version = r.be16();
    const uint32_t count_map = r.be32();
    if (is_map || version != 1) return false;  // version 4 and lookup-table curves: not representable
    params = json::object();
    for (int bit = 0; bit < 32; ++bit) {
        if (!(count_map & (1u << bit))) continue;
        const uint16_t n = r.be16();
        if (n < 2 || n > 19) return false;
        json pts = json::array();
        for (int i = 0; i < n; ++i) {
            const int out = r.be16(), in = r.be16();
            pts.push_back(json::array({in, out}));
        }
        if (bit > 3) continue;  // extra channels
        const bool identity = pts.size() == 2 && pts[0] == json::array({0, 0}) && pts[1] == json::array({255, 255});
        if (identity) continue;
        if (!curves_points_ok(pts)) return false;
        params[kChan[bit]] = pts;
    }
    return true;
}

}  // namespace

bool adjustment_from_psd(const std::string& key, const std::vector<uint8_t>& data, std::string& type,
                         std::string& params_json) {
    try {
        if (key == "nvrt") {
            type = "invert";
            params_json.clear();
            return true;
        }
        if (key == "post" || key == "thrs") {
            if (data.size() < 2) return false;
            const int v = (data[0] << 8) | data[1];
            if (key == "post") {
                if (v < 2 || v > 255) return false;
                type = "posterize";
                params_json = json{{"levels", v}}.dump();
            } else {
                if (v < 1 || v > 255) return false;
                type = "threshold";
                params_json = json{{"level", v}}.dump();
            }
            return true;
        }
        json p;
        if (key == "levl" && levels_from(data, p)) {
            type = "levels";
            params_json = p.dump();
            return true;
        }
        if (key == "curv" && curves_from(data, p)) {
            type = "curves";
            params_json = p.dump();
            return true;
        }
    } catch (const std::exception&) {
        return false;
    }
    // brit / hue2 / blwh: doc 20's slider mappings onto Photoshop's are undecided (doc 20 Part E
    // notes 3-5), so their settings are not translated; the block is kept and written back.
    return false;
}

bool adjustment_to_psd(const std::string& type, const std::string& params_json, std::string& key,
                       std::vector<uint8_t>& data) {
    json p = json::object();
    try {
        if (!params_json.empty()) p = json::parse(params_json);
    } catch (const std::exception&) {
        return false;
    }
    io::ByteWriter w;
    try {
        if (type == "invert") {
            key = "nvrt";
        } else if (type == "posterize" || type == "threshold") {
            const bool post = type == "posterize";
            key = post ? "post" : "thrs";
            const int v = p.value(post ? "levels" : "level", post ? 4 : 128);
            w.be16(static_cast<uint16_t>(v));
            w.be16(0);
        } else if (type == "levels") {
            key = "levl";
            w.be16(2);
            for (int i = 0; i < 29; ++i) {
                json s = (i < 4 && p.contains(kChan[i])) ? p[kChan[i]] : json::object();
                w.be16(static_cast<uint16_t>(s.value("in_black", 0)));
                w.be16(static_cast<uint16_t>(s.value("in_white", 255)));
                w.be16(static_cast<uint16_t>(s.value("out_black", 0)));
                w.be16(static_cast<uint16_t>(s.value("out_white", 255)));
                const double g = s.value("gamma", 1.0);
                w.be16(static_cast<uint16_t>(std::lround(g * 100.0)));
            }
        } else if (type == "curves") {
            key = "curv";
            uint32_t bits = 0;
            for (int i = 0; i < 4; ++i)
                if (p.contains(kChan[i])) bits |= 1u << i;
            w.u8(0);
            w.be16(1);
            w.be32(bits);
            for (int i = 0; i < 4; ++i) {
                if (!(bits & (1u << i))) continue;
                const json& pts = p[kChan[i]];
                w.be16(static_cast<uint16_t>(pts.size()));
                for (const auto& pt : pts) {
                    w.be16(static_cast<uint16_t>(pt[1].get<int>()));  // output
                    w.be16(static_cast<uint16_t>(pt[0].get<int>()));  // input
                }
            }
            // 'Crv ' extra section (version 4): channel id + points per curve.
            w.str("Crv ");
            w.be16(4);
            uint32_t n = 0;
            for (int i = 0; i < 4; ++i) n += (bits >> i) & 1u;
            w.be32(n);
            for (int i = 0; i < 4; ++i) {
                if (!(bits & (1u << i))) continue;
                const json& pts = p[kChan[i]];
                w.be16(static_cast<uint16_t>(i));
                w.be16(static_cast<uint16_t>(pts.size()));
                for (const auto& pt : pts) {
                    w.be16(static_cast<uint16_t>(pt[1].get<int>()));
                    w.be16(static_cast<uint16_t>(pt[0].get<int>()));
                }
            }
        } else {
            return false;  // brightness_contrast, hue_saturation, black_white: mapping undecided
        }
    } catch (const std::exception&) {
        return false;
    }
    w.pad_to(4);
    data = std::move(w.buf);
    return true;
}

}  // namespace detail
}  // namespace rl::psd

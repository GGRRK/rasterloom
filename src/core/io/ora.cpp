// SPDX-License-Identifier: GPL-3.0-or-later
//
// OpenRaster reader/writer, from the OpenRaster 0.0.6 specification (file layout, layer stack,
// extensions) and the decisions in docs/research/psd-and-ora.md section 3.
#include "core/io/ora.hpp"

#include <nlohmann/json.hpp>

#include <cstring>
#include <map>
#include <string_view>

#include "core/base/quant.hpp"
#include "core/io/image_util.hpp"
#include "core/io/io_error.hpp"
#include "core/io/layer_name.hpp"
#include "core/io/render_rows.hpp"
#include "core/io/sha256.hpp"
#include "core/io/unrendered_adjustment.hpp"
#include "core/io/xml_mini.hpp"
#include "core/io/zip.hpp"
#include "core/psd/psd.hpp"

namespace rl::ora {

namespace {

using io::IoError;
using nlohmann::json;

constexpr const char* kMime = "image/openraster";
constexpr const char* kNs = "https://rasterloom.invalid/ns/1";
constexpr int kFormatVersion = 1;
constexpr int kMaxSide = psd::kPsbMaxSide;

struct OpName {
    BlendMode mode;
    const char* op;
};
constexpr OpName kOps[] = {
    {BlendMode::Norm, "svg:src-over"},        {BlendMode::Diss, "krita:dissolve"},
    {BlendMode::Dark, "svg:darken"},          {BlendMode::Mul, "svg:multiply"},
    {BlendMode::Idiv, "svg:color-burn"},      {BlendMode::Lbrn, "krita:linear_burn"},
    {BlendMode::DkCl, "krita:darker color"},  {BlendMode::Lite, "svg:lighten"},
    {BlendMode::Scrn, "svg:screen"},          {BlendMode::Div, "svg:color-dodge"},
    {BlendMode::Lddg, "krita:linear_dodge"},  {BlendMode::LgCl, "krita:lighter color"},
    {BlendMode::Over, "svg:overlay"},         {BlendMode::SLit, "svg:soft-light"},
    {BlendMode::HLit, "svg:hard-light"},      {BlendMode::VLit, "krita:vivid_light"},
    {BlendMode::LLit, "krita:linear light"},  {BlendMode::PLit, "krita:pin_light"},
    {BlendMode::HMix, "krita:hard_mix_photoshop"}, {BlendMode::Diff, "svg:difference"},
    {BlendMode::Smud, "krita:exclusion"},     {BlendMode::Fsub, "krita:subtract"},
    {BlendMode::Fdiv, "krita:divide"},        {BlendMode::Hue, "svg:hue"},
    {BlendMode::Sat, "svg:saturation"},       {BlendMode::Colr, "svg:color"},
    {BlendMode::Lum, "svg:luminosity"},
};

// Reading: our own ids plus the approximations of research 3.3.
std::optional<BlendMode> mode_of_op(const std::string& op) {
    for (const OpName& o : kOps)
        if (op == o.op) return o.mode;
    if (op == "svg:plus" || op == "krita:add") return BlendMode::Lddg;
    if (op == "krita:soft_light" || op == "krita:soft_light_svg") return BlendMode::SLit;
    if (op == "krita:hard mix") return BlendMode::HMix;
    if (op == "svg:exclusion") return BlendMode::Smud;
    if (op == "krita:normal") return BlendMode::Norm;
    return std::nullopt;
}

std::string num(double v) { return json(v).dump(); }

std::string hex_color(Rgba8 c) {
    char b[16];
    std::snprintf(b, sizeof b, "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
    return b;
}

Rgba8 parse_hex(const std::string& s) {
    unsigned r = 0, g = 0, b = 0, a = 0;
    if (s.size() != 9 || s[0] != '#' || std::sscanf(s.c_str() + 1, "%2x%2x%2x%2x", &r, &g, &b, &a) != 4)
        throw IoError("document.json: bad colour '" + s + "'");
    return canonicalize(Rgba8{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b), static_cast<uint8_t>(a)});
}

const char* kind_name(NodeKind k) {
    switch (k) {
        case NodeKind::Raster: return "raster";
        case NodeKind::Adjustment: return "adjustment";
        case NodeKind::Group: return "group";
    }
    return "raster";
}

// ---- writer -------------------------------------------------------------------------------------

struct Writer {
    const DocState& s;
    std::vector<std::string>& warnings;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> data_files;   // data/...
    std::vector<std::pair<std::string, std::vector<uint8_t>>> priv_files;   // rasterloom/...
    int layer_no = 0;
    int priv_no = 0;
    std::string xml;

    std::vector<uint8_t> png_of(const std::vector<Rgba8>& px, int w, int h) {
        io::RgbaBuffer b;
        b.w = w;
        b.h = h;
        b.px = px;
        return io::encode_png(b);
    }

    // PNG of a raster image's content bbox (a 1x1 transparent PNG when empty).
    std::vector<uint8_t> rgba_png(const RgbaImage& img, io::IRect& bb) {
        bb = io::content_bbox(img);
        if (bb.empty()) {
            bb = io::IRect{0, 0, 1, 1};
            return png_of({Rgba8{}}, 1, 1);
        }
        return png_of(io::read_region(img, bb), bb.w, bb.h);
    }

    std::string add_data_png(std::vector<uint8_t> png) {
        const std::string name = "data/layer" + std::to_string(++layer_no) + ".png";
        data_files.emplace_back(name, std::move(png));
        return name;
    }
    std::string add_priv(const std::string& dir, const std::string& ext, std::vector<uint8_t> bytes) {
        const std::string name = "rasterloom/" + dir + "/" + std::to_string(++priv_no) + ext;
        priv_files.emplace_back(name, std::move(bytes));
        return name;
    }

    json foreign_json(const ForeignData& f) {
        json arr = json::array();
        for (const ForeignBlock& b : f.blocks)
            arr.push_back(json{{"ns", b.ns}, {"key", b.key}, {"src", add_priv("foreign", ".bin", b.bytes())}});
        return arr;
    }

    json mask_json(const LayerMask& m) {
        json j{{"enabled", m.enabled}, {"outside", m.plane.background()}};
        const io::IRect bb = io::content_bbox(m.plane);
        if (bb.empty()) {
            j["src"] = nullptr;
            return j;
        }
        const std::vector<uint8_t> g = io::read_region(m.plane, bb);
        std::vector<Rgba8> px(g.size());
        for (size_t i = 0; i < g.size(); ++i) px[i] = Rgba8{g[i], g[i], g[i], 255};
        j["src"] = add_priv("masks", ".png", png_of(px, bb.w, bb.h));
        j["x"] = bb.x;
        j["y"] = bb.y;
        return j;
    }

    bool needs_bake(const Node& n) const { return (n.mask && n.mask->enabled) || q(n.fill) != 255; }

    // Mask and fill folded into alpha: a' = q(a/255 * m/255 * fill) (viewing baseline only).
    RgbaImage baked(const Node& n) const {
        RgbaImage out(s.w, s.h);
        const RgbaImage& src = n.pixels;
        for (const TileKey& k : src.sorted_keys()) {
            RgbaTile t = src.tile(k.tx, k.ty);
            for (int ly = 0; ly < kTileSize; ++ly)
                for (int lx = 0; lx < kTileSize; ++lx) {
                    Rgba8& p = t.at(lx, ly);
                    if (p.a == 0) continue;
                    const int x = (k.tx * kTileSize) + lx, y = (k.ty * kTileSize) + ly;
                    const double m = (n.mask && n.mask->enabled) ? dec(n.mask->plane.get(x, y)) : 1.0;
                    p.a = q((dec(p.a) * m) * n.fill);
                    p = canonicalize(p);
                }
            out.put_tile_sparse(k.tx, k.ty, t);
        }
        return out;
    }

    std::string common_attrs(const Node& n) {
        std::string a = " name=\"" + io::xml_escape(n.name.empty() ? n.id : n.name) + "\"";
        a += " rl:id=\"" + io::xml_escape(n.id) + "\"";
        a += " visibility=\"" + std::string(n.visible ? "visible" : "hidden") + "\"";
        return a;
    }

    // One raster layer element; `op` overrides the composite-op, `opacity` its opacity.
    void layer_xml(const Node& n, const std::string& indent, const std::string& op, double opacity, bool alpha_preserve,
                   json& jn) {
        io::IRect bb;
        std::vector<uint8_t> png;
        if (needs_bake(n)) {
            png = rgba_png(baked(n), bb);
        } else {
            png = rgba_png(n.pixels, bb);
        }
        const std::string src = add_data_png(std::move(png));
        xml += indent + "<layer" + common_attrs(n) + " src=\"" + src + "\" x=\"" + std::to_string(bb.x) + "\" y=\"" +
               std::to_string(bb.y) + "\" opacity=\"" + num(opacity) + "\" composite-op=\"" + io::xml_escape(op) + "\"";
        if (alpha_preserve) xml += " alpha-preserve=\"true\"";
        if (n.lock_alpha) xml += " rl:alpha-locked=\"true\"";
        xml += "/>\n";
        // document.json pixel source: the data/ file when unbaked, else a private unbaked copy.
        if (!needs_bake(n)) {
            jn["pixels"] = json{{"src", src}, {"x", bb.x}, {"y", bb.y}};
        } else {
            io::IRect ub;
            std::vector<uint8_t> raw = rgba_png(n.pixels, ub);
            jn["pixels"] = json{{"src", add_priv("layers", ".png", std::move(raw))}, {"x", ub.x}, {"y", ub.y}};
        }
    }

    json node_json(const Node& n) {
        json j{{"id", n.id},           {"name", n.name},       {"kind", kind_name(n.kind)},
               {"visible", n.visible}, {"opacity", n.opacity}, {"mode", blend_mode_name(n.mode)},
               {"seed", n.seed}};
        if (!n.is_group()) {
            j["fill"] = n.fill;
            j["clip"] = n.clip;
            j["clbl"] = n.clbl;
        }
        if (n.is_raster()) j["lock_alpha"] = n.lock_alpha;
        if (n.is_adjustment()) {
            json params = json::object();
            if (!n.adjust_params.empty()) params = json::parse(n.adjust_params);
            j["adjustment"] = json{{"type", n.adjustment ? n.adjustment->type() : io::kUnsupportedAdjustment},
                                   {"params", params}};
        }
        if (n.mask) j["mask"] = mask_json(*n.mask);
        if (!n.foreign.empty()) j["foreign"] = foreign_json(n.foreign);
        return j;
    }

    // Writes `children` (bottom-first) as stack.xml elements top-first; returns their JSON
    // (bottom-first).
    json children_xml(const std::vector<Node>& children, const std::string& indent) {
        const size_t n = children.size();
        std::vector<json> js(n);
        // Units: a node plus the clipped raster/adjustment layers directly above it.
        std::vector<std::pair<size_t, size_t>> units;  // [begin, end)
        for (size_t i = 0; i < n;) {
            size_t e = i + 1;
            if (!children[i].is_group())
                while (e < n && !children[e].is_group() && children[e].clip) ++e;
            units.emplace_back(i, e);
            i = e;
        }
        std::string saved;
        std::vector<std::string> parts(units.size());
        for (size_t u = 0; u < units.size(); ++u) {
            saved.swap(xml);
            xml.clear();
            const auto [b, e] = units[u];
            const Node& base = children[b];
            js[b] = node_json(base);
            if (base.is_group()) {
                const bool pass = base.mode == BlendMode::Pass;
                xml += indent + "<stack" + common_attrs(base) + " opacity=\"" + num(base.opacity) + "\" isolation=\"" +
                       (pass ? "auto" : "isolate") + "\"";
                if (!pass) xml += std::string(" composite-op=\"") + io::xml_escape(composite_op(base.mode)) + "\"";
                xml += ">\n";
                js[b]["children"] = children_xml(base.children, indent + "  ");
                xml += indent + "</stack>\n";
            } else if (e - b > 1 && base.is_raster()) {
                xml += indent + "<stack name=\"" + io::xml_escape(base.name.empty() ? base.id : base.name) +
                       " (clipping group)\" rl:clipgroup=\"" + io::xml_escape(base.id) + "\" visibility=\"" +
                       (base.visible ? "visible" : "hidden") + "\" opacity=\"" + num(base.opacity) +
                       "\" isolation=\"isolate\" composite-op=\"" + io::xml_escape(composite_op(base.mode)) + "\">\n";
                for (size_t k = e; k-- > b + 1;) {
                    const Node& c = children[k];
                    js[k] = node_json(c);
                    if (!c.is_raster()) continue;
                    const bool normal = c.mode == BlendMode::Norm;
                    layer_xml(c, indent + "  ", normal ? "svg:src-atop" : composite_op(c.mode), c.opacity, !normal, js[k]);
                }
                layer_xml(base, indent + "  ", "svg:src-over", 1.0, false, js[b]);
                xml += indent + "</stack>\n";
            } else {
                for (size_t k = e; k-- > b;) {
                    const Node& c = children[k];
                    if (k != b) js[k] = node_json(c);
                    if (!c.is_raster()) continue;
                    layer_xml(c, indent, composite_op(c.mode), c.opacity, false, js[k]);
                }
            }
            parts[u] = std::move(xml);
            xml = std::move(saved);
        }
        for (size_t u = units.size(); u-- > 0;) xml += parts[u];
        json arr = json::array();
        for (json& j : js) arr.push_back(std::move(j));
        return arr;
    }
};

}  // namespace

const char* composite_op(BlendMode m) {
    for (const OpName& o : kOps)
        if (o.mode == m) return o.op;
    return "svg:src-over";
}

bool is_ora(const std::vector<uint8_t>& b) {
    return b.size() >= 54 && std::memcmp(b.data(), "PK\x03\x04", 4) == 0 && std::memcmp(b.data() + 30, "mimetype", 8) == 0 &&
           std::memcmp(b.data() + 38, kMime, 16) == 0;
}

std::vector<uint8_t> write(const DocState& s, std::vector<std::string>& warnings) {
    if (s.w < 1 || s.h < 1 || s.w > kMaxSide || s.h > kMaxSide) throw IoError("ORA: bad canvas size");
    Writer wr{s, warnings, {}, {}, 0, 0, {}};

    wr.xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<image version=\"0.0.6\" w=\"" + std::to_string(s.w) + "\" h=\"" +
             std::to_string(s.h) + "\" xmlns:rl=\"" + kNs + "\">\n  <stack>\n";
    json tree = wr.children_xml(s.root.children, "    ");
    wr.xml += "  </stack>\n</image>\n";
    const std::string stack = wr.xml;

    // Merged image and thumbnail from one streamed render.
    io::ThumbAccumulator thumb(s.w, s.h, 256);
    io::BandRenderer rr(s, true);
    std::vector<uint8_t> merged = io::encode_png_rows(s.w, s.h, [&](int y) {
        const Rgba8* row = rr.row(y);
        thumb.add_row(y, row);
        return row;
    });
    std::vector<uint8_t> thumb_png = io::encode_png(thumb.finish());

    json doc{{"format", "rasterloom.orp"},
             {"version", kFormatVersion},
             {"stack_sha256", io::sha256_hex(stack)},
             {"canvas", json{{"w", s.w}, {"h", s.h}, {"bg", hex_color(s.bg)}}},
             {"root", json{{"children", tree}}}};
    if (!s.foreign.empty()) doc["foreign"] = wr.foreign_json(s.foreign);
    auto sel_png = [&](const GrayImage& g) {
        std::vector<Rgba8> px(static_cast<size_t>(s.w) * static_cast<size_t>(s.h));
        for (int y = 0; y < s.h; ++y)
            for (int x = 0; x < s.w; ++x) {
                const uint8_t v = g.get(x, y);
                px[(static_cast<size_t>(y) * static_cast<size_t>(s.w)) + static_cast<size_t>(x)] = Rgba8{v, v, v, 255};
            }
        return wr.add_priv("selection", ".png", wr.png_of(px, s.w, s.h));
    };
    json sel = json::object();
    if (s.selection.active()) sel["mask"] = sel_png(s.selection.mask);
    if (s.selection.saved) sel["saved"] = sel_png(*s.selection.saved);
    if (!sel.empty()) doc["selection"] = sel;
    const std::string doc_text = doc.dump(1) + "\n";

    io::ZipWriter z;
    z.add("mimetype", std::vector<uint8_t>(kMime, kMime + std::strlen(kMime)), io::ZipWriter::Method::Store);
    z.add("stack.xml", std::vector<uint8_t>(stack.begin(), stack.end()));
    for (auto& [name, bytes] : wr.data_files) z.add(name, bytes, io::ZipWriter::Method::Store);
    z.add("Thumbnails/thumbnail.png", thumb_png, io::ZipWriter::Method::Store);
    z.add("mergedimage.png", merged, io::ZipWriter::Method::Store);
    z.add("document.json", std::vector<uint8_t>(doc_text.begin(), doc_text.end()));
    for (auto& [name, bytes] : wr.priv_files)
        z.add(name, bytes, name.size() > 4 && name.compare(name.size() - 4, 4, ".png") == 0 ? io::ZipWriter::Method::Store
                                                                                             : io::ZipWriter::Method::Deflate);
    return z.finish();
}

// ---- reader -------------------------------------------------------------------------------------

namespace {

struct Reader {
    const io::ZipReader& z;
    DocState& s;
    std::vector<std::string>& warnings;
    int next_id = 1;
    std::map<std::string, bool> used_ids;

    io::RgbaBuffer png(const std::string& name) {
        if (!z.has(name)) throw IoError("ORA: missing '" + name + "'");
        try {
            return io::decode_png(z.read(name));
        } catch (const IoError&) {
            throw;
        } catch (const std::exception& e) {
            throw IoError("ORA: '" + name + "': " + e.what());
        }
    }

    void blit(RgbaImage& img, const io::RgbaBuffer& b, int64_t x, int64_t y) {
        io::write_region(img, x, y, b.w, b.h, [&](int cx, int cy) {
            return canonicalize(b.at(static_cast<int>(cx - x), static_cast<int>(cy - y)));
        });
    }

    std::string fresh_id() {
        std::string id;
        do id = "n" + std::to_string(next_id++);
        while (used_ids.count(id));
        used_ids[id] = true;
        return id;
    }

    // ---- document.json (authoritative) ----
    ForeignData foreign_of(const json& arr) {
        ForeignData f;
        if (!arr.is_array()) return f;
        for (const json& b : arr) f.add(b.at("ns").get<std::string>(), b.at("key").get<std::string>(), z.read(b.at("src").get<std::string>()));
        return f;
    }

    Node node_of(const json& j) {
        const std::string kind = j.at("kind").get<std::string>();
        const std::string id = j.at("id").get<std::string>();
        if (id.empty() || id == "root" || used_ids.count(id)) throw IoError("document.json: bad or duplicate id '" + id + "'");
        used_ids[id] = true;
        Node n;
        if (kind == "group") {
            n = Node::make_group(id, BlendMode::Pass);
        } else if (kind == "adjustment") {
            const json& a = j.at("adjustment");
            const std::string type = a.at("type").get<std::string>();
            const json& p = a.at("params");
            const std::string params = (p.is_object() && p.empty()) ? std::string() : p.dump();
            n = Node::make_adjustment(id, io::build_file_adjustment(type, params, "ORA node '" + id + "'", warnings));
            n.adjust_params = params.empty() ? std::string("{}") : params;  // canonical (node.hpp)
        } else if (kind == "raster") {
            n = Node::make_raster(id, s.w, s.h);
        } else {
            throw IoError("document.json: unknown node kind '" + kind + "'");
        }
        n.name = file_name(j.value("name", std::string()));
        n.visible = j.at("visible").get<bool>();
        n.opacity = j.at("opacity").get<double>();
        const auto mode = parse_blend_mode(j.at("mode").get<std::string>());
        if (!mode) throw IoError("document.json: unknown mode '" + j.at("mode").get<std::string>() + "'");
        n.mode = *mode;
        n.seed = j.value("seed", uint64_t{0});
        if (kind != "group") {
            n.fill = j.at("fill").get<double>();
            n.clip = j.at("clip").get<bool>();
            n.clbl = j.at("clbl").get<bool>();
        }
        if (kind == "raster") {
            n.lock_alpha = j.at("lock_alpha").get<bool>();
            if (j.contains("pixels") && j["pixels"].is_object()) {
                const json& p = j["pixels"];
                blit(n.pixels, png(p.at("src").get<std::string>()), p.at("x").get<int64_t>(), p.at("y").get<int64_t>());
            }
        }
        if (j.contains("mask")) {
            const json& m = j["mask"];
            LayerMask lm;
            lm.enabled = m.at("enabled").get<bool>();
            lm.plane.reset(s.w, s.h, m.at("outside").get<uint8_t>());
            if (m.contains("src") && m["src"].is_string()) {
                const io::RgbaBuffer b = png(m["src"].get<std::string>());
                const int64_t x = m.at("x").get<int64_t>(), y = m.at("y").get<int64_t>();
                io::write_region(lm.plane, x, y, b.w, b.h,
                                 [&](int cx, int cy) { return b.at(static_cast<int>(cx - x), static_cast<int>(cy - y)).r; });
            }
            n.mask = std::move(lm);
        }
        if (j.contains("foreign")) n.foreign = foreign_of(j["foreign"]);
        if (kind == "group")
            for (const json& c : j.at("children")) n.children.push_back(node_of(c));
        return n;
    }

    GrayImage gray_plane(const std::string& name) {
        const io::RgbaBuffer b = png(name);
        if (b.w != s.w || b.h != s.h) throw IoError("ORA: '" + name + "' has the wrong size");
        GrayImage g(s.w, s.h, 0);
        io::write_region(g, 0, 0, s.w, s.h, [&](int x, int y) { return b.at(x, y).r; });
        return g;
    }

    void from_json(const json& doc) {
        const json& c = doc.at("canvas");
        s.bg = parse_hex(c.at("bg").get<std::string>());
        for (const json& n : doc.at("root").at("children")) s.root.children.push_back(node_of(n));
        if (doc.contains("foreign")) s.foreign = foreign_of(doc["foreign"]);
        if (doc.contains("selection")) {
            const json& sel = doc["selection"];
            if (sel.contains("mask")) s.selection.mask = gray_plane(sel["mask"].get<std::string>());
            if (sel.contains("saved")) s.selection.saved = gray_plane(sel["saved"].get<std::string>());
        }
    }

    // ---- stack.xml (generic OpenRaster) ----
    static double opacity_of(const io::XmlElement& e) {
        const std::string* o = e.attr("opacity");
        if (!o) return 1.0;
        try {
            const double v = std::stod(*o);
            return v != v ? 1.0 : clamp01(v);
        } catch (const std::exception&) {
            return 1.0;
        }
    }
    static bool visible_of(const io::XmlElement& e) {
        const std::string* v = e.attr("visibility");
        return !(v && *v == "hidden");
    }
    std::string name_of(const io::XmlElement& e, const char* dflt) {
        const std::string* v = e.attr("name");
        return v ? file_name(*v) : dflt;
    }
    // A layer name from the file, sanitised per doc 60 §13 (warning when it changed).
    std::string file_name(const std::string& raw) {
        io::SanitizedName n = io::sanitize_layer_name(raw);
        if (n.changed()) warnings.push_back(io::sanitized_name_warning("ORA", n));
        return std::move(n.name);
    }
    static int64_t int_attr(const io::XmlElement& e, const char* k) {
        const std::string* v = e.attr(k);
        if (!v) return 0;
        try {
            return std::stoll(*v);
        } catch (const std::exception&) {
            return 0;
        }
    }

    BlendMode op_mode(const io::XmlElement& e, bool& clip_marker) {
        clip_marker = false;
        const std::string* op = e.attr("composite-op");
        const std::string* ap = e.attr("alpha-preserve");
        if (ap && *ap == "true") clip_marker = true;
        if (!op) return BlendMode::Norm;
        if (*op == "svg:src-atop") {
            clip_marker = true;
            return BlendMode::Norm;
        }
        if (auto m = mode_of_op(*op)) return *m;
        warnings.push_back("ORA: composite-op '" + *op + "' is not supported; using Normal");
        return BlendMode::Norm;
    }

    // Children of a <stack>, returned bottom-first.
    std::vector<Node> stack_children(const io::XmlElement& st) {
        std::vector<Node> out;
        for (size_t k = st.children.size(); k-- > 0;) {
            const io::XmlElement& e = st.children[k];
            if (e.name == "stack") {
                Node g = Node::make_group(fresh_id(), BlendMode::Pass);
                g.name = name_of(e, "Group");
                g.visible = visible_of(e);
                g.opacity = opacity_of(e);
                const std::string* iso = e.attr("isolation");
                if (iso && *iso == "auto") {
                    g.mode = BlendMode::Pass;
                } else {
                    bool clip = false;
                    g.mode = op_mode(e, clip);
                }
                g.children = stack_children(e);
                out.push_back(std::move(g));
            } else if (e.name == "layer") {
                Node n = Node::make_raster(fresh_id(), s.w, s.h);
                n.name = name_of(e, "Layer");
                n.visible = visible_of(e);
                n.opacity = opacity_of(e);
                bool clip = false;
                n.mode = op_mode(e, clip);
                if (clip) {
                    if (!out.empty() && !out.back().is_group())
                        n.clip = true;
                    else
                        warnings.push_back("ORA: layer '" + n.name + "' inherits alpha but has no layer below; imported unclipped");
                }
                const std::string* al = e.attr("rl:alpha-locked");
                n.lock_alpha = al && *al == "true";
                const std::string* src = e.attr("src");
                if (src) {
                    const std::string p = *src;
                    if (p.size() < 4 || p.compare(p.size() - 4, 4, ".png") != 0)
                        warnings.push_back("ORA: layer '" + n.name + "' source '" + p + "' is not a PNG; left empty");
                    else
                        blit(n.pixels, png(p), int_attr(e, "x"), int_attr(e, "y"));
                }
                out.push_back(std::move(n));
            }
            // <text>, <filter> and unknown elements are ignored.
        }
        return out;
    }
};

}  // namespace

DocState read(const std::vector<uint8_t>& bytes, std::vector<std::string>& warnings) {
    const io::ZipReader z(bytes);
    if (!z.has("mimetype")) {
        warnings.push_back("ORA: no mimetype entry");
    } else {
        const std::vector<uint8_t> m = z.read("mimetype");
        if (std::string(m.begin(), m.end()) != kMime) warnings.push_back("ORA: unexpected mimetype");
    }
    if (!z.has("stack.xml")) throw IoError("ORA: no stack.xml");
    const std::vector<uint8_t> stack_bytes = z.read("stack.xml");
    const std::string stack(stack_bytes.begin(), stack_bytes.end());
    const io::XmlElement image = io::parse_xml(stack);
    if (image.name != "image") throw IoError("ORA: stack.xml root is not <image>");
    const int64_t w = Reader::int_attr(image, "w"), h = Reader::int_attr(image, "h");
    if (w < 1 || h < 1 || w > kMaxSide || h > kMaxSide)
        throw IoError("ORA: canvas size " + std::to_string(w) + "x" + std::to_string(h) + " is out of range");

    DocState s;
    s.w = static_cast<int>(w);
    s.h = static_cast<int>(h);
    s.bg = Rgba8{};
    s.root = Node::make_group("root", BlendMode::Pass);
    s.selection = Selection(s.w, s.h);
    Reader rd{z, s, warnings, 1, {}};

    bool use_json = false;
    json doc;
    if (z.has("document.json")) {
        const std::vector<uint8_t> d = z.read("document.json");
        bool repaired = false;
        const std::string text =
            io::repair_json_text(std::string_view(reinterpret_cast<const char*>(d.data()), d.size()), repaired);
        if (repaired)
            warnings.push_back("ORA: document.json has invalid UTF-8 or lone UTF-16 surrogate escapes; "
                               "they were replaced with U+FFFD");
        try {
            doc = json::parse(text);
        } catch (const std::exception& e) {
            throw IoError(std::string("ORA: document.json: ") + e.what());
        }
        if (doc.value("format", std::string()) == "rasterloom.orp" &&
            doc.value("stack_sha256", std::string()) == io::sha256_hex(stack)) {
            if (doc.value("version", 0) > kFormatVersion)
                warnings.push_back("ORA: document.json is from a newer Rasterloom; unknown fields are ignored");
            use_json = true;
        } else {
            warnings.push_back("ORA: stack.xml was changed by another program; Rasterloom-only data (masks, fill, "
                               "adjustments, ...) was ignored and the layers were read from stack.xml");
        }
    }
    try {
        if (use_json) {
            const json& c = doc.at("canvas");
            if (c.at("w").get<int64_t>() != w || c.at("h").get<int64_t>() != h)
                throw IoError("ORA: document.json and stack.xml disagree on the canvas size");
            rd.from_json(doc);
            return s;
        }
    } catch (const json::exception& e) {
        throw IoError(std::string("ORA: document.json: ") + e.what());
    }
    const io::XmlElement* root = nullptr;
    for (const io::XmlElement& c : image.children)
        if (c.name == "stack") {
            root = &c;
            break;
        }
    if (!root) throw IoError("ORA: stack.xml has no root <stack>");
    s.root.children = rd.stack_children(*root);
    return s;
}

}  // namespace rl::ora

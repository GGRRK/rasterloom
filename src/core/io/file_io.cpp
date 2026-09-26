// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/io/file_io.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

#include "core/io/atomic_file.hpp"
#include "core/io/image_util.hpp"
#include "core/io/io_error.hpp"
#include "core/io/jpeg.hpp"
#include "core/io/ora.hpp"
#include "core/io/png.hpp"
#include "core/io/render_rows.hpp"
#include "core/io/sha256.hpp"
#include "core/io/tiff.hpp"
#include "core/psd/psd.hpp"

namespace rl::io {

namespace {

std::string lower_ext(const std::string& path) {
    const size_t slash = path.rfind('/');
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string e = path.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

bool is_png(const std::vector<uint8_t>& b) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return b.size() >= 8 && std::equal(sig, sig + 8, b.begin());
}

DocState single_layer_doc(const RgbaBuffer& img, const std::string& name) {
    if (img.w < 1 || img.h < 1 || img.w > psd::kPsbMaxSide || img.h > psd::kPsbMaxSide)
        throw IoError("image size out of range");
    DocState s;
    s.w = img.w;
    s.h = img.h;
    s.bg = Rgba8{};
    s.root = Node::make_group("root", BlendMode::Pass);
    s.selection = Selection(s.w, s.h);
    Node n = Node::make_raster("n1", s.w, s.h);
    n.name = name;
    write_region(n.pixels, 0, 0, img.w, img.h, [&](int x, int y) { return canonicalize(img.at(x, y)); });
    s.root.children.push_back(std::move(n));
    return s;
}

RgbaBuffer composite(const DocState& s) {
    RgbaBuffer b;
    b.w = s.w;
    b.h = s.h;
    b.px.resize(static_cast<size_t>(s.w) * static_cast<size_t>(s.h));
    for_each_render_row(s, true, [&](int y, const Rgba8* row) {
        std::copy(row, row + s.w, b.px.begin() + (static_cast<std::ptrdiff_t>(y) * s.w));
    });
    return b;
}

nlohmann::json node_dump(const Node& n) {
    using nlohmann::json;
    json j{{"id", n.id}, {"name", n.name}, {"kind", n.is_group() ? "group" : n.is_raster() ? "raster" : "adjustment"},
           {"visible", n.visible}, {"opacity", n.opacity}, {"blend", blend_mode_name(n.mode)},
           {"blend_4cc", psd::blend_key(n.mode)}, {"mask", static_cast<bool>(n.mask)}};
    if (n.mask) {
        j["mask_enabled"] = n.mask->enabled;
        j["mask_outside"] = n.mask->plane.background();
    }
    if (!n.is_group()) {
        j["fill"] = n.fill;
        j["clip"] = n.clip;
        j["clbl"] = n.clbl;
    }
    if (n.is_raster()) {
        j["lock_alpha"] = n.lock_alpha;
        const IRect bb = content_bbox(n.pixels);
        j["bbox"] = bb.empty() ? json(nullptr) : json::array({bb.x, bb.y, bb.w, bb.h});
    }
    if (n.is_adjustment()) {
        j["adjustment"] = n.adjustment ? n.adjustment->type() : "";
        j["adjust_params"] = n.adjust_params;
    }
    json fb = json::array();
    for (const ForeignBlock& b : n.foreign.blocks)
        fb.push_back(json{{"ns", b.ns}, {"key", b.key}, {"size", b.size()}, {"sha256", sha256_hex(b.bytes())}});
    j["foreign"] = fb;
    if (n.is_group()) {
        json c = json::array();
        for (const Node& k : n.children) c.push_back(node_dump(k));
        j["children"] = c;
    }
    return j;
}

}  // namespace

FileFormat format_for_path(const std::string& path) {
    const std::string e = lower_ext(path);
    if (e == "png") return FileFormat::Png;
    if (e == "jpg" || e == "jpeg" || e == "jpe") return FileFormat::Jpeg;
    if (e == "tif" || e == "tiff") return FileFormat::Tiff;
    if (e == "ora" || e == "orp") return FileFormat::Ora;
    if (e == "psd" || e == "psb") return FileFormat::Psd;
    return FileFormat::Unknown;
}

FileFormat sniff_format(const std::vector<uint8_t>& b) {
    if (psd::is_psd(b)) return FileFormat::Psd;
    if (is_png(b)) return FileFormat::Png;
    if (is_jpeg(b)) return FileFormat::Jpeg;
    if (is_tiff(b)) return FileFormat::Tiff;
    if (b.size() >= 4 && b[0] == 'P' && b[1] == 'K' && b[2] == 3 && b[3] == 4) return FileFormat::Ora;
    return FileFormat::Unknown;
}

const char* format_name(FileFormat f) {
    switch (f) {
        case FileFormat::Png: return "PNG";
        case FileFormat::Jpeg: return "JPEG";
        case FileFormat::Tiff: return "TIFF";
        case FileFormat::Ora: return "OpenRaster";
        case FileFormat::Psd: return "PSD";
        case FileFormat::Unknown: break;
    }
    return "unknown";
}

DocState decode_document(const std::vector<uint8_t>& bytes, FileFormat& format, std::vector<std::string>& warnings) {
    format = sniff_format(bytes);
    try {
        switch (format) {
            case FileFormat::Psd: return psd::read(bytes, warnings);
            case FileFormat::Ora: return ora::read(bytes, warnings);
            case FileFormat::Png: return single_layer_doc(decode_png(bytes), "Background");
            case FileFormat::Jpeg: return single_layer_doc(decode_jpeg(bytes), "Background");
            case FileFormat::Tiff: return single_layer_doc(decode_tiff(bytes), "Background");
            case FileFormat::Unknown: break;
        }
    } catch (const IoError&) {
        throw;
    } catch (const std::exception& e) {
        // libpng / json / allocation failures surface as IoError too.
        throw IoError(std::string(format_name(format)) + ": " + e.what());
    }
    throw IoError("unrecognised file format");
}

OpenResult open_document(const std::string& path) {
    OpenResult r;
    const std::vector<uint8_t> bytes = read_file(path);
    DocState s;
    try {
        s = decode_document(bytes, r.format, r.warnings);
    } catch (const IoError& e) {
        throw IoError("cannot open '" + path + "': " + e.what());
    }
    r.doc = std::make_unique<Document>(s.w, s.h, s.bg);
    r.doc->state() = std::move(s);
    return r;
}

std::vector<uint8_t> encode_document(const DocState& s, FileFormat f, const SaveOptions& opt,
                                     std::vector<std::string>& warnings) {
    try {
        switch (f) {
            case FileFormat::Ora: return ora::write(s, warnings);
            case FileFormat::Psd: {
                psd::WriteOptions po;
                po.force_psb = opt.force_psb;
                return psd::write(s, po, warnings);
            }
            case FileFormat::Png: {
                BandRenderer rr(s, true);  // streamed: no full-canvas buffer
                return encode_png_rows(s.w, s.h, [&rr](int y) { return rr.row(y); });
            }
            case FileFormat::Jpeg: {
                if (opt.jpeg_quality < 1 || opt.jpeg_quality > 100) throw IoError("JPEG quality must be 1..100");
                RgbaBuffer c = composite(s);
                bool translucent = false;
                for (Rgba8& p : c.px) {
                    translucent = translucent || p.a != 255;
                    p = matte_white(p);
                }
                if (translucent) warnings.push_back("JPEG has no transparency: the image was flattened onto white");
                return encode_jpeg(c, opt.jpeg_quality);
            }
            case FileFormat::Tiff: return encode_tiff(composite(s));
            case FileFormat::Unknown: break;
        }
    } catch (const IoError&) {
        throw;
    } catch (const std::exception& e) {
        throw IoError(std::string(format_name(f)) + ": " + e.what());
    }
    throw IoError("unknown file format (use .orp, .ora, .psd, .psb, .png, .jpg or .tif)");
}

std::vector<std::string> save_document(const DocState& s, const std::string& path, const SaveOptions& opt) {
    const FileFormat f = format_for_path(path);
    std::vector<std::string> warnings;
    SaveOptions o = opt;
    if (lower_ext(path) == "psb") o.force_psb = true;
    std::vector<uint8_t> bytes;
    try {
        bytes = encode_document(s, f, o, warnings);
    } catch (const IoError& e) {
        throw IoError("cannot save '" + path + "': " + e.what());
    }
    atomic_write_file(path, bytes);
    return warnings;
}

std::string dump_tree_json(const DocState& s) {
    using nlohmann::json;
    json c = json::array();
    for (const Node& n : s.root.children) c.push_back(node_dump(n));
    json fb = json::array();
    for (const ForeignBlock& b : s.foreign.blocks)
        fb.push_back(json{{"ns", b.ns}, {"key", b.key}, {"size", b.size()}, {"sha256", sha256_hex(b.bytes())}});
    json j{{"w", s.w}, {"h", s.h}, {"children", c}, {"foreign", fb}};
    return j.dump(1) + "\n";
}

}  // namespace rl::io

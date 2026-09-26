// SPDX-License-Identifier: GPL-3.0-or-later
//
// PSD/PSB reader. Section layout from Adobe's "Photoshop File Formats Specification": File Header,
// Color Mode Data, Image Resources, Layer and Mask Information (Layer Info, Layer Records, Channel
// Image Data, Global Layer Mask Info, Additional Layer Information), Image Data. See psd.hpp for
// the scope and the psd-tools cross-references.
#include <zlib.h>

#include <map>

#include "core/base/quant.hpp"
#include "core/io/bytes.hpp"
#include "core/io/image_util.hpp"
#include "core/io/layer_name.hpp"
#include "core/io/unrendered_adjustment.hpp"
#include "core/psd/psd.hpp"
#include "core/psd/psd_internal.hpp"

namespace rl::psd {

namespace {

using io::ByteReader;
using io::IoError;

enum ColorMode { kBitmap = 0, kGray = 1, kIndexed = 2, kRgb = 3, kCmyk = 4, kMulti = 7, kDuotone = 8, kLab = 9 };

struct Header {
    int version = 1;
    int channels = 0;
    int w = 0;
    int h = 0;
    int depth = 8;
    int mode = kRgb;
    bool psb() const { return version == 2; }
};

struct Block {
    std::string sig;  // "8BIM" or "8B64"
    std::string key;
    std::vector<uint8_t> data;
};

struct Channel {
    int16_t id = 0;
    uint64_t len = 0;
};

struct Record {
    int32_t top = 0, left = 0, bottom = 0, right = 0;
    std::vector<Channel> channels;
    std::string blend = "norm";
    uint8_t opacity = 255, clipping = 0, flags = 0;
    std::vector<uint8_t> mask_data, ranges;
    std::string pascal;
    std::vector<Block> blocks;
    // Decoded 8-bit planes (layer rect, or mask rect for -2), and the raw -3 channel.
    std::map<int, std::vector<uint8_t>> planes;
    std::vector<uint8_t> real_raw;
    bool has_real_channel = false;

    const Block* block(const std::string& k) const {
        for (const Block& b : blocks)
            if (b.key == k) return &b;
        return nullptr;
    }
};

struct MaskGeom {
    bool present = false;
    int32_t top = 0, left = 0, bottom = 0, right = 0;
    uint8_t def = 0, flags = 0;
};

MaskGeom mask_geom(const Record& r) {
    MaskGeom m;
    if (r.mask_data.size() < 18) return m;
    ByteReader b(r.mask_data, "PSD layer mask");
    m.present = true;
    m.top = b.bei32();
    m.left = b.bei32();
    m.bottom = b.bei32();
    m.right = b.bei32();
    m.def = b.u8();
    m.flags = b.u8();
    return m;
}

bool is_sig(const uint8_t* p) { return std::memcmp(p, "8BIM", 4) == 0 || std::memcmp(p, "8B64", 4) == 0; }

std::vector<Block> read_blocks(ByteReader& r, bool psb, bool global, std::vector<std::string>& warnings) {
    std::vector<Block> out;
    while (r.left() >= 12) {
        if (!is_sig(r.here())) {
            // Tolerate a writer that padded a layer block without counting it in the length.
            size_t k = 1;
            while (k < 4 && r.left() >= k + 12 && r.here()[k - 1] == 0 && !is_sig(r.here() + k)) ++k;
            if (k < 4 && r.left() >= k + 12 && r.here()[k - 1] == 0 && is_sig(r.here() + k)) {
                r.skip(k);
                continue;
            }
            if (r.left() > 3) warnings.push_back("PSD: ignored " + std::to_string(r.left()) +
                                                 " unparseable trailing bytes in " +
                                                 (global ? "the global additional info" : "a layer record"));
            break;
        }
        Block b;
        b.sig = r.str(4);
        b.key = r.str(4);
        const uint64_t len = (psb && detail::is_big_key(b.key)) ? r.be64() : r.be32();
        b.data = r.bytes(len);
        if (global) {
            const uint64_t pad = (4 - (len % 4)) % 4;
            r.skip(std::min<uint64_t>(pad, r.left()));
        }
        out.push_back(std::move(b));
    }
    return out;
}

// Decodes one channel (compression field + data) of w x h samples at `depth` bits into 8-bit.
std::vector<uint8_t> decode_plane(ByteReader cr, int64_t w, int64_t h, int depth, bool psb, const std::string& what) {
    if (cr.left() < 2) {
        if (w * h == 0) return {};
        throw IoError("PSD: " + what + ": channel data missing");
    }
    const uint16_t comp = cr.be16();
    if (w <= 0 || h <= 0) return {};
    const int bpc = depth / 8;
    const uint64_t rowbytes = static_cast<uint64_t>(w) * static_cast<uint64_t>(bpc);
    const uint64_t total = rowbytes * static_cast<uint64_t>(h);
    // Bounds on the decoded size relative to the stored size stop decompression bombs.
    const uint64_t stored = cr.left();
    const uint64_t max_ratio = comp == 0 ? 1 : (comp == 1 ? 130 : 1100);
    if (total > (uint64_t{1} << 34) || total > (stored + 64) * max_ratio + (comp == 1 ? static_cast<uint64_t>(h) * 130 : 0))
        throw IoError("PSD: " + what + ": channel is corrupt (size " + std::to_string(w) + "x" + std::to_string(h) + ")");
    std::vector<uint8_t> buf(static_cast<size_t>(total));
    if (comp == 0) {
        cr.need(total);
        std::memcpy(buf.data(), cr.here(), static_cast<size_t>(total));
    } else if (comp == 1) {
        std::vector<uint64_t> counts(static_cast<size_t>(h));
        for (auto& c : counts) c = psb ? cr.be32() : cr.be16();
        for (int64_t y = 0; y < h; ++y) {
            const uint64_t c = counts[static_cast<size_t>(y)];
            cr.need(c);
            if (!packbits_decode(cr.here(), static_cast<size_t>(c), buf.data() + (static_cast<uint64_t>(y) * rowbytes),
                                 static_cast<size_t>(rowbytes)))
                throw IoError("PSD: " + what + ": corrupt RLE row " + std::to_string(y));
            cr.skip(c);
        }
    } else if (comp == 2 || comp == 3) {
        if (total > 0xFFFFFFFFu || cr.left() > 0xFFFFFFFFu) throw IoError("PSD: " + what + ": ZIP channel too large");
        z_stream zs{};
        if (inflateInit(&zs) != Z_OK) throw IoError("PSD: zlib init failed");
        zs.next_in = const_cast<Bytef*>(cr.here());
        zs.avail_in = static_cast<uInt>(std::min<uint64_t>(cr.left(), 0xFFFFFFFFu));
        zs.next_out = buf.data();
        zs.avail_out = static_cast<uInt>(total);
        const int rc = inflate(&zs, Z_FINISH);
        const uint64_t got = zs.total_out;
        inflateEnd(&zs);
        if ((rc != Z_STREAM_END && rc != Z_BUF_ERROR) || got != total)
            throw IoError("PSD: " + what + ": corrupt ZIP channel");
        if (comp == 3) {
            for (int64_t y = 0; y < h; ++y) {
                uint8_t* row = buf.data() + (static_cast<uint64_t>(y) * rowbytes);
                if (depth == 8) {
                    for (int64_t x = 1; x < w; ++x) row[x] = static_cast<uint8_t>(row[x] + row[x - 1]);
                } else if (depth == 16) {
                    for (int64_t x = 1; x < w; ++x) {
                        const unsigned prev = (row[(2 * x) - 2] << 8) | row[(2 * x) - 1];
                        const unsigned cur = (row[2 * x] << 8) | row[(2 * x) + 1];
                        const unsigned v = (prev + cur) & 0xFFFFu;
                        row[2 * x] = static_cast<uint8_t>(v >> 8);
                        row[(2 * x) + 1] = static_cast<uint8_t>(v);
                    }
                }
            }
        }
    } else {
        throw IoError("PSD: " + what + ": unknown channel compression " + std::to_string(comp));
    }
    if (depth == 8) return buf;
    // 16 -> 8 bit: q(v / 65535) (00-conventions C2).
    std::vector<uint8_t> out(static_cast<size_t>(w * h));
    for (size_t i = 0; i < out.size(); ++i) {
        const unsigned v = (buf[2 * i] << 8) | buf[(2 * i) + 1];
        out[i] = q(static_cast<double>(v) / 65535.0);
    }
    return out;
}

std::vector<Record> read_layer_info(ByteReader& r, const Header& hd, std::vector<std::string>& warnings) {
    std::vector<Record> recs;
    if (r.left() < 2) return recs;
    const int16_t count = r.bei16();
    const int n = count < 0 ? -count : count;
    for (int i = 0; i < n; ++i) {
        Record rec;
        rec.top = r.bei32();
        rec.left = r.bei32();
        rec.bottom = r.bei32();
        rec.right = r.bei32();
        const uint16_t nch = r.be16();
        if (nch > 56) throw IoError("PSD: layer " + std::to_string(i) + " has " + std::to_string(nch) + " channels");
        for (int c = 0; c < nch; ++c) {
            Channel ch;
            ch.id = r.bei16();
            ch.len = hd.psb() ? r.be64() : r.be32();
            rec.channels.push_back(ch);
        }
        const std::string sig = r.str(4);
        if (sig != "8BIM") throw IoError("PSD: bad blend-mode signature in layer record " + std::to_string(i));
        rec.blend = r.str(4);
        rec.opacity = r.u8();
        rec.clipping = r.u8();
        rec.flags = r.u8();
        r.skip(1);
        const uint32_t extra = r.be32();
        ByteReader x = r.sub(extra, "PSD layer record");
        const uint32_t mlen = x.be32();
        rec.mask_data = x.bytes(mlen);
        const uint32_t rlen = x.be32();
        rec.ranges = x.bytes(rlen);
        const uint8_t nlen = x.u8();
        rec.pascal = x.str(nlen);
        const size_t pad = (4 - ((1 + nlen) % 4)) % 4;
        x.skip(std::min<size_t>(pad, x.left()));
        rec.blocks = read_blocks(x, hd.psb(), false, warnings);
        recs.push_back(std::move(rec));
    }
    // Channel image data, in record order.
    for (size_t i = 0; i < recs.size(); ++i) {
        Record& rec = recs[i];
        const MaskGeom mg = mask_geom(rec);
        for (const Channel& ch : rec.channels) {
            ByteReader cr = r.sub(ch.len, "PSD channel data");
            const std::string what = "layer " + std::to_string(i) + " channel " + std::to_string(ch.id);
            if (ch.id == -3) {
                rec.real_raw = cr.bytes(cr.left());
                rec.has_real_channel = true;
                continue;
            }
            int64_t w = static_cast<int64_t>(rec.right) - rec.left, h = static_cast<int64_t>(rec.bottom) - rec.top;
            if (ch.id == -2) {
                if (!mg.present) continue;
                w = static_cast<int64_t>(mg.right) - mg.left;
                h = static_cast<int64_t>(mg.bottom) - mg.top;
            }
            if (w < 0 || h < 0 || w > kPsbMaxSide * 2LL || h > kPsbMaxSide * 2LL)
                throw IoError("PSD: " + what + ": bad bounds");
            rec.planes[ch.id] = decode_plane(cr, w, h, hd.depth, hd.psb(), what);
        }
    }
    return recs;
}

// The layer's name (Unicode `luni` when present, else the Latin-1 Pascal name), sanitised per
// doc 60 §13 (io/layer_name.hpp).
io::SanitizedName record_name(const Record& rec) {
    if (const Block* b = rec.block("luni"); b && b->data.size() >= 4) {
        ByteReader r(b->data, "PSD luni");
        const uint32_t n = r.be32();
        std::vector<uint16_t> u;
        for (uint32_t i = 0; i < n && r.left() >= 2; ++i) u.push_back(r.be16());
        while (!u.empty() && u.back() == 0) u.pop_back();
        return io::sanitize_layer_name_utf16(u);
    }
    return io::sanitize_layer_name(detail::latin1_to_utf8(rec.pascal));
}

uint8_t first_byte(const Block* b, uint8_t dflt) { return (b && !b->data.empty()) ? b->data[0] : dflt; }

void own(Node& n, const std::string& key, std::vector<uint8_t> v) { n.foreign.add("psd.own", key, std::move(v)); }

struct Builder {
    const Header& hd;
    std::vector<std::string>& warnings;
    int next_id = 1;

    std::string new_id() { return "n" + std::to_string(next_id++); }

    // Properties every node kind carries: name, visibility, opacity, mask, pass-through blocks.
    void common(Node& n, const Record& rec, const std::string& name) {
        n.name = name;
        n.visible = !(rec.flags & 2);
        n.opacity = dec(rec.opacity);
        own(n, "flags", {rec.flags});
        if (!rec.ranges.empty()) own(n, "ranges", rec.ranges);
        own(n, "pname", std::vector<uint8_t>(rec.pascal.begin(), rec.pascal.end()));
        if (const Block* b = rec.block("lspf")) own(n, "lspf", b->data);
        for (const Block& b : rec.blocks)
            if (!detail::is_owned_layer_key(b.key)) n.foreign.add(b.sig == "8B64" ? "psd.tb64" : "psd.tb", b.key, b.data);
        const MaskGeom mg = mask_geom(rec);
        if (mg.present) {
            LayerMask m;
            m.plane.reset(hd.w, hd.h, mg.def);
            m.enabled = !(mg.flags & 2);
            auto it = rec.planes.find(-2);
            const int64_t mw = static_cast<int64_t>(mg.right) - mg.left, mh = static_cast<int64_t>(mg.bottom) - mg.top;
            if (it != rec.planes.end() && !it->second.empty()) {
                const std::vector<uint8_t>& p = it->second;
                io::write_region(m.plane, mg.left, mg.top, mw, mh, [&](int x, int y) {
                    return p[static_cast<size_t>(((static_cast<int64_t>(y) - mg.top) * mw) + (static_cast<int64_t>(x) - mg.left))];
                });
            }
            n.mask = std::move(m);
            own(n, "mask.flags", {mg.flags});
            own(n, "mask.tail", std::vector<uint8_t>(rec.mask_data.begin() + 18, rec.mask_data.end()));
            if (rec.has_real_channel) own(n, "mask.real", rec.real_raw);
        }
    }

    Node raster(const Record& rec, const std::string& name, size_t index) {
        Node n = Node::make_raster(new_id(), hd.w, hd.h);
        common(n, rec, name);
        n.mode = mode_from_key(rec.blend).value_or(BlendMode::Norm);
        if (n.mode == BlendMode::Pass) n.mode = BlendMode::Norm;
        if (!mode_from_key(rec.blend))
            warnings.push_back("PSD: layer '" + name + "': unknown blend mode '" + rec.blend + "', using Normal");
        n.fill = dec(first_byte(rec.block("iOpa"), 255));
        n.clip = rec.clipping != 0;
        n.clbl = first_byte(rec.block("clbl"), 1) != 0;
        uint32_t lspf = 0;
        if (const Block* b = rec.block("lspf"); b && b->data.size() >= 4)
            lspf = (static_cast<uint32_t>(b->data[0]) << 24) | (static_cast<uint32_t>(b->data[1]) << 16) |
                   (static_cast<uint32_t>(b->data[2]) << 8) | b->data[3];
        n.lock_alpha = (rec.flags & 1) || (lspf & 1u) || (lspf & 0x80000000u);
        own(n, "lock", {static_cast<uint8_t>(n.lock_alpha ? 1 : 0)});  // record flag bit 0 is kept while unchanged

        const int64_t w = static_cast<int64_t>(rec.right) - rec.left, h = static_cast<int64_t>(rec.bottom) - rec.top;
        if (w <= 0 || h <= 0) return n;
        if (rec.left < 0 || rec.top < 0 || rec.right > hd.w || rec.bottom > hd.h)
            warnings.push_back("PSD: layer '" + name + "': pixels outside the canvas were discarded");
        auto plane = [&](int id) -> const std::vector<uint8_t>* {
            auto it = rec.planes.find(id);
            return (it == rec.planes.end() || it->second.size() != static_cast<size_t>(w * h)) ? nullptr : &it->second;
        };
        const std::vector<uint8_t>* c0 = plane(0);
        const std::vector<uint8_t>* c1 = plane(1);
        const std::vector<uint8_t>* c2 = plane(2);
        const std::vector<uint8_t>* c3 = plane(3);
        const std::vector<uint8_t>* ca = plane(-1);
        const int mode = hd.mode;
        (void)index;
        io::write_region(n.pixels, rec.left, rec.top, w, h, [&](int x, int y) {
            const size_t i = static_cast<size_t>(((static_cast<int64_t>(y) - rec.top) * w) + (static_cast<int64_t>(x) - rec.left));
            auto at = [i](const std::vector<uint8_t>* p, uint8_t d) { return p ? (*p)[i] : d; };
            Rgba8 px;
            px.a = at(ca, 255);
            if (mode == kCmyk) {
                // Stored inverted: s = 255 - ink, so (1 - ink) = s / 255. RGB = (1-C)(1-K) etc.
                const double k = dec(at(c3, 255));
                px.r = q(dec(at(c0, 255)) * k);
                px.g = q(dec(at(c1, 255)) * k);
                px.b = q(dec(at(c2, 255)) * k);
            } else if (mode == kGray || mode == kDuotone) {
                px.r = px.g = px.b = at(c0, 0);
            } else {
                px.r = at(c0, 0);
                px.g = at(c1, 0);
                px.b = at(c2, 0);
            }
            return canonicalize(px);
        });
        return n;
    }
};

}  // namespace

DocState read(const std::vector<uint8_t>& bytes, std::vector<std::string>& warnings) {
    ByteReader r(bytes, "PSD");
    if (r.str(4) != "8BPS") throw IoError("not a PSD/PSB file");
    Header hd;
    hd.version = r.be16();
    if (hd.version != 1 && hd.version != 2) throw IoError("PSD: unknown version " + std::to_string(hd.version));
    r.skip(6);
    hd.channels = r.be16();
    const uint32_t h = r.be32(), w = r.be32();
    hd.depth = r.be16();
    hd.mode = r.be16();
    const uint32_t lim = hd.psb() ? kPsbMaxSide : kPsdMaxSide;
    if (w < 1 || h < 1 || w > lim || h > lim)
        throw IoError("PSD: image size " + std::to_string(w) + "x" + std::to_string(h) + " is out of range for " +
                      (hd.psb() ? "PSB" : "PSD"));
    hd.w = static_cast<int>(w);
    hd.h = static_cast<int>(h);
    if (hd.depth != 8 && hd.depth != 16)
        throw IoError("PSD: " + std::to_string(hd.depth) + "-bit files are not supported (8 and 16 bit are)");
    int color_channels = 3;
    switch (hd.mode) {
        case kRgb: break;
        case kGray: color_channels = 1; break;
        case kDuotone:
            color_channels = 1;
            warnings.push_back("PSD: duotone image imported as its greyscale data");
            break;
        case kCmyk:
            color_channels = 4;
            warnings.push_back("PSD: CMYK converted to 8-bit RGB (naive, no colour management)");
            break;
        default:
            throw IoError("PSD: colour mode " + std::to_string(hd.mode) +
                          " is not supported (RGB, Grayscale and CMYK are)");
    }
    if (hd.mode == kGray) warnings.push_back("PSD: greyscale converted to RGB");
    if (hd.depth == 16) warnings.push_back("PSD: 16-bit channels converted to 8-bit");
    if (hd.channels < color_channels || hd.channels > 56) throw IoError("PSD: bad channel count");

    DocState s;
    s.w = hd.w;
    s.h = hd.h;
    s.bg = Rgba8{};
    s.root = Node::make_group("root", BlendMode::Pass);
    s.selection = Selection(hd.w, hd.h);

    // Color mode data: only meaningful for indexed and duotone; not written back (we write RGB).
    { ByteReader cm = r.sub(r.be32(), "PSD colour mode data"); }

    // Image resources.
    const bool extra_channels = hd.channels > color_channels + 1;
    {
        ByteReader ir = r.sub(r.be32(), "PSD image resources");
        while (ir.left() >= 12) {
            const size_t start = ir.pos();
            ir.skip(4);  // signature (8BIM, MeSa, ...)
            const uint16_t id = ir.be16();
            const uint8_t nl = ir.u8();
            ir.skip(nl + ((1 + nl) % 2));
            const uint32_t sz = ir.be32();
            ir.skip(sz);
            if ((sz % 2) && ir.left() > 0) ir.skip(1);
            const bool regenerated = id == 1033 || id == 1036 || id == 1057;
            const bool colour_bound = hd.mode != kRgb && id == 1039;
            const bool channel_bound =
                extra_channels && (id == 1006 || id == 1007 || id == 1045 || id == 1053 || id == 1077);
            if (regenerated) continue;
            if (colour_bound || channel_bound) {
                warnings.push_back("PSD: image resource " + std::to_string(id) + " dropped (" +
                                   (colour_bound ? "ICC profile of the source colour mode" : "describes extra channels") + ")");
                continue;
            }
            s.foreign.add("psd.irb", std::to_string(id),
                          std::vector<uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(ir.data() - bytes.data() + start),
                                               bytes.begin() + static_cast<std::ptrdiff_t>(ir.data() - bytes.data() + ir.pos())));
        }
    }
    if (extra_channels) warnings.push_back("PSD: extra (alpha/spot) channels of the merged image were dropped");

    // Layer and mask information.
    std::vector<Record> recs;
    {
        const uint64_t len = hd.psb() ? r.be64() : r.be32();
        ByteReader lm = r.sub(len, "PSD layer and mask information");
        if (lm.left() > 0) {
            const uint64_t li = hd.psb() ? lm.be64() : lm.be32();
            ByteReader lr = lm.sub(li, "PSD layer info");
            recs = read_layer_info(lr, hd, warnings);
        }
        if (lm.left() >= 4) {
            const uint32_t gl = lm.be32();
            std::vector<uint8_t> glmi = lm.bytes(gl);
            if (!glmi.empty()) s.foreign.add("psd.glmi", "", std::move(glmi));
        }
        // Global blocks start 4-aligned relative to the section.
        while (lm.left() > 0 && lm.pos() % 4 && *lm.here() == 0) lm.skip(1);
        for (Block& b : read_blocks(lm, hd.psb(), true, warnings)) {
            if (b.key == "Lr16" || b.key == "Lr32" || b.key == "Layr") {
                if (b.key == "Lr32") throw IoError("PSD: 32-bit layer data is not supported");
                if (!recs.empty()) continue;
                ByteReader lr(b.data, "PSD " + b.key);
                recs = read_layer_info(lr, hd, warnings);
                continue;
            }
            if (detail::is_consumed_global_key(b.key)) continue;
            s.foreign.add(b.sig == "8B64" ? "psd.gtb64" : "psd.gtb", b.key, std::move(b.data));
        }
    }

    Builder bld{hd, warnings};
    if (recs.empty()) {
        // A flat file: the merged image is the only pixel data.
        if (r.left() < 2) throw IoError("PSD: no layers and no image data");
        Record rec;
        rec.left = rec.top = 0;
        rec.right = hd.w;
        rec.bottom = hd.h;
        const uint16_t comp = r.be16();
        const uint64_t plane_rows = static_cast<uint64_t>(hd.h);
        std::vector<uint64_t> counts;
        if (comp == 1) {
            counts.resize(static_cast<size_t>(plane_rows * static_cast<uint64_t>(hd.channels)));
            for (auto& c : counts) c = hd.psb() ? r.be32() : r.be16();
        }
        const uint64_t rowbytes = static_cast<uint64_t>(hd.w) * static_cast<uint64_t>(hd.depth / 8);
        for (int c = 0; c < color_channels; ++c) {
            io::ByteWriter one;
            one.be16(comp == 1 ? 0 : comp);
            if (comp == 1) {
                std::vector<uint8_t> raw(static_cast<size_t>(rowbytes * plane_rows));
                for (uint64_t y = 0; y < plane_rows; ++y) {
                    const uint64_t cnt = counts[static_cast<size_t>((static_cast<uint64_t>(c) * plane_rows) + y)];
                    r.need(cnt);
                    if (!packbits_decode(r.here(), static_cast<size_t>(cnt), raw.data() + (y * rowbytes), static_cast<size_t>(rowbytes)))
                        throw IoError("PSD: corrupt RLE in the merged image");
                    r.skip(cnt);
                }
                one.raw(raw);
            } else if (comp == 0) {
                one.raw(r.bytes(rowbytes * plane_rows));
            } else {
                throw IoError("PSD: ZIP-compressed merged image data is not supported");
            }
            ByteReader cr(one.buf, "PSD merged image");
            rec.planes[c] = decode_plane(cr, hd.w, hd.h, hd.depth, hd.psb(), "merged channel " + std::to_string(c));
        }
        Node n = bld.raster(rec, "Background", 0);
        n.foreign.blocks.clear();  // bookkeeping of a synthetic record
        s.root.children.push_back(std::move(n));
        return s;
    }

    // Build the tree. Records are bottom-to-top; a group is [divider, children..., group record].
    std::vector<Node> stack;
    stack.push_back(std::move(s.root));
    for (size_t i = 0; i < recs.size(); ++i) {
        const Record& rec = recs[i];
        const Block* sect = rec.block("lsct");
        if (!sect) sect = rec.block("lsdk");
        uint32_t type = 0;
        if (sect && sect->data.size() >= 4)
            type = (static_cast<uint32_t>(sect->data[0]) << 24) | (static_cast<uint32_t>(sect->data[1]) << 16) |
                   (static_cast<uint32_t>(sect->data[2]) << 8) | sect->data[3];
        const io::SanitizedName sname = record_name(rec);
        const std::string& name = sname.name;
        if (type == 3) {
            Node g = Node::make_group("", BlendMode::Pass);
            for (const Block& b : rec.blocks)
                if (!detail::is_owned_layer_key(b.key)) {
                    std::vector<uint8_t> enc(b.sig.begin(), b.sig.end());
                    enc.insert(enc.end(), b.data.begin(), b.data.end());
                    g.foreign.add("psd.div", b.key, std::move(enc));
                }
            g.foreign.add("psd.own", "div.flags", {rec.flags});
            g.foreign.add("psd.own", "div.pname", std::vector<uint8_t>(rec.pascal.begin(), rec.pascal.end()));
            if (const Block* d = rec.block("luni")) g.foreign.add("psd.own", "div.luni", d->data);
            stack.push_back(std::move(g));
            continue;
        }
        if (sname.changed()) warnings.push_back(io::sanitized_name_warning("PSD", sname));  // not for dividers
        if (type == 1 || type == 2) {
            Node g;
            if (stack.size() > 1) {
                g = std::move(stack.back());
                stack.pop_back();
            } else {
                warnings.push_back("PSD: group '" + name + "' has no closing divider; imported as an empty group");
                g = Node::make_group("", BlendMode::Pass);
            }
            g.id = bld.new_id();
            bld.common(g, rec, name);
            std::string key = rec.blend;
            if (sect->data.size() >= 12 && std::memcmp(sect->data.data() + 4, "8BIM", 4) == 0)
                key.assign(reinterpret_cast<const char*>(sect->data.data() + 8), 4);
            g.mode = mode_from_key(key).value_or(BlendMode::Pass);
            own(g, "lsct", sect->data);
            stack.back().children.push_back(std::move(g));
            continue;
        }
        // Adjustment layer?
        const Block* adj = nullptr;
        for (const Block& b : rec.blocks)
            if (detail::is_adjustment_key(b.key)) {
                adj = &b;
                break;
            }
        if (adj) {
            std::string type_name, params;
            const bool mapped = detail::adjustment_from_psd(adj->key, adj->data, type_name, params);
            if (!mapped) {
                type_name = io::kUnsupportedAdjustment;
                params.clear();
                warnings.push_back("PSD: adjustment layer '" + name + "' ('" + adj->key +
                                   "') has no Rasterloom equivalent; it is kept and saved back but not rendered");
            }
            Node n = Node::make_adjustment(bld.new_id(), io::build_file_adjustment(type_name, params, "PSD layer '" + name + "'", warnings));
            n.adjust_params = params.empty() ? std::string("{}") : params;  // canonical (node.hpp)
            // Reuse the raster path for the shared properties, then drop its pixels.
            Node tmp = bld.raster(rec, name, i);
            n.name = tmp.name;
            n.visible = tmp.visible;
            n.opacity = tmp.opacity;
            n.mode = tmp.mode;
            n.fill = tmp.fill;
            n.clip = tmp.clip;
            n.clbl = tmp.clbl;
            n.mask = std::move(tmp.mask);
            n.foreign = std::move(tmp.foreign);
            stack.back().children.push_back(std::move(n));
            continue;
        }
        stack.back().children.push_back(bld.raster(rec, name, i));
    }
    while (stack.size() > 1) {
        Node g = std::move(stack.back());
        stack.pop_back();
        g.id = bld.new_id();
        g.name = "Group";
        warnings.push_back("PSD: a group divider has no group record; the group was closed at the top");
        stack.back().children.push_back(std::move(g));
    }
    s.root = std::move(stack.back());
    return s;
}

}  // namespace rl::psd

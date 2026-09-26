// SPDX-License-Identifier: GPL-3.0-or-later
//
// PSD/PSB writer (see psd.hpp). Layout per Adobe's "Photoshop File Formats Specification".
#include <cmath>
#include <cstring>

#include "core/base/quant.hpp"
#include "core/io/bytes.hpp"
#include "core/io/image_util.hpp"
#include "core/io/jpeg.hpp"
#include "core/io/render_rows.hpp"
#include "core/io/unrendered_adjustment.hpp"
#include "core/psd/psd.hpp"
#include "core/psd/psd_internal.hpp"

namespace rl::psd {

namespace {

using io::ByteWriter;
using io::IoError;

constexpr int kThumbMax = 160;

struct ChanOut {
    int16_t id = 0;
    std::vector<uint8_t> data;  // compression field + data
};

struct RecOut {
    int32_t top = 0, left = 0, bottom = 0, right = 0;
    std::vector<ChanOut> ch;
    std::string blend = "norm";
    uint8_t opacity = 255, clipping = 0, flags = 0;
    std::vector<uint8_t> extra;
};

const ForeignBlock* own(const Node& n, const std::string& key) { return n.foreign.find("psd.own", key); }

void put_block(ByteWriter& w, const std::string& sig, const std::string& key, const std::vector<uint8_t>& data,
               bool psb, int pad) {
    w.str(sig);
    w.str(key);
    if (psb && detail::is_big_key(key))
        w.be64(data.size());
    else
        w.be32(static_cast<uint32_t>(data.size()));
    w.raw(data);
    if (pad > 1)
        for (size_t k = data.size(); k % static_cast<size_t>(pad); ++k) w.u8(0);
}

ChanOut empty_channel(int16_t id) {
    ChanOut c;
    c.id = id;
    c.data = {0, 0};  // raw, no samples
    return c;
}

// RLE-compressed channel (compression 1): row byte counts, then the PackBits rows.
ChanOut rle_channel(int16_t id, const std::vector<uint8_t>& plane, int w, int h, bool psb) {
    if (w <= 0 || h <= 0) return empty_channel(id);
    ChanOut c;
    c.id = id;
    std::vector<uint8_t> rows;
    std::vector<uint32_t> counts(static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) {
        const size_t before = rows.size();
        packbits_encode(plane.data() + (static_cast<size_t>(y) * static_cast<size_t>(w)), static_cast<size_t>(w), rows);
        counts[static_cast<size_t>(y)] = static_cast<uint32_t>(rows.size() - before);
    }
    ByteWriter bw;
    bw.be16(1);
    for (uint32_t k : counts) {
        if (psb)
            bw.be32(k);
        else
            bw.be16(static_cast<uint16_t>(k));
    }
    bw.raw(rows);
    c.data = std::move(bw.buf);
    return c;
}

std::vector<uint8_t> default_ranges() {
    std::vector<uint8_t> r;
    for (int i = 0; i < 10; ++i) r.insert(r.end(), {0x00, 0x00, 0xFF, 0xFF});
    return r;
}

std::vector<uint8_t> luni_data(const std::string& name) {
    ByteWriter w;
    const std::vector<uint16_t> u = detail::utf8_to_utf16(name);
    w.be32(static_cast<uint32_t>(u.size()));
    for (uint16_t c : u) w.be16(c);
    w.pad_to(4);
    return w.buf;
}

std::vector<uint8_t> u32_block(uint32_t v) {
    ByteWriter w;
    w.be32(v);
    return w.buf;
}

struct Writer {
    const DocState& s;
    bool psb;
    std::vector<std::string>& warnings;
    std::vector<RecOut> recs;

    // Pascal name, mask, blending ranges and the owned + foreign tagged blocks of a record.
    void finish_extra(RecOut& r, const Node& n, const std::vector<uint8_t>& mask_data,
                      const std::vector<std::pair<std::string, std::vector<uint8_t>>>& owned_blocks,
                      const std::string& foreign_ns_prefix) {
        ByteWriter x;
        x.be32(static_cast<uint32_t>(mask_data.size()));
        x.raw(mask_data);
        const ForeignBlock* rg = own(n, foreign_ns_prefix + "ranges");
        const std::vector<uint8_t> ranges = rg ? rg->bytes() : default_ranges();
        x.be32(static_cast<uint32_t>(ranges.size()));
        x.raw(ranges);
        // Pascal name: the one read from the file when the name is unchanged, else Latin-1.
        std::string pname;
        const ForeignBlock* pn = own(n, foreign_ns_prefix + "pname");
        const std::string& uname = n.name.empty() ? n.id : n.name;
        if (pn && detail::latin1_to_utf8(std::string(pn->bytes().begin(), pn->bytes().end())) == uname)
            pname.assign(pn->bytes().begin(), pn->bytes().end());
        else
            pname = detail::utf8_to_latin1(uname);
        if (pname.size() > 255) pname.resize(255);
        x.u8(static_cast<uint8_t>(pname.size()));
        x.str(pname);
        for (size_t k = 1 + pname.size(); k % 4; ++k) x.u8(0);
        for (const auto& [key, data] : owned_blocks) put_block(x, "8BIM", key, data, psb, 1);
        r.extra = std::move(x.buf);
    }

    void append_foreign(ByteWriter& x, const Node& n) {
        for (const ForeignBlock& b : n.foreign.blocks) {
            if (b.ns == "psd.tb") put_block(x, "8BIM", b.key, b.bytes(), psb, 1);
            if (b.ns == "psd.tb64") put_block(x, "8B64", b.key, b.bytes(), psb, 1);
        }
    }

    // Mask data record + channels -2 (and the kept raw -3).
    std::vector<uint8_t> mask_part(const Node& n, RecOut& r) {
        if (!n.mask) return {};
        const GrayImage& plane = n.mask->plane;
        const uint8_t def = plane.background();
        io::IRect bb = io::content_bbox(plane);
        ByteWriter m;
        m.bei32(bb.empty() ? 0 : bb.y);
        m.bei32(bb.empty() ? 0 : bb.x);
        m.bei32(bb.empty() ? 0 : bb.y + bb.h);
        m.bei32(bb.empty() ? 0 : bb.x + bb.w);
        m.u8(def);
        uint8_t flags = 0;
        if (const ForeignBlock* f = own(n, "mask.flags"); f && f->size() == 1) flags = f->bytes()[0];
        flags = static_cast<uint8_t>((flags & ~2u) | (n.mask->enabled ? 0u : 2u));
        m.u8(flags);
        const ForeignBlock* tail = own(n, "mask.tail");
        if (tail)
            m.raw(tail->bytes());
        else
            m.zeros(2);
        if (bb.empty()) {
            r.ch.push_back(empty_channel(-2));
        } else {
            r.ch.push_back(rle_channel(-2, io::read_region(plane, bb), bb.w, bb.h, psb));
        }
        if (const ForeignBlock* real = own(n, "mask.real")) {
            ChanOut c;
            c.id = -3;
            c.data = real->bytes();
            if (c.data.size() < 2) c.data = {0, 0};
            r.ch.push_back(std::move(c));
        }
        return m.buf;
    }

    uint8_t record_flags(const Node& n, bool lock) {
        uint8_t f = 0x08;  // bit 3: Photoshop 5.0+ (bit 4 is meaningful)
        if (const ForeignBlock* fb = own(n, "flags"); fb && fb->size() == 1) f = fb->bytes()[0];
        // Bit 0 (transparency protected): the file's own bit while the lock is unchanged (a file
        // may lock through lspf alone), else the current lock.
        const ForeignBlock* was = own(n, "lock");
        const bool unchanged = was && was->size() == 1 && (was->bytes()[0] != 0) == lock;
        f = static_cast<uint8_t>(f & (unchanged ? ~2u : ~3u));
        if (!unchanged && lock) f |= 1;
        if (!n.visible) f |= 2;
        return f;
    }

    std::vector<std::pair<std::string, std::vector<uint8_t>>> layer_owned(const Node& n) {
        std::vector<std::pair<std::string, std::vector<uint8_t>>> b;
        b.emplace_back("luni", luni_data(n.name.empty() ? n.id : n.name));
        const uint8_t fill = q(n.fill);
        if (fill != 255) b.emplace_back("iOpa", std::vector<uint8_t>{fill, 0, 0, 0});
        const ForeignBlock* lspf = own(n, "lspf");
        if (lspf || n.lock_alpha) {
            std::vector<uint8_t> v = (lspf && lspf->size() >= 4) ? lspf->bytes() : u32_block(0);
            v[3] = static_cast<uint8_t>((v[3] & ~1u) | (n.lock_alpha ? 1u : 0u));
            b.emplace_back("lspf", std::move(v));
        }
        if (!n.clbl) b.emplace_back("clbl", std::vector<uint8_t>{0, 0, 0, 0});
        return b;
    }

    void emit_layer(const Node& n) {
        RecOut r;
        const bool raster = n.is_raster();
        r.blend = blend_key(n.mode == BlendMode::Pass ? BlendMode::Norm : n.mode);
        r.opacity = q(n.opacity);
        r.clipping = n.clip ? 1 : 0;
        r.flags = record_flags(n, raster && n.lock_alpha);
        std::vector<std::pair<std::string, std::vector<uint8_t>>> owned = layer_owned(n);

        bool has_adj_block = false;
        for (const ForeignBlock& b : n.foreign.blocks)
            if ((b.ns == "psd.tb" || b.ns == "psd.tb64") && detail::is_adjustment_key(b.key)) has_adj_block = true;
        if (n.is_adjustment() && !has_adj_block) {
            std::string key;
            std::vector<uint8_t> data;
            const std::string type = n.adjustment ? n.adjustment->type() : "";
            if (detail::adjustment_to_psd(type, n.adjust_params, key, data)) {
                owned.emplace_back(key, std::move(data));
            } else {
                warnings.push_back("PSD: adjustment layer '" + (n.name.empty() ? n.id : n.name) + "' (" + type +
                                   ") cannot be expressed in PSD; written as an empty layer");
            }
        }

        if (raster) {
            const io::IRect bb = io::content_bbox(n.pixels);
            if (!bb.empty()) {
                r.top = bb.y;
                r.left = bb.x;
                r.bottom = bb.y + bb.h;
                r.right = bb.x + bb.w;
                const std::vector<Rgba8> px = io::read_region(n.pixels, bb);
                std::vector<uint8_t> plane(px.size());
                const int16_t ids[4] = {-1, 0, 1, 2};
                for (int16_t id : ids) {
                    for (size_t i = 0; i < px.size(); ++i)
                        plane[i] = id == -1 ? px[i].a : id == 0 ? px[i].r : id == 1 ? px[i].g : px[i].b;
                    r.ch.push_back(rle_channel(id, plane, bb.w, bb.h, psb));
                }
            }
        }
        if (r.ch.empty())
            for (int16_t id : {-1, 0, 1, 2}) r.ch.push_back(empty_channel(id));
        const std::vector<uint8_t> mask = mask_part(n, r);
        finish_extra(r, n, mask, owned, "");
        ByteWriter x;
        x.buf = std::move(r.extra);
        append_foreign(x, n);
        r.extra = std::move(x.buf);
        recs.push_back(std::move(r));
    }

    void emit_group(const Node& g) {
        // Closing divider (bottom).
        {
            RecOut d;
            for (int16_t id : {-1, 0, 1, 2}) d.ch.push_back(empty_channel(id));
            d.flags = 0x18;
            if (const ForeignBlock* f = own(g, "div.flags"); f && f->size() == 1) d.flags = f->bytes()[0];
            ByteWriter x;
            x.be32(0);
            const std::vector<uint8_t> ranges = default_ranges();
            x.be32(static_cast<uint32_t>(ranges.size()));
            x.raw(ranges);
            std::string pname = "</Layer group>";
            if (const ForeignBlock* p = own(g, "div.pname")) pname.assign(p->bytes().begin(), p->bytes().end());
            x.u8(static_cast<uint8_t>(pname.size()));
            x.str(pname);
            for (size_t k = 1 + pname.size(); k % 4; ++k) x.u8(0);
            const ForeignBlock* lu = own(g, "div.luni");
            put_block(x, "8BIM", "luni", lu ? lu->bytes() : luni_data("</Layer group>"), psb, 1);
            ByteWriter ls;
            ls.be32(3);
            put_block(x, "8BIM", "lsct", ls.buf, psb, 1);
            for (const ForeignBlock& b : g.foreign.blocks) {
                if (b.ns != "psd.div" || b.size() < 4) continue;
                const std::string sig(b.bytes().begin(), b.bytes().begin() + 4);
                put_block(x, sig, b.key, std::vector<uint8_t>(b.bytes().begin() + 4, b.bytes().end()), psb, 1);
            }
            d.extra = std::move(x.buf);
            recs.push_back(std::move(d));
        }
        for (const Node& c : g.children) emit(c);
        // Group record (top).
        RecOut r;
        const std::string key = blend_key(g.mode);
        r.blend = key;
        r.opacity = q(g.opacity);
        r.flags = record_flags(g, false);
        for (int16_t id : {-1, 0, 1, 2}) r.ch.push_back(empty_channel(id));
        const std::vector<uint8_t> mask = mask_part(g, r);
        std::vector<std::pair<std::string, std::vector<uint8_t>>> owned;
        owned.emplace_back("luni", luni_data(g.name.empty() ? g.id : g.name));
        ByteWriter ls;
        uint32_t type = 1;
        bool subtype = false;
        uint32_t sub = 0;
        if (const ForeignBlock* raw = own(g, "lsct"); raw && raw->size() >= 4) {
            const auto& b = raw->bytes();
            const uint32_t t = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
                               (static_cast<uint32_t>(b[2]) << 8) | b[3];
            if (t == 1 || t == 2) type = t;
            if (b.size() >= 16) {
                subtype = true;
                sub = (static_cast<uint32_t>(b[12]) << 24) | (static_cast<uint32_t>(b[13]) << 16) |
                      (static_cast<uint32_t>(b[14]) << 8) | b[15];
            }
        }
        ls.be32(type);
        ls.str("8BIM");
        ls.str(key);
        if (subtype) ls.be32(sub);
        owned.emplace_back("lsct", ls.buf);
        finish_extra(r, g, mask, owned, "");
        ByteWriter x;
        x.buf = std::move(r.extra);
        append_foreign(x, g);
        r.extra = std::move(x.buf);
        recs.push_back(std::move(r));
    }

    void emit(const Node& n) {
        if (n.is_group())
            emit_group(n);
        else
            emit_layer(n);
    }

    std::vector<uint8_t> layer_info() {
        ByteWriter w;
        if (recs.empty()) return {};
        if (recs.size() > 32767) throw IoError("PSD: more than 32767 layer records (groups count twice)");
        w.bei16(static_cast<int16_t>(-static_cast<int>(recs.size())));  // negative: merged alpha = transparency
        for (const RecOut& r : recs) {
            w.bei32(r.top);
            w.bei32(r.left);
            w.bei32(r.bottom);
            w.bei32(r.right);
            w.be16(static_cast<uint16_t>(r.ch.size()));
            for (const ChanOut& c : r.ch) {
                w.bei16(c.id);
                if (psb)
                    w.be64(c.data.size());
                else
                    w.be32(static_cast<uint32_t>(c.data.size()));
            }
            w.str("8BIM");
            w.str(r.blend);
            w.u8(r.opacity);
            w.u8(r.clipping);
            w.u8(r.flags);
            w.u8(0);
            w.be32(static_cast<uint32_t>(r.extra.size()));
            w.raw(r.extra);
        }
        for (const RecOut& r : recs)
            for (const ChanOut& c : r.ch) w.raw(c.data);
        w.pad_to(4);
        return w.buf;
    }
};

std::vector<uint8_t> irb(uint16_t id, const std::vector<uint8_t>& data) {
    ByteWriter w;
    w.str("8BIM");
    w.be16(id);
    w.be16(0);  // empty pascal name, padded to even
    w.be32(static_cast<uint32_t>(data.size()));
    w.raw(data);
    if (data.size() % 2) w.u8(0);
    return w.buf;
}

}  // namespace

std::vector<uint8_t> write(const DocState& s, const WriteOptions& opt, std::vector<std::string>& warnings) {
    if (s.w < 1 || s.h < 1 || s.w > kPsbMaxSide || s.h > kPsbMaxSide)
        throw IoError("PSD: a document of " + std::to_string(s.w) + "x" + std::to_string(s.h) +
                      " px cannot be written (PSB allows 300000 px per side)");
    const bool psb = opt.force_psb || s.w > kPsdMaxSide || s.h > kPsdMaxSide;
    if (s.bg.a != 0)
        warnings.push_back("PSD: the canvas background colour is not stored in PSD (layers only)");

    Writer wr{s, psb, warnings, {}};
    for (const Node& c : s.root.children) wr.emit(c);
    const std::vector<uint8_t> li = wr.layer_info();
    const int channels = wr.recs.empty() ? 3 : 4;

    // Merged composite (+ thumbnail), streamed band by band.
    const size_t W = static_cast<size_t>(s.w);
    std::vector<std::vector<uint8_t>> rows(static_cast<size_t>(channels));
    std::vector<std::vector<uint32_t>> counts(static_cast<size_t>(channels));
    std::vector<uint8_t> plane(W);
    io::ThumbAccumulator thumb(s.w, s.h, kThumbMax);
    std::vector<Rgba8> matted(W);
    io::for_each_render_row(s, false, [&](int y, const Rgba8* row) {
        for (size_t x = 0; x < W; ++x) matted[x] = io::matte_white(row[x]);
        thumb.add_row(y, matted.data());
        for (int c = 0; c < channels; ++c) {
            for (size_t x = 0; x < W; ++x)
                plane[x] = c == 0 ? matted[x].r : c == 1 ? matted[x].g : c == 2 ? matted[x].b : row[x].a;
            auto& out = rows[static_cast<size_t>(c)];
            const size_t before = out.size();
            packbits_encode(plane.data(), W, out);
            counts[static_cast<size_t>(c)].push_back(static_cast<uint32_t>(out.size() - before));
        }
    });

    ByteWriter w;
    // Header.
    w.str("8BPS");
    w.be16(psb ? 2 : 1);
    w.zeros(6);
    w.be16(static_cast<uint16_t>(channels));
    w.be32(static_cast<uint32_t>(s.h));
    w.be32(static_cast<uint32_t>(s.w));
    w.be16(8);
    w.be16(3);  // RGB
    // Color mode data.
    w.be32(0);
    // Image resources: the kept ones in file order, then the regenerated ones.
    {
        ByteWriter ir;
        bool have_1005 = false;
        for (const ForeignBlock& b : s.foreign.blocks) {
            if (b.ns != "psd.irb") continue;
            if (b.key == "1005") have_1005 = true;
            ir.raw(b.bytes());
        }
        if (!have_1005) {
            ByteWriter r;
            r.be32(72u << 16);  // hRes 72.0 (Fixed 16.16)
            r.be16(1);          // pixels per inch
            r.be16(1);          // width unit: inches
            r.be32(72u << 16);
            r.be16(1);
            r.be16(1);
            ir.raw(irb(1005, r.buf));
        }
        {
            const io::RgbaBuffer t = thumb.finish();
            const std::vector<uint8_t> jpg = io::encode_jpeg(t, 80);
            ByteWriter r;
            r.be32(1);  // kJpegRGB
            r.be32(static_cast<uint32_t>(t.w));
            r.be32(static_cast<uint32_t>(t.h));
            const uint32_t wb = ((static_cast<uint32_t>(t.w) * 24) + 31) / 32 * 4;
            r.be32(wb);
            r.be32(wb * static_cast<uint32_t>(t.h));
            r.be32(static_cast<uint32_t>(jpg.size()));
            r.be16(24);
            r.be16(1);
            r.raw(jpg);
            ir.raw(irb(1036, r.buf));
        }
        {
            ByteWriter r;
            r.be32(1);
            r.u8(1);  // hasRealMergedData
            for (const char* name : {"Rasterloom", "Rasterloom"}) {
                const std::vector<uint16_t> u = detail::utf8_to_utf16(name);
                r.be32(static_cast<uint32_t>(u.size()));
                for (uint16_t c : u) r.be16(c);
            }
            r.be32(1);
            ir.raw(irb(1057, r.buf));
        }
        w.be32(static_cast<uint32_t>(ir.size()));
        w.raw(ir.buf);
    }
    // Layer and mask information.
    {
        ByteWriter lm;
        if (psb)
            lm.be64(li.size());
        else
            lm.be32(static_cast<uint32_t>(li.size()));
        lm.raw(li);
        const ForeignBlock* glmi = s.foreign.find("psd.glmi", "");
        lm.be32(glmi ? static_cast<uint32_t>(glmi->size()) : 0);
        if (glmi) lm.raw(glmi->bytes());
        for (const ForeignBlock& b : s.foreign.blocks) {
            if (b.ns == "psd.gtb") put_block(lm, "8BIM", b.key, b.bytes(), psb, 4);
            if (b.ns == "psd.gtb64") put_block(lm, "8B64", b.key, b.bytes(), psb, 4);
        }
        if (psb)
            w.be64(lm.size());
        else
            w.be32(static_cast<uint32_t>(lm.size()));
        w.raw(lm.buf);
    }
    // Image data: RLE, all row counts first, then all rows, channel by channel.
    w.be16(1);
    for (int c = 0; c < channels; ++c)
        for (uint32_t k : counts[static_cast<size_t>(c)]) {
            if (psb)
                w.be32(k);
            else
                w.be16(static_cast<uint16_t>(k));
        }
    for (int c = 0; c < channels; ++c) w.raw(rows[static_cast<size_t>(c)]);
    return std::move(w.buf);
}

}  // namespace rl::psd

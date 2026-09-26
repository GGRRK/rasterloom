// SPDX-License-Identifier: GPL-3.0-or-later
// Document codecs: PSD/PSB, OpenRaster (.orp/.ora), PNG/JPEG/TIFF import and export.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "core/adjust/adjustment.hpp"
#include "core/io/atomic_file.hpp"
#include "core/io/bytes.hpp"
#include "core/io/export_png.hpp"
#include "core/io/file_io.hpp"
#include "core/io/io_error.hpp"
#include "core/io/jpeg.hpp"
#include "core/io/ora.hpp"
#include "core/io/tiff.hpp"
#include "core/io/zip.hpp"
#include "core/psd/psd.hpp"
#include "io_test_util.hpp"

using namespace rl;
using rliotest::doc_from_ops;

namespace {

// A document exercising every node property the model has.
DocState rich_doc() {
    DocState s = doc_from_ops(80, 60, R"(
        {"op":"add_layer","id":"bg","fill":"gradient","from":"#102030FF","to":"#F0E0D0FF","dir":"h"},
        {"op":"add_group","id":"g","mode":"pass"},
        {"op":"add_layer","id":"base","parent":"g","fill":"gradient","from":"#FF000000","to":"#FF0000FF","dir":"v","rect":[8,8,50,40]},
        {"op":"add_layer","id":"c1","parent":"g","fill":"noise","seed":5,"rect":[0,0,80,30]},
        {"op":"set_clip","layer":"c1","value":true},
        {"op":"set_blend","layer":"c1","mode":"scrn"},
        {"op":"set_clbl","layer":"base","value":false},
        {"op":"set_fill","layer":"base","value":0.6},
        {"op":"add_mask","layer":"base","fill":"gradient","from":0,"to":255,"dir":"h","rect":[10,0,40,60],"outside":255},
        {"op":"add_group","id":"iso","mode":"mul"},
        {"op":"add_layer","id":"d","parent":"iso","fill":"noise","seed":9,"rect":[30,20,40,30]},
        {"op":"set_blend","layer":"d","mode":"diss","seed":77},
        {"op":"set_opacity","layer":"iso","value":0.8},
        {"op":"add_mask","layer":"iso","fill":"solid","value":128,"rect":[0,0,60,60]},
        {"op":"set_mask_enabled","layer":"iso","value":false},
        {"op":"add_adjustment","id":"inv","type":"invert"},
        {"op":"set_opacity","layer":"inv","value":0.25},
        {"op":"lock_transparency","layer":"bg","value":true},
        {"op":"set_visible","layer":"d","value":false}
    )", "#FFFFFF80");
    s.root.children[0].name = "Hintergrund \xC3\xA4\xE2\x98\xBA";
    s.root.children[0].foreign.add("psd.tb", "lfx2", {1, 2, 3, 4, 5});
    s.foreign.add("psd.irb", "1034", {'8', 'B', 'I', 'M', 0x04, 0x0A, 0, 0, 0, 0, 0, 1, 1, 0});
    s.selection.mask.set(3, 4, 200);
    s.selection.saved = GrayImage(80, 60, 0);
    s.selection.saved->set(70, 50, 9);
    return s;
}

void expect_same_tree(const Node& a, const Node& b, bool exact_props) {
    ASSERT_EQ(a.kind, b.kind) << a.id;
    EXPECT_EQ(a.name.empty() ? a.id : a.name, b.name.empty() ? b.id : b.name);
    EXPECT_EQ(a.visible, b.visible) << a.id;
    EXPECT_EQ(a.mode, b.mode) << a.id;
    if (exact_props) {
        EXPECT_EQ(a.id, b.id);
        EXPECT_EQ(a.opacity, b.opacity) << a.id;
        EXPECT_EQ(a.fill, b.fill) << a.id;
        EXPECT_EQ(a.seed, b.seed) << a.id;
        EXPECT_EQ(a.adjust_params, b.adjust_params) << a.id;
    }
    EXPECT_EQ(a.clip, b.clip) << a.id;
    EXPECT_EQ(a.clbl, b.clbl) << a.id;
    EXPECT_EQ(a.lock_alpha, b.lock_alpha) << a.id;
    ASSERT_EQ(static_cast<bool>(a.mask), static_cast<bool>(b.mask)) << a.id;
    if (a.mask) {
        EXPECT_EQ(a.mask->enabled, b.mask->enabled) << a.id;
        EXPECT_TRUE(a.mask->plane.pixels_equal(b.mask->plane)) << a.id;
    }
    if (a.is_raster()) {
        EXPECT_TRUE(a.pixels.pixels_equal(b.pixels)) << a.id;
    }
    if (a.is_adjustment()) {
        EXPECT_STREQ(a.adjustment->type(), b.adjustment->type());
    }
    ASSERT_EQ(a.children.size(), b.children.size()) << a.id;
    for (size_t i = 0; i < a.children.size(); ++i) expect_same_tree(a.children[i], b.children[i], exact_props);
}

}  // namespace

// ---- OpenRaster ---------------------------------------------------------------------------------

TEST(Ora, OrpRoundTripsTheWholeModelLosslessly) {
    const DocState s = rich_doc();
    std::vector<std::string> w;
    const auto bytes = ora::write(s, w);
    EXPECT_EQ(bytes, ora::write(s, w)) << "writer must be deterministic";
    const DocState r = ora::read(bytes, w);
    EXPECT_TRUE(w.empty()) << w.front();
    EXPECT_EQ(r.w, s.w);
    EXPECT_EQ(r.bg, s.bg);
    expect_same_tree(s.root, r.root, true);
    ASSERT_EQ(r.root.children[0].foreign.blocks.size(), 1u);
    EXPECT_EQ(r.root.children[0].foreign.blocks[0].bytes(), (std::vector<uint8_t>{1, 2, 3, 4, 5}));
    EXPECT_EQ(r.foreign.blocks.size(), 1u);
    EXPECT_TRUE(r.selection.mask.pixels_equal(s.selection.mask));
    ASSERT_TRUE(r.selection.saved);
    EXPECT_TRUE(r.selection.saved->pixels_equal(*s.selection.saved));
    EXPECT_EQ(io::render_document(r).px, io::render_document(s).px);
}

TEST(Ora, StackXmlIsAViewingBaselineAndEditsFallBackToIt) {
    const DocState s = rich_doc();
    std::vector<std::string> w;
    const auto bytes = ora::write(s, w);
    io::ZipReader z(bytes);
    const auto sx = z.read("stack.xml");
    const std::string xml(sx.begin(), sx.end());
    EXPECT_NE(xml.find("isolation=\"auto\""), std::string::npos);          // pass-through group
    EXPECT_NE(xml.find("composite-op=\"svg:multiply\""), std::string::npos);  // isolated mul group
    EXPECT_NE(xml.find("krita:dissolve"), std::string::npos);
    EXPECT_NE(xml.find("rl:clipgroup=\"base\""), std::string::npos);
    EXPECT_NE(xml.find("alpha-preserve=\"true\""), std::string::npos);  // screen clipped layer
    EXPECT_EQ(xml.find("<filter"), std::string::npos);
    EXPECT_EQ(xml.find("\"inv\""), std::string::npos);  // adjustment layers are JSON-only
    // Another program rewrites stack.xml: the hash no longer matches -> generic import + warning.
    io::ZipWriter zw;
    for (const std::string& n : z.names()) {
        auto d = z.read(n);
        if (n == "stack.xml") {
            std::string t(d.begin(), d.end());
            t.insert(t.find("<stack>"), "<!-- edited -->");
            d.assign(t.begin(), t.end());
        }
        zw.add(n, d, n == "mimetype" ? io::ZipWriter::Method::Store : io::ZipWriter::Method::Deflate);
    }
    std::vector<std::string> w2;
    const DocState r = ora::read(zw.finish(), w2);
    ASSERT_FALSE(w2.empty());
    EXPECT_NE(w2[0].find("changed by another program"), std::string::npos);
    EXPECT_EQ(r.root.children.size(), 3u);  // bg, pass group, isolated group; the adjustment is JSON-only
}

TEST(Ora, MissingEntriesAreIoErrors) {
    io::ZipWriter zw;
    const std::string m = "image/openraster";
    zw.add("mimetype", std::vector<uint8_t>(m.begin(), m.end()), io::ZipWriter::Method::Store);
    const std::string st = "<image w=\"4\" h=\"4\"><stack><layer src=\"data/nope.png\"/></stack></image>";
    zw.add("stack.xml", std::vector<uint8_t>(st.begin(), st.end()));
    std::vector<std::string> w;
    EXPECT_THROW(ora::read(zw.finish(), w), io::IoError);
}

// ---- PSD ----------------------------------------------------------------------------------------

TEST(Psd, WriteReadPreservesTreeAndForeignBlocks) {
    DocState s = rich_doc();
    s.bg = Rgba8{};
    std::vector<std::string> w;
    const auto bytes = psd::write(s, {}, w);
    const DocState r = psd::read(bytes, w);
    ASSERT_EQ(r.root.children.size(), s.root.children.size());
    expect_same_tree(s.root, r.root, false);
    // Opacity/fill are bytes in PSD: they come back as q(v)/255.
    EXPECT_DOUBLE_EQ(r.root.children[2].opacity, 204.0 / 255.0);
    EXPECT_DOUBLE_EQ(r.root.children[1].children[0].fill, 153.0 / 255.0);
    EXPECT_STREQ(r.root.children[3].adjustment->type(), "invert");
    const ForeignBlock* lfx = r.root.children[0].foreign.find("psd.tb", "lfx2");
    ASSERT_NE(lfx, nullptr);
    EXPECT_EQ(lfx->bytes(), (std::vector<uint8_t>{1, 2, 3, 4, 5}));
    EXPECT_NE(r.foreign.find("psd.irb", "1034"), nullptr);
    // Re-save is byte-identical.
    std::vector<std::string> w2;
    EXPECT_EQ(psd::write(r, {}, w2), bytes);
}

TEST(Psd, AllBlendModesRoundTripThroughTheirKeys) {
    for (int m = 1; m < kBlendModeCount; ++m) {
        const auto mode = static_cast<BlendMode>(m);
        const std::string key = psd::blend_key(mode);
        EXPECT_EQ(key.size(), 4u);
        EXPECT_EQ(psd::mode_from_key(key), mode) << key;
    }
    EXPECT_EQ(psd::mode_from_key("pass"), BlendMode::Pass);
    EXPECT_FALSE(psd::mode_from_key("mul"));
}

TEST(Psd, MergedImageIsOurRenderWhiteMattedWithAlpha) {
    DocState s = doc_from_ops(33, 17, R"({"op":"add_layer","id":"a","fill":"gradient","from":"#FF000000","to":"#00FF00FF","dir":"h"})");
    std::vector<std::string> w;
    const auto b = psd::write(s, {}, w);
    io::ByteReader r(b);
    r.skip(12);
    EXPECT_EQ(r.be16(), 4);  // channels: RGB + transparency
    r.skip(12);
    r.skip(r.be32());  // colour mode data
    const uint32_t irlen = r.be32();
    const std::string irs(reinterpret_cast<const char*>(r.here()), irlen);
    r.skip(irlen);
    for (int id : {1005, 1036, 1057}) {
        const char k[2] = {static_cast<char>(id >> 8), static_cast<char>(id & 255)};
        EXPECT_NE(irs.find(std::string("8BIM") + std::string(k, 2)), std::string::npos) << id;
    }
    r.skip(r.be32());  // layer and mask info
    EXPECT_EQ(r.be16(), 1);  // RLE
    std::vector<uint32_t> counts(17 * 4);
    for (auto& c : counts) c = r.be16();
    const io::RgbaBuffer ref = io::render_document([&] {
        DocState t = s;
        t.bg = Rgba8{};
        return t;
    }());
    for (int c = 0; c < 4; ++c)
        for (int y = 0; y < 17; ++y) {
            std::vector<uint8_t> row(33);
            const uint32_t n = counts[static_cast<size_t>((c * 17) + y)];
            ASSERT_TRUE(psd::packbits_decode(r.here(), n, row.data(), row.size()));
            r.skip(n);
            for (int x = 0; x < 33; ++x) {
                const Rgba8 p = ref.at(x, y);
                const double a = p.a / 255.0;
                auto m = [a](uint8_t v) {
                    const double yy = std::min(std::max((v / 255.0) * a + (1.0 - a), 0.0), 1.0) * 255.0;
                    return static_cast<uint8_t>(std::round(yy));
                };
                const uint8_t want = c == 0 ? m(p.r) : c == 1 ? m(p.g) : c == 2 ? m(p.b) : p.a;
                ASSERT_EQ(row[static_cast<size_t>(x)], want) << "c=" << c << " x=" << x << " y=" << y;
            }
        }
}

TEST(Psd, PromotesToPsbAboveThirtyThousandAndOnRequest) {
    DocState s = doc_from_ops(10, 10, R"({"op":"add_layer","id":"a","fill":"solid","color":"#336699FF","rect":[1,1,3,3]})");
    std::vector<std::string> w;
    EXPECT_EQ(psd::write(s, {}, w)[5], 1);
    EXPECT_EQ(psd::write(s, psd::WriteOptions{true}, w)[5], 2);
    DocState wide = s;
    wide.w = 30001;
    wide.h = 2;
    wide.root.children[0].pixels.reset(30001, 2);
    wide.root.children[0].pixels.set(30000, 1, Rgba8{1, 2, 3, 255});
    wide.selection = Selection(30001, 2);
    const auto b = psd::write(wide, {}, w);
    EXPECT_EQ(b[5], 2);
    const DocState r = psd::read(b, w);
    EXPECT_EQ(r.w, 30001);
    EXPECT_EQ(r.root.children[0].pixels.get(30000, 1), (Rgba8{1, 2, 3, 255}));
}

TEST(Psd, CmykIsStoredInvertedAndConverted) {
    // Hand-made 2x1 CMYK file, no layers: stored bytes 0xFF = no ink -> white; 0x00 C,M,Y + 0xFF K
    // -> full CMY ink -> black.
    io::ByteWriter w;
    w.str("8BPS");
    w.be16(1);
    w.zeros(6);
    w.be16(4);
    w.be32(1);
    w.be32(2);
    w.be16(8);
    w.be16(4);
    w.be32(0);
    w.be32(0);
    w.be32(0);
    w.be16(0);  // raw
    for (int c = 0; c < 4; ++c) {
        w.u8(0xFF);
        w.u8(c < 3 ? 0x00 : 0xFF);
    }
    std::vector<std::string> warn;
    const DocState s = psd::read(w.buf, warn);
    const Node& n = s.root.children.at(0);
    EXPECT_EQ(n.pixels.get(0, 0), (Rgba8{255, 255, 255, 255}));
    EXPECT_EQ(n.pixels.get(1, 0), (Rgba8{0, 0, 0, 255}));
    ASSERT_FALSE(warn.empty());
}

TEST(Psd, SixteenBitSamplesQuantiseWithTheSharedRule) {
    // Flat 16-bit greyscale file, 4x1, raw: 16 -> 8 bit is q(v / 65535) (00-conventions C2), not
    // v >> 8: 0x00FF -> 1, 0xFF00 -> 254, 0xFFFF -> 255, 0x8080 -> 128.
    io::ByteWriter w;
    w.str("8BPS");
    w.be16(1);
    w.zeros(6);
    w.be16(1);
    w.be32(1);
    w.be32(4);
    w.be16(16);
    w.be16(1);
    w.be32(0);
    w.be32(0);
    w.be32(0);
    w.be16(0);
    for (uint16_t v : {0x00FF, 0xFF00, 0xFFFF, 0x8080}) w.be16(v);
    std::vector<std::string> warn;
    const DocState s = psd::read(w.buf, warn);
    const Node& n = s.root.children.at(0);
    EXPECT_EQ(n.pixels.get(0, 0).r, 1);
    EXPECT_EQ(n.pixels.get(1, 0).g, 254);
    EXPECT_EQ(n.pixels.get(2, 0).b, 255);
    EXPECT_EQ(n.pixels.get(3, 0), (Rgba8{128, 128, 128, 255}));
}

TEST(Psd, TruncatedAndCorruptFilesThrowIoErrorNeverCrash) {
    DocState s = rich_doc();
    std::vector<std::string> w;
    const auto good = psd::write(s, {}, w);
    for (size_t cut = 0; cut < good.size(); cut += 53) {
        std::vector<uint8_t> t(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
        try {
            (void)psd::read(t, w);
        } catch (const io::IoError&) {
        }
    }
    auto bad = good;
    for (size_t i = 30; i < bad.size(); i += 101) bad[i] ^= 0x5A;
    try {
        (void)psd::read(bad, w);
    } catch (const io::IoError&) {
    }
    SUCCEED();
}

TEST(Psd, AuthoredAdjustmentBlocksParseBack) {
    for (const char* ops : {R"({"op":"add_adjustment","id":"a","type":"invert"})"}) {
        DocState s = doc_from_ops(4, 4, ops);
        std::vector<std::string> w;
        const DocState r = psd::read(psd::write(s, {}, w), w);
        ASSERT_EQ(r.root.children.size(), 1u);
        EXPECT_TRUE(r.root.children[0].is_adjustment());
        EXPECT_STREQ(r.root.children[0].adjustment->type(), "invert");
        EXPECT_NE(r.root.children[0].foreign.find("psd.tb", "nvrt"), nullptr);
    }
}

// ---- raster formats and dispatch --------------------------------------------------------------

TEST(Raster, TiffRgbaIsExactAndJpegIsDeterministic) {
    io::RgbaBuffer b;
    b.w = 37;
    b.h = 23;
    b.px.resize(37 * 23);
    for (int y = 0; y < 23; ++y)
        for (int x = 0; x < 37; ++x) b.at(x, y) = Rgba8{uint8_t(x * 7), uint8_t(y * 11), uint8_t(x ^ y), uint8_t(255 - x)};
    const auto t = io::encode_tiff(b);
    EXPECT_EQ(t, io::encode_tiff(b));
    EXPECT_EQ(io::decode_tiff(t).px, b.px);
    const auto j = io::encode_jpeg(b, 90);
    EXPECT_EQ(j, io::encode_jpeg(b, 90));
    const io::RgbaBuffer d = io::decode_jpeg(j);
    ASSERT_EQ(d.w, 37);
    int maxd = 0;
    for (size_t i = 0; i < d.px.size(); ++i) maxd = std::max(maxd, std::abs(int(d.px[i].g) - int(b.px[i].g)));
    EXPECT_LT(maxd, 40);
    EXPECT_EQ(d.px[0].a, 255);
}

TEST(FileIo, EveryFormatSavesAndOpensByExtensionAndContent) {
    const std::string dir = rliotest::scratch_dir("formats");
    const DocState s = rich_doc();
    const io::RgbaBuffer ref = io::render_document(s);
    for (const char* ext : {"orp", "ora", "psd", "psb", "png", "tif", "jpg"}) {
        const std::string p = dir + "/doc." + ext;
        io::save_document(s, p);
        io::OpenResult r = io::open_document(p);
        ASSERT_TRUE(r.doc) << ext;
        EXPECT_EQ(r.doc->width(), 80) << ext;
        const io::RgbaBuffer got = io::render_document(r.doc->state());
        const std::string e = ext;
        if (e == "orp" || e == "ora" || e == "png" || e == "tif") {
            EXPECT_EQ(got.px, ref.px) << ext;
        }
    }
    // A misnamed file opens by content.
    std::filesystem::copy_file(dir + "/doc.psd", dir + "/really_psd.png");
    EXPECT_EQ(io::open_document(dir + "/really_psd.png").format, io::FileFormat::Psd);
    EXPECT_THROW(io::save_document(s, dir + "/x.xcf"), io::IoError);
    EXPECT_FALSE(std::filesystem::exists(dir + "/x.xcf"));
    EXPECT_THROW(io::open_document(dir + "/missing.psd"), io::IoError);
}

TEST(FileIo, FailedEncodeDoesNotTruncateExistingFile) {
    const std::string dir = rliotest::scratch_dir("noclobber");
    const std::string p = dir + "/big.jpg";
    io::atomic_write_file(p, {'k', 'e', 'e', 'p'});
    DocState s = doc_from_ops(4, 4, "");
    s.w = 70000;  // beyond JPEG's limit: the encoder throws before anything is written
    s.h = 1;
    s.selection = Selection(70000, 1);
    EXPECT_THROW(io::save_document(s, p), io::IoError);
    std::ifstream in(p, std::ios::binary);
    EXPECT_EQ(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), "keep");
}

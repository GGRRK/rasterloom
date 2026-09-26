// SPDX-License-Identifier: GPL-3.0-or-later
//
// docs/math/60-editing-ops.md §14: the base64 / PNG payload codec, COPY and its crop (§14.4.1: what
// the GUI puts on the clipboard), and the equivalence the GUI relies on: Copy (crop) followed by
// Paste in Place (place_image at the crop origin) gives exactly the layer that layer_via_copy
// makes, for layer and merged sources, hard and soft selections. Mutation hooks 43-45.
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "core/base/error.hpp"
#include "core/base/mutation.hpp"
#include "core/edit/place.hpp"
#include "core/edit/png_payload.hpp"
#include "core/geometry/dense.hpp"
#include "core/io/png.hpp"
#include "core/script/engine.hpp"
#include "../test_util.hpp"

using namespace rl;
using rltest::make_script;

namespace {

constexpr const char* kK =
    R"({"op":"add_layer","id":"k","fill":"gradient","from":"#2040C0FF","to":"#F0C020FF","dir":"h"})";

std::unique_ptr<Document> run(const std::string& ops, int w = 48, int h = 40, const std::string& bg = "#00000000") {
    return script::run_script_text(make_script(w, h, ops, bg)).doc;
}

io::RgbaBuffer pattern(int w, int h, bool with_zero_alpha) {
    io::RgbaBuffer b;
    b.w = w;
    b.h = h;
    b.px.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t a = static_cast<uint8_t>((x * 37 + y * 11) % 256);
            b.at(x, y) = Rgba8{static_cast<uint8_t>(x * 9 + y), static_cast<uint8_t>(255 - x * 3), static_cast<uint8_t>(y * 17),
                               with_zero_alpha ? a : static_cast<uint8_t>(a | 1)};
        }
    return b;
}

std::vector<Rgba8> layer_px(Document& d, const std::string& id) {
    return geom::to_dense(d.require_raster(id, "test").pixels);
}

}  // namespace

TEST(PlaceOps, CoreRegistersTheThreeOpsWithHistory) {
    const auto& r = script::default_registry();
    for (const char* op : {"place_image", "layer_via_copy", "clear"}) {
        const script::OpSpec* s = r.find(op);
        ASSERT_NE(s, nullptr) << op;
        EXPECT_TRUE(s->records_history) << op;
    }
}

TEST(PlacePayload, Base64RoundTripAndStrictness) {
    std::vector<uint8_t> out;
    std::string err;
    for (size_t n = 0; n < 10; ++n) {
        std::vector<uint8_t> v(n);
        for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 71 + 5);
        const std::string s = edit::encode_base64(v.data(), v.size());
        EXPECT_EQ(s.size() % 4, 0u);
        ASSERT_TRUE(edit::decode_base64(s, out, err)) << s << ": " << err;
        EXPECT_EQ(out, v);
    }
    EXPECT_TRUE(edit::decode_base64("QQ==", out, err));
    EXPECT_EQ(out, std::vector<uint8_t>({'A'}));
    EXPECT_TRUE(edit::decode_base64("QR==", out, err));  // unused low bits ignored (§14.1)
    EXPECT_EQ(out, std::vector<uint8_t>({'A'}));
    for (const char* bad : {"QQ=", "Q===", "QQ==QUJD", "QU JD", "QU-D", "QU_D", "=QUJ", "QUJD\n"})
        EXPECT_FALSE(edit::decode_base64(bad, out, err)) << bad;
}

TEST(PlacePayload, CoreEncoderOutputDecodesExactly) {
    const io::RgbaBuffer img = pattern(37, 23, true);
    const io::RgbaBuffer back = edit::decode_payload(edit::encode_payload(img));
    ASSERT_EQ(back.w, img.w);
    ASSERT_EQ(back.h, img.h);
    EXPECT_TRUE(back.px == img.px);  // colour under alpha 0 survives the payload; place_image canonicalises
}

TEST(PlacePayload, DecoderAgreesWithLibpngOnTheSubset) {
    // The core encoder (libpng, adaptive filters) and the doc's decoder must agree byte for byte.
    const io::RgbaBuffer img = pattern(64, 9, false);
    const std::vector<uint8_t> png = io::encode_png(img);
    EXPECT_TRUE(edit::decode_png_payload(png).px == io::decode_png(png).px);
}

TEST(PlaceOps, FloorHalfRoundsTowardNegativeInfinity) {
    EXPECT_EQ(edit::floor_half(44), 22);
    EXPECT_EQ(edit::floor_half(5), 2);
    EXPECT_EQ(edit::floor_half(0), 0);
    EXPECT_EQ(edit::floor_half(-1), -1);
    EXPECT_EQ(edit::floor_half(-3), -2);
    EXPECT_EQ(edit::floor_half(-4), -2);
    EXPECT_EQ(edit::floor_half(-5), -3);
}

TEST(PlaceOps, CropTrimsTransparentBordersAndReportsEmpty) {
    // A 10x6 opaque rect at (7, 5) inside an otherwise empty layer, no selection.
    auto d = run(R"({"op":"add_layer","id":"u","fill":"solid","color":"#FF8000FF","rect":[7,5,10,6]})");
    const auto c = edit::copy_region(d->state(), "u");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->x, 7);
    EXPECT_EQ(c->y, 5);
    EXPECT_EQ(c->pixels.w, 10);
    EXPECT_EQ(c->pixels.h, 6);
    // A selection that only covers transparent pixels copies nothing.
    auto e = run(R"({"op":"add_layer","id":"u","fill":"solid","color":"#FF8000FF","rect":[7,5,10,6]},
                   {"op":"select_rect","x":30,"y":20,"w":5,"h":5})");
    EXPECT_FALSE(edit::copy_region(e->state(), "u").has_value());
    EXPECT_THROW(edit::copy_region(d->state(), "nope"), ScriptError);
}

// Copy (crop) + Paste in Place (place_image at the crop origin) == layer_via_copy, which the
// goldens prove against the reference. This is the GUI's clipboard path.
TEST(PlaceOps, CopyThenPasteInPlaceEqualsLayerViaCopy) {
    const std::string setups[] = {
        // layer source, anti-aliased ellipse (partial coverage)
        std::string(kK) + R"(,{"op":"add_layer","id":"u","fill":"noise","seed":4,"alpha":"random"},
            {"op":"select_ellipse","x":5.5,"y":3,"w":30,"h":22,"antialias":true})",
        // layer source, feathered rectangle, layer partly transparent
        std::string(kK) + R"(,{"op":"add_layer","id":"u","fill":"gradient","from":"#E0302000","to":"#E03020FF","dir":"v","rect":[4,4,36,30]},
            {"op":"select_rect","x":8,"y":6,"w":24,"h":20},{"op":"feather","radius":3})",
        // no selection
        std::string(kK) + R"(,{"op":"add_layer","id":"u","fill":"solid","color":"#10E080C0","rect":[9,9,12,7]})",
    };
    for (const std::string& setup : setups) {
        for (bool merged : {false, true}) {
            auto a = run(setup);
            const auto c = edit::copy_region(a->state(), merged ? std::string() : std::string("u"));
            ASSERT_TRUE(c.has_value());
            script::Json place = {{"op", "place_image"}, {"id", "p"}, {"png", edit::encode_payload(c->pixels)},
                                  {"x", c->x}, {"y", c->y}};
            script::Json lvc = {{"op", "layer_via_copy"}, {"id", "p"}};
            if (merged) lvc["merged"] = true;
            else lvc["layer"] = "u";
            auto via_place = run(setup + "," + place.dump());
            auto via_lvc = run(setup + "," + lvc.dump());
            EXPECT_TRUE(layer_px(*via_place, "p") == layer_px(*via_lvc, "p")) << setup << " merged=" << merged;
        }
    }
}

TEST(PlaceOps, CutClearsTheSourceInTheSameRecord) {
    const std::string setup = std::string(kK) + R"(,{"op":"add_layer","id":"u","fill":"noise","seed":2,"alpha":"random"},
        {"op":"select_rect","x":4,"y":4,"w":20,"h":10})";
    auto d = run(setup + R"(,{"op":"layer_via_copy","layer":"u","id":"c","cut":true})");
    auto clr = run(setup + R"(,{"op":"clear","layer":"u"})");
    EXPECT_TRUE(layer_px(*d, "u") == layer_px(*clr, "u"));
    EXPECT_EQ(d->history_size(), 4u);  // k, u, select_rect, layer_via_copy
}

TEST(PlaceOps, MutationHooksChangeTheirFormula) {
    // 43: centring of an image 3 px wider than the canvas.
    io::RgbaBuffer img = pattern(51, 43, false);
    const std::string op = script::Json{{"op", "place_image"}, {"id", "p"}, {"png", edit::encode_payload(img)}}.dump();
    auto good = run(op);
    {
        mut::ScopedMutations m({43});
        auto bad = run(op);
        EXPECT_FALSE(layer_px(*good, "p") == layer_px(*bad, "p"));
    }
    // 44 / 45: soft selections.
    const std::string soft = std::string(kK) + R"(,{"op":"add_layer","id":"u","fill":"solid","color":"#E03020FF"},
        {"op":"select_ellipse","x":4,"y":4,"w":30,"h":20,"antialias":true})";
    for (int id : {44, 45}) {
        const std::string tail = id == 44 ? R"(,{"op":"layer_via_copy","layer":"u","id":"p"})" : R"(,{"op":"clear","layer":"u"})";
        const std::string target = id == 44 ? "p" : "u";
        auto g = run(soft + tail);
        mut::ScopedMutations m({id});
        auto b = run(soft + tail);
        EXPECT_FALSE(layer_px(*g, target) == layer_px(*b, target)) << id;
    }
}

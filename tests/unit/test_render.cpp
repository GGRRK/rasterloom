// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tree rendering: pass-through vs isolated groups (D4), clip groups with clbl both ways (§6),
// masks, visibility, merges, flatten, undo -- through the render-script engine.
#include <gtest/gtest.h>

#include "core/base/error.hpp"
#include "core/base/mutation.hpp"
#include "core/base/quant.hpp"
#include "core/base/rng.hpp"
#include "test_util.hpp"

using namespace rl;
using rltest::render_text;
using rltest::same_pixels;
using rltest::make_script;

namespace {
const char* kBackdrop = R"({"op":"add_layer","id":"k","fill":"gradient","from":"#2040C0FF","to":"#F0C020FF"})";
Rgba8 px(const io::RgbaBuffer& b, int x, int y) { return b.at(x, y); }
}  // namespace

// ---- groups ---------------------------------------------------------------------------------------

TEST(Groups, PassThroughWithFullOpacityEqualsChildrenAtRoot) {
    const std::string kids =
        R"({"op":"add_layer","id":"a","parent":"PARENT","fill":"noise","seed":3,"rect":[5,5,40,30]},
           {"op":"set_blend","layer":"a","mode":"mul"},
           {"op":"add_adjustment","id":"i","parent":"PARENT","type":"invert"},
           {"op":"set_fill","layer":"i","value":0.3},
           {"op":"add_layer","id":"b","parent":"PARENT","fill":"noise","seed":4,"rect":[20,10,40,40]},
           {"op":"set_blend","layer":"b","mode":"sLit"})";
    auto with_parent = [&](const std::string& p) {
        std::string s = kids;
        for (size_t at; (at = s.find("PARENT")) != std::string::npos;) s.replace(at, 6, p);
        return s;
    };
    const auto grouped = render_text(make_script(64, 64, std::string(kBackdrop) +
                                                         R"(,{"op":"add_group","id":"g"},)" + with_parent("g")));
    const auto flat = render_text(make_script(64, 64, std::string(kBackdrop) + "," + with_parent("root")));
    EXPECT_TRUE(same_pixels(grouped, flat));
}

TEST(Groups, PassThroughMultipliesAgainstBackdropIsolatedDoesNot) {
    const std::string base = R"({"op":"add_layer","id":"k","fill":"solid","color":"#808080"},)";
    const std::string child = R"({"op":"add_layer","id":"c","parent":"g","fill":"solid","color":"#8000FF"},
                                 {"op":"set_blend","layer":"c","mode":"mul"})";
    const auto pass = render_text(make_script(8, 8, base + R"({"op":"add_group","id":"g"},)" + child));
    const auto iso = render_text(make_script(8, 8, base + R"({"op":"add_group","id":"g","mode":"isolated"},)" + child));
    // pass: mul against grey 128: 128/255*128/255 -> 64, 0, 128
    EXPECT_EQ(px(pass, 3, 3), (Rgba8{q(dec(128) * dec(128)), 0, q(dec(128) * dec(255)), 255}));
    // isolated: mul onto transparency leaves the child colour, then Normal
    EXPECT_EQ(px(iso, 3, 3), (Rgba8{128, 0, 255, 255}));
    mut::ScopedMutations m({4});  // pass-through forced isolated
    const auto pass4 = render_text(make_script(8, 8, base + R"({"op":"add_group","id":"g"},)" + child));
    EXPECT_EQ(px(pass4, 3, 3), (Rgba8{128, 0, 255, 255}));
}

TEST(Groups, AdjustmentInsidePassThroughReachesBelowTheGroup) {
    const std::string base = R"({"op":"add_layer","id":"k","fill":"solid","color":"#C80A0A"},)";
    const auto pass = render_text(make_script(
        8, 8, base + R"({"op":"add_group","id":"g"},{"op":"add_adjustment","id":"i","parent":"g","type":"invert"})"));
    EXPECT_EQ(px(pass, 0, 0), (Rgba8{55, 245, 245, 255}));
    const auto iso = render_text(make_script(
        8, 8,
        base + R"({"op":"add_group","id":"g","mode":"isolated"},{"op":"add_adjustment","id":"i","parent":"g","type":"invert"})"));
    EXPECT_EQ(px(iso, 0, 0), (Rgba8{200, 10, 10, 255}));  // T is transparent: nothing to recolour
}

TEST(Groups, PassThroughOpacityIsOneLerpNotPerChild) {
    const std::string ops = R"({"op":"add_layer","id":"k","fill":"solid","color":"#000000"},
        {"op":"add_group","id":"g"},
        {"op":"add_layer","id":"a","parent":"g","fill":"solid","color":"#FF0000"},
        {"op":"add_layer","id":"b","parent":"g","fill":"solid","color":"#0000FF"},
        {"op":"set_opacity","layer":"g","value":0.5})";
    // One lerp: R = blue; w = 0.5 -> (0, 0, 128)
    EXPECT_EQ(px(render_text(make_script(8, 8, ops)), 1, 1), (Rgba8{0, 0, 128, 255}));
    mut::ScopedMutations m({19});  // per child: red at 0.5 then blue at 0.5 -> (64, 0, 128)
    EXPECT_EQ(px(render_text(make_script(8, 8, ops)), 1, 1), (Rgba8{64, 0, 128, 255}));
}

TEST(Groups, PassThroughLerpIsPremultiplied) {
    // Transparent pre-group backdrop, group opacity 0.5 with an opaque child: a straight lerp would
    // bleed the canonical black; the premultiplied lerp keeps the child colour at alpha 128.
    const std::string ops = R"({"op":"add_group","id":"g"},
        {"op":"add_layer","id":"a","parent":"g","fill":"solid","color":"#40C080"},
        {"op":"set_opacity","layer":"g","value":0.5})";
    EXPECT_EQ(px(render_text(make_script(8, 8, ops)), 2, 2), (Rgba8{0x40, 0xC0, 0x80, 128}));
}

TEST(Groups, HiddenAndEmptyGroupsLeaveBackdropExactly) {
    const std::string ops = std::string(kBackdrop) + R"(,
        {"op":"add_group","id":"e1"},
        {"op":"add_group","id":"e2","mode":"isolated"},
        {"op":"add_group","id":"h"},
        {"op":"add_layer","id":"hc","parent":"h","fill":"solid","color":"#FF00FF"},
        {"op":"set_visible","layer":"h","value":false},
        {"op":"add_group","id":"v","mode":"scrn"},
        {"op":"add_layer","id":"vc","parent":"v","fill":"solid","color":"#00FF00"},
        {"op":"set_visible","layer":"vc","value":false})";
    EXPECT_TRUE(same_pixels(render_text(make_script(40, 20, ops)), render_text(make_script(40, 20, kBackdrop))));
}

TEST(Groups, NestedGroupsAndMovesRender) {
    const std::string ops = std::string(kBackdrop) + R"(,
        {"op":"add_group","id":"p1"},
        {"op":"add_group","id":"i1","parent":"p1","mode":"mul"},
        {"op":"add_group","id":"p2","parent":"i1"},
        {"op":"add_layer","id":"x","parent":"p2","fill":"noise","seed":1,"rect":[0,0,30,30]},
        {"op":"set_blend","layer":"x","mode":"over"},
        {"op":"set_opacity","layer":"p1","value":0.8},
        {"op":"set_opacity","layer":"i1","value":0.6},
        {"op":"set_opacity","layer":"p2","value":0.5},
        {"op":"move_layer","id":"x","parent":"p1","index":0},
        {"op":"move_layer","id":"x","parent":"p2"})";
    const auto a = render_text(make_script(64, 40, ops));
    const auto b = render_text(make_script(64, 40, ops));
    EXPECT_TRUE(same_pixels(a, b));
    EXPECT_FALSE(same_pixels(a, render_text(make_script(64, 40, kBackdrop))));
}

// ---- clip groups ----------------------------------------------------------------------------------

namespace {
// Base: red with alpha 128, fill F; clipped: opaque green (optionally a mode).
std::string clip_ops(const std::string& base_extra, const std::string& clip_extra = "") {
    return R"({"op":"add_layer","id":"base","fill":"solid","color":"#FF000080"},
              {"op":"add_layer","id":"c","fill":"solid","color":"#00FF00"},
              {"op":"set_clip","layer":"c","value":true})" +
           base_extra + clip_extra;
}
}  // namespace

TEST(Clip, Fill0BaseShowsClippedColourInsideShape) {
    const auto img = render_text(make_script(4, 4, clip_ops(R"(,{"op":"set_fill","layer":"base","value":0})")));
    // G seeded (0,0,0,0); clipped green at c=1 -> opaque green; cg = S * 1 * 1 = 128/255.
    EXPECT_EQ(px(img, 0, 0), (Rgba8{0, 255, 0, 128}));
    {
        mut::ScopedMutations m({6});  // fill as opacity: the whole group vanishes
        EXPECT_EQ(px(render_text(make_script(4, 4, clip_ops(R"(,{"op":"set_fill","layer":"base","value":0})"))), 0, 0),
                  Rgba8{});
    }
    {
        mut::ScopedMutations m({5});  // S applied twice
        EXPECT_NE(px(render_text(make_script(4, 4, clip_ops(R"(,{"op":"set_fill","layer":"base","value":0})"))), 0, 0),
                  (Rgba8{0, 255, 0, 128}));
    }
}

TEST(Clip, BaseOpacity0HidesGroupAndAlphaNeverExceedsShape) {
    EXPECT_EQ(px(render_text(make_script(4, 4, clip_ops(R"(,{"op":"set_opacity","layer":"base","value":0})"))), 1, 1),
              Rgba8{});
    // Random base alpha and clipped noise: result alpha <= q(S * o0) everywhere.
    const std::string ops = R"({"op":"add_layer","id":"base","fill":"noise","seed":2},
        {"op":"add_layer","id":"c","fill":"noise","seed":5,"alpha":255},
        {"op":"set_clip","layer":"c","value":true},
        {"op":"set_opacity","layer":"base","value":0.7})";
    auto res = script::run_script_text(make_script(64, 64, ops));
    const auto img = io::render_document(res.doc->state());
    const Node& base = *res.doc->find("base").node;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) EXPECT_LE(px(img, x, y).a, q(dec(base.pixels.get(x, y).a) * 0.7));
}

TEST(Clip, ClblFalseBlendsClippedLayerOntoBackdrop) {
    const std::string pre = R"({"op":"add_layer","id":"k","fill":"solid","color":"#C8C8C8"},)";
    const std::string ops = pre + clip_ops(R"(,{"op":"set_fill","layer":"base","value":0},
        {"op":"set_blend","layer":"c","mode":"mul"})");
    // clbl = true: mul onto the transparent interior -> green, then Normal at S = 128/255.
    const Rgba8 on = px(render_text(make_script(4, 4, ops)), 0, 0);
    // clbl = false: base contributes nothing (fill 0); green multiplies the grey backdrop at S.
    const std::string off_ops = ops + R"(,{"op":"set_clbl","layer":"base","value":false})";
    const Rgba8 off = px(render_text(make_script(4, 4, off_ops)), 0, 0);
    const double S = dec(128);
    const double cb = dec(200);
    // clbl=false red channel: mul(0.784, 0) = 0 at coverage S over opaque grey.
    EXPECT_EQ(off, (Rgba8{q((1.0 - S) * cb), q((S * (cb * 1.0)) + ((1.0 * cb) * (1.0 - S))), q((1.0 - S) * cb), 255}));
    EXPECT_NE(on, off);
    mut::ScopedMutations m({20});  // clbl ignored
    EXPECT_EQ(px(render_text(make_script(4, 4, off_ops)), 0, 0), on);
}

TEST(Clip, BaseMaskIsPartOfTheShape) {
    const std::string ops = R"({"op":"add_layer","id":"base","fill":"solid","color":"#FF0000"},
        {"op":"add_mask","layer":"base","fill":"solid","value":255,"rect":[0,0,2,4]},
        {"op":"add_layer","id":"c","fill":"solid","color":"#00FF00"},
        {"op":"set_clip","layer":"c","value":true})";
    const auto img = render_text(make_script(4, 4, ops));
    EXPECT_EQ(px(img, 0, 0), (Rgba8{0, 255, 0, 255}));
    EXPECT_EQ(px(img, 3, 0), Rgba8{});  // masked out: outside the clip shape
    mut::ScopedMutations m({23});
    EXPECT_EQ(px(render_text(make_script(4, 4, ops)), 3, 0), (Rgba8{0, 255, 0, 255}));
}

TEST(Clip, HiddenBaseHidesGroupAndInvalidBasesRenderUnclipped) {
    // Hidden base hides the clipped layer too.
    EXPECT_EQ(px(render_text(make_script(4, 4, clip_ops(R"(,{"op":"set_visible","layer":"base","value":false})"))), 0, 0),
              Rgba8{});
    // Clipped layer at index 0: rendered unclipped.
    const auto lone = render_text(make_script(4, 4, R"({"op":"add_layer","id":"c","fill":"solid","color":"#00FF00"},
        {"op":"set_clip","layer":"c","value":true})"));
    EXPECT_EQ(px(lone, 0, 0), (Rgba8{0, 255, 0, 255}));
    // A group followed by a clipped layer: unclipped.
    const auto after_group = render_text(make_script(4, 4, R"({"op":"add_group","id":"g"},
        {"op":"add_layer","id":"c","fill":"solid","color":"#00FF00"},
        {"op":"set_clip","layer":"c","value":true})"));
    EXPECT_EQ(px(after_group, 0, 0), (Rgba8{0, 255, 0, 255}));
    // A hidden clipped layer is skipped; the base alone then takes the LONE path.
    const auto hidden_clip = render_text(make_script(4, 4, clip_ops(R"(,{"op":"set_visible","layer":"c","value":false})")));
    EXPECT_EQ(px(hidden_clip, 0, 0), (Rgba8{255, 0, 0, 128}));
}

TEST(Clip, ClippedAdjustmentActsOnlyInsideShape) {
    const std::string ops = R"({"op":"add_layer","id":"k","fill":"solid","color":"#FFFFFF"},
        {"op":"add_layer","id":"base","fill":"solid","color":"#204060","rect":[0,0,2,4]},
        {"op":"add_adjustment","id":"i","type":"invert"},
        {"op":"set_clip","layer":"i","value":true})";
    const auto img = render_text(make_script(4, 4, ops));
    EXPECT_EQ(px(img, 0, 0), (Rgba8{0xDF, 0xBF, 0x9F, 255}));
    EXPECT_EQ(px(img, 3, 0), (Rgba8{255, 255, 255, 255}));
}

// ---- masks ----------------------------------------------------------------------------------------

TEST(Masks, DisabledEqualsNoMaskAndApplyBakes) {
    const std::string layer = std::string(kBackdrop) +
                              R"(,{"op":"add_layer","id":"u","fill":"noise","seed":8,"alpha":255})";
    const auto plain = render_text(make_script(32, 32, layer));
    const auto disabled = render_text(make_script(32, 32, layer + R"(,{"op":"add_mask","layer":"u","fill":"gradient"},
        {"op":"set_mask_enabled","layer":"u","value":false})"));
    const auto deleted = render_text(make_script(32, 32, layer + R"(,{"op":"add_mask","layer":"u","fill":"gradient"},
        {"op":"delete_mask","layer":"u"})"));
    const auto masked = render_text(make_script(32, 32, layer + R"(,{"op":"add_mask","layer":"u","fill":"gradient"})"));
    EXPECT_TRUE(same_pixels(plain, disabled));
    EXPECT_TRUE(same_pixels(plain, deleted));
    EXPECT_FALSE(same_pixels(plain, masked));

    auto res = script::run_script_text(make_script(32, 32, R"({"op":"add_layer","id":"u","fill":"solid","color":"#FF000080"},
        {"op":"add_mask","layer":"u","fill":"gradient","from":0,"to":255},
        {"op":"set_mask_enabled","layer":"u","value":false},
        {"op":"apply_mask","layer":"u"})"));
    const Node& u = *res.doc->find("u").node;
    EXPECT_FALSE(u.mask.has_value());
    for (int x = 0; x < 32; ++x) {
        const uint8_t m = q(dec(0) + ((static_cast<double>(x) / 31.0) * (dec(255) - dec(0))));
        const uint8_t a = q(dec(128) * dec(m));
        EXPECT_EQ(u.pixels.get(x, 0), canonicalize(Rgba8{255, 0, 0, a})) << x;
    }
}

TEST(Masks, OutsideValueAndGroupMask) {
    auto res = script::run_script_text(make_script(100, 100, R"({"op":"add_group","id":"g","mode":"isolated"},
        {"op":"add_mask","layer":"g","fill":"solid","value":255,"rect":[20,20,60,60],"outside":64})"));
    const Node& g = *res.doc->find("g").node;
    EXPECT_EQ(g.mask->plane.get(0, 0), 64);
    EXPECT_EQ(g.mask->plane.get(20, 20), 255);
    EXPECT_EQ(g.mask->plane.get(80, 80), 64);
}

// ---- fills ----------------------------------------------------------------------------------------

TEST(Fills, GradientEndpointsAndCheckerAndNoise) {
    auto res = script::run_script_text(make_script(256, 4, R"({"op":"add_layer","id":"g","fill":"gradient"},
        {"op":"add_layer","id":"one","fill":"gradient","from":"#102030","to":"#FFFFFF","rect":[5,0,1,4]},
        {"op":"add_layer","id":"c","fill":"checker","cell":3,"a":"#FF0000","b":"#0000FF00"},
        {"op":"add_layer","id":"n","fill":"noise","seed":42,"alpha":"random"})"));
    const Document& d = *res.doc;
    Document& dm = *res.doc;
    for (int x = 0; x < 256; ++x) EXPECT_EQ(dm.find("g").node->pixels.get(x, 1), (Rgba8{uint8_t(x), uint8_t(x), uint8_t(x), 255}));
    EXPECT_EQ(dm.find("one").node->pixels.get(5, 2), (Rgba8{0x10, 0x20, 0x30, 255}));  // rw == 1 -> t = 0
    EXPECT_EQ(dm.find("one").node->pixels.get(6, 2), Rgba8{});
    const RgbaImage& c = dm.find("c").node->pixels;
    EXPECT_EQ(c.get(0, 0), (Rgba8{255, 0, 0, 255}));
    EXPECT_EQ(c.get(2, 2), (Rgba8{255, 0, 0, 255}));
    EXPECT_EQ(c.get(3, 0), Rgba8{});  // colour b has alpha 0 -> canonical
    EXPECT_EQ(c.get(3, 3), (Rgba8{255, 0, 0, 255}));
    const Rgba8 n = dm.find("n").node->pixels.get(17, 3);
    const Rgba8 expect{uint8_t(pixel_hash(42, 17, 3, 0) >> 56), uint8_t(pixel_hash(42, 17, 3, 1) >> 56),
                       uint8_t(pixel_hash(42, 17, 3, 2) >> 56), uint8_t(pixel_hash(42, 17, 3, 3) >> 56)};
    EXPECT_EQ(n, canonicalize(expect));
    (void)d;
}

// ---- merges, flatten, undo ------------------------------------------------------------------------

TEST(Merge, FlattenRendersIdenticallyAndResetsBg) {
    const std::string ops = std::string(kBackdrop) + R"(,
        {"op":"add_layer","id":"s","fill":"noise","seed":9,"rect":[3,3,50,20]},
        {"op":"set_blend","layer":"s","mode":"scrn"})";
    const auto before = render_text(make_script(60, 30, ops, "#FF000080"));
    auto res = script::run_script_text(make_script(60, 30, ops + R"(,{"op":"flatten"})", "#FF000080"));
    EXPECT_EQ(res.doc->bg(), Rgba8{});
    ASSERT_EQ(res.doc->root().children.size(), 1u);
    EXPECT_EQ(res.doc->root().children[0].id, "flattened");
    EXPECT_TRUE(same_pixels(before, io::render_document(res.doc->state())));
}

TEST(Merge, MergeVisibleKeepsHiddenInPlaceAndIgnoresBg) {
    auto res = script::run_script_text(make_script(16, 16, R"({"op":"add_layer","id":"h0","fill":"solid","color":"#FFFFFF"},
        {"op":"set_visible","layer":"h0","value":false},
        {"op":"add_layer","id":"a","fill":"solid","color":"#FF000080"},
        {"op":"add_layer","id":"h1","fill":"solid","color":"#00FF00"},
        {"op":"set_visible","layer":"h1","value":false},
        {"op":"add_layer","id":"b","fill":"solid","color":"#0000FF40","rect":[0,0,4,4]},
        {"op":"merge_visible"})", "#808080FF"));
    const auto& ch = res.doc->root().children;
    ASSERT_EQ(ch.size(), 3u);
    EXPECT_EQ(ch[0].id, "h0");
    EXPECT_EQ(ch[1].id, "merged");
    EXPECT_EQ(ch[2].id, "h1");
    EXPECT_EQ(ch[1].pixels.get(10, 10), (Rgba8{255, 0, 0, 128}));  // rendered onto transparency
}

TEST(Merge, MergeDownMatchesRenderOverTransparency) {
    const std::string ops = R"({"op":"add_layer","id":"l","fill":"noise","seed":1},
        {"op":"add_layer","id":"u","fill":"noise","seed":2},
        {"op":"set_blend","layer":"u","mode":"mul"},
        {"op":"set_opacity","layer":"u","value":0.5},
        {"op":"add_mask","layer":"u","fill":"noise","seed":3})";
    const auto before = render_text(make_script(70, 70, ops));
    auto res = script::run_script_text(make_script(70, 70, ops + R"(,{"op":"merge_down","layer":"u"})"));
    ASSERT_EQ(res.doc->root().children.size(), 1u);
    EXPECT_TRUE(same_pixels(before, io::render_document(res.doc->state())));
}

TEST(Merge, MergeDownPreconditions) {
    EXPECT_THROW(render_text(make_script(8, 8, R"({"op":"add_layer","id":"u"},{"op":"merge_down","layer":"u"})")),
                 ScriptError);
    EXPECT_THROW(render_text(make_script(8, 8, R"({"op":"add_group","id":"g"},{"op":"add_layer","id":"u"},
        {"op":"merge_down","layer":"u"})")), ScriptError);
    EXPECT_THROW(render_text(make_script(8, 8, R"({"op":"add_layer","id":"l"},{"op":"add_layer","id":"u"},
        {"op":"set_visible","layer":"l","value":false},{"op":"merge_down","layer":"u"})")), ScriptError);
    EXPECT_THROW(render_text(make_script(8, 8, R"({"op":"add_layer","id":"l"},{"op":"add_layer","id":"u"},
        {"op":"add_layer","id":"c"},{"op":"set_clip","layer":"c","value":true},{"op":"merge_down","layer":"u"})")),
                 ScriptError);
}

TEST(Merge, MergeDownClipPathBakesTheClipGroup) {
    auto res = script::run_script_text(make_script(4, 4, R"({"op":"add_layer","id":"base","fill":"solid","color":"#FF000080"},
        {"op":"set_fill","layer":"base","value":0.5},
        {"op":"add_layer","id":"c","fill":"solid","color":"#00FF00"},
        {"op":"set_clip","layer":"c","value":true},
        {"op":"merge_down","layer":"c"})"));
    const Node& b = *res.doc->find("base").node;
    EXPECT_EQ(b.fill, 1.0);
    // G = seed (255,0,0,q(0.5)=128) then opaque green at c=1 -> (0,255,0,255); alpha q(S * 1).
    EXPECT_EQ(b.pixels.get(1, 1), (Rgba8{0, 255, 0, 128}));
}

TEST(History, UndoOpRestoresPixelsAndStructure) {
    const std::string base = std::string(kBackdrop) + R"(,{"op":"add_layer","id":"u","fill":"noise","seed":4})";
    const auto expect = render_text(make_script(40, 40, base));
    const auto undone = render_text(make_script(40, 40, base + R"(,{"op":"set_blend","layer":"u","mode":"diff"},
        {"op":"add_layer","id":"z","fill":"solid","color":"#123456"},
        {"op":"merge_down","layer":"z"},
        {"op":"undo","steps":3})"));
    EXPECT_TRUE(same_pixels(expect, undone));
    EXPECT_THROW(render_text(make_script(8, 8, R"({"op":"undo"})")), ScriptError);
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Transform and canvas geometry (doc 30 §11-§16) and the GUI helpers (run_op, pointer builders).
#include <gtest/gtest.h>

#include "core/base/error.hpp"
#include "core/base/mutation.hpp"
#include "core/geometry/canvas_ops.hpp"
#include "core/geometry/dense.hpp"
#include "core/geometry/gui_api.hpp"
#include "core/paint/paint.hpp"
#include "core/script/engine.hpp"
#include "core/transform/transform.hpp"

using namespace rl;
using transform::Mat3;

namespace {

Document make_doc(int W, int H) {
    auto res = script::run_script_text(R"({"canvas":{"w":)" + std::to_string(W) + R"(,"h":)" + std::to_string(H) +
                                       R"(,"bg":"#ffffffff"},"ops":[
        {"op":"add_layer","id":"L"},
        {"op":"select_ellipse","x":3.5,"y":2.25,"w":14.5,"h":9.0},
        {"op":"fill_selection","layer":"L","color":"#ff2000ff"},
        {"op":"add_group","id":"G"},
        {"op":"add_layer","id":"C","parent":"G"},
        {"op":"select_rect","x":1,"y":1,"w":5,"h":3},
        {"op":"fill_selection","layer":"C","color":"#2040a0c0"},
        {"op":"add_mask","layer":"C","fill":"gradient","from":10,"to":250},
        {"op":"set_visible","layer":"C","value":false}
    ],"out":"png8"})");
    return std::move(*res.doc);
}

Node& node(Document& d, const char* id) { return *d.find(id).node; }

}  // namespace

TEST(GeometryTransform, KeysKernelExactPoints) {
    EXPECT_EQ(transform::keys(0.0), 1.0);
    EXPECT_EQ(transform::keys(1.0), 0.0);
    EXPECT_EQ(transform::keys(-1.0), 0.0);
    EXPECT_EQ(transform::keys(2.0), 0.0);
    EXPECT_EQ(transform::keys(-2.5), 0.0);
}

TEST(GeometryTransform, InverseOfAffineHasExactLastRow) {
    const Mat3 m = transform::from_params({3.0, -2.0, 1.7, 0.6, 30.0, 10.0, -5.0, 32.0, 32.0});
    auto n = transform::inverse(m);
    ASSERT_TRUE(n);
    EXPECT_EQ((*n)[6], 0.0);
    EXPECT_EQ((*n)[7], 0.0);
    EXPECT_EQ((*n)[8], 1.0);
    EXPECT_FALSE(transform::inverse(Mat3{1, 0, 0, 1, 0, 0, 0, 0, 1}));
    EXPECT_FALSE(transform::inverse(Mat3{1e-7, 0, 0, 0, 1e-7, 0, 0, 0, 1}));
}

TEST(GeometryTransform, IdentityAndIntegerShiftAreExact) {
    Document d = make_doc(24, 16);
    Node& L = node(d, "L");
    const RgbaImage before = L.pixels;
    ASSERT_TRUE(geom::transform_layer(L, transform::kIdentity, transform::Interp::Bicubic));
    EXPECT_TRUE(L.pixels.pixels_equal(before));
    ASSERT_TRUE(geom::transform_layer(L, transform::Tr(2, -1), transform::Interp::Bicubic));
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 24; ++x) EXPECT_EQ(L.pixels.get(x, y), before.get(x - 2, y + 1)) << x << "," << y;
    // Mutation 30 blurs the identity.
    {
        mut::ScopedMutations m({30});
        Node c = node(d, "L");
        c.pixels = before;
        geom::transform_layer(c, transform::kIdentity, transform::Interp::Bicubic);
        EXPECT_FALSE(c.pixels.pixels_equal(before));
    }
    // Mirror through the matrix equals flip_layer.
    Node a = node(d, "L"), b = node(d, "L");
    geom::transform_layer(a, Mat3{-1, 0, 24, 0, 1, 0, 0, 0, 1}, transform::Interp::Bicubic);
    geom::flip_layer(b, true);
    EXPECT_TRUE(a.pixels.pixels_equal(b.pixels));
}

TEST(GeometryTransform, QuadAffineBranchMapsCorners) {
    transform::Quad qd;
    qd.rx = 0;
    qd.ry = 0;
    qd.rw = 64;
    qd.rh = 64;
    qd.q = {{{4, 4}, {52, 10}, {60, 58}, {12, 52}}};
    auto m = transform::from_quad(qd);
    ASSERT_TRUE(m);
    auto p = geom::gui::map_point(*m, 64, 64);
    ASSERT_TRUE(p);
    EXPECT_NEAR(p->x, 60, 1e-9);
    EXPECT_NEAR(p->y, 58, 1e-9);
    qd.q = {{{0, 0}, {10, 0}, {10, 0}, {0, 10}}};
    EXPECT_FALSE(transform::from_quad(qd));
}

TEST(GeometryCanvas, RightAngleRotationsRoundTrip) {
    Document d = make_doc(24, 16);
    const DocState orig = d.state();
    geom::rotate_canvas(d.state(), 90);
    EXPECT_EQ(d.width(), 16);
    EXPECT_EQ(d.height(), 24);
    EXPECT_EQ(node(d, "C").mask->plane.width(), 16);
    // old top-left lands at the new top-right
    EXPECT_EQ(node(d, "L").pixels.get(15, 0), orig.root.children[0].pixels.get(0, 0));
    geom::rotate_canvas(d.state(), 180);
    geom::rotate_canvas(d.state(), -90);  // normalises to the 270 path
    EXPECT_EQ(d.width(), 24);
    geom::rotate_canvas(d.state(), 180);
    EXPECT_TRUE(node(d, "L").pixels.pixels_equal(orig.root.children[0].pixels));
    EXPECT_TRUE(node(d, "C").pixels.pixels_equal(orig.root.children[1].children[0].pixels));
    EXPECT_TRUE(node(d, "C").mask->plane.pixels_equal(orig.root.children[1].children[0].mask->plane));
}

TEST(GeometryCanvas, CanvasOpsClearSelectionAndResizeEverything) {
    Document d = make_doc(24, 16);
    d.state().selection.saved = d.state().selection.mask;
    ASSERT_TRUE(d.selection().active());
    geom::canvas_size(d.state(), 25, 15, geom::Anchor::C);  // ox = 0, oy = floor_div(-1, 2) = -1
    EXPECT_EQ(d.width(), 25);
    EXPECT_FALSE(d.selection().active());
    EXPECT_FALSE(d.selection().saved.has_value());
    EXPECT_EQ(d.selection().mask.width(), 25);
    EXPECT_EQ(node(d, "C").pixels.width(), 25);
    EXPECT_EQ(node(d, "C").mask->plane.height(), 15);
    // Crop past the canvas: transparent pixels, mask 255.
    geom::crop(d.state(), -3, 2, 10, 20);
    EXPECT_EQ(node(d, "L").pixels.get(0, 19), Rgba8{});
    EXPECT_EQ(node(d, "C").mask->plane.get(0, 0), 255);
    geom::image_size(d.state(), 7, 31, transform::Interp::Bicubic);
    EXPECT_EQ(node(d, "C").mask->plane.width(), 7);
    auto sz = geom::rotate_canvas_size(64, 64, 30);
    ASSERT_TRUE(sz);
    EXPECT_EQ(sz->w, 88);
    EXPECT_FALSE(geom::rotate_canvas_size(16384, 16384, 45));
}

TEST(GeometryCanvas, ImageSizeKeepsOpaqueOpaque) {
    auto res = script::run_script_text(
        R"({"canvas":{"w":17,"h":9},"ops":[{"op":"add_layer","id":"L","fill":"gradient","from":"#102030ff","to":"#f0e0d0ff"},{"op":"image_size","w":40,"h":5}],"out":"png8"})");
    const Node& L = *res.doc->find("L").node;
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 40; ++x) EXPECT_EQ(L.pixels.get(x, y).a, 255);
}

TEST(GeometryGui, RunOpRecordsOneHistoryEntryAndRollsBackErrors) {
    Document d = make_doc(24, 16);
    const size_t h0 = d.history_size();
    geom::gui::run_op(d, geom::gui::select_rect_op({2, 2, 5, 5}, select::Mode::New));
    EXPECT_EQ(d.history_size(), h0 + 1);
    const std::vector<uint8_t> before = geom::to_dense(d.selection().mask);
    EXPECT_THROW(geom::gui::run_op(d, geom::gui::expand_op(0)), ScriptError);
    EXPECT_EQ(d.history_size(), h0 + 1);
    EXPECT_EQ(geom::to_dense(d.selection().mask), before);
    geom::gui::run_op(d, geom::gui::transform_matrix_op("L", transform::Tr(1, 0)));
    geom::gui::run_op(d, geom::gui::flip_op(true));
    geom::gui::run_op(d, geom::gui::rotate_canvas_op(-90));
    EXPECT_EQ(d.width(), 16);
    d.undo(3);
    EXPECT_EQ(d.width(), 24);
}

TEST(GeometryGui, PointerBuilders) {
    auto r = geom::gui::marquee_from_drag(10.6, 4.2, 3.1, 9.9, false, false);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->x, 3.0);
    EXPECT_EQ(r->y, 4.0);
    EXPECT_EQ(r->w, 8.0);
    EXPECT_EQ(r->h, 6.0);
    EXPECT_FALSE(geom::gui::marquee_from_drag(5, 5, 5, 9, false, false));
    auto sq = geom::gui::marquee_from_drag(10, 10, 14, 12, true, true);
    ASSERT_TRUE(sq);
    EXPECT_EQ(sq->w, 8.0);
    EXPECT_EQ(sq->h, 8.0);
    EXPECT_EQ(geom::gui::mode_from_modifiers(true, true), select::Mode::Intersect);

    geom::gui::LassoBuilder b;
    for (int i = 0; i < 10000; ++i) b.add(10 + 5 * std::cos(i * 0.001), 10 + 5 * std::sin(i * 0.001));
    b.add(b.points().back().x, b.points().back().y);  // duplicate dropped
    EXPECT_LE(b.points().size(), 4096u);
    EXPECT_TRUE(b.valid());
    Document d = make_doc(24, 16);
    geom::gui::run_op(d, b.op(select::Mode::New));
    geom::gui::run_op(d, geom::gui::select_wand_op("L", 10.7, 6.2, 32, true, true, select::Mode::Add));
    geom::gui::run_op(d, geom::gui::bucket_fill_op("L", 0.5, 0.5, "#00ff00ff"));
    geom::gui::run_op(d, geom::gui::gradient_op("C", true, 3, 3, 9, 9, "#ff0000ff", "#ff000000"));
    EXPECT_TRUE(d.selection().active());
}
